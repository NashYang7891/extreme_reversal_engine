#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <shared_mutex>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <curl/curl.h>          // 必须添加这一行
#include "indicators.h"

using json = nlohmann::json;
std::atomic<bool> keep_running{true};

// ---------- K线结构 ----------
struct KLine {
    double open;
    double high;
    double low;
    double close;
    int64_t timestamp;
};

class KLineManager {
public:
    KLineManager(int interval_sec) : interval_ms_(interval_sec * 1000) { resetCurrent(); }
    void update(double price, int64_t now_ms) {
        if (current_.timestamp == 0) {
            current_.timestamp = (now_ms / interval_ms_) * interval_ms_;
            current_.open = current_.high = current_.low = current_.close = price;
        } else {
            int64_t kline_start = (now_ms / interval_ms_) * interval_ms_;
            if (kline_start > current_.timestamp) {
                completeKLine();
                current_.timestamp = kline_start;
                current_.open = current_.high = current_.low = current_.close = price;
            } else {
                current_.high = std::max(current_.high, price);
                current_.low = std::min(current_.low, price);
                current_.close = price;
            }
        }
    }
    const std::deque<KLine>& getClosedKLines() const { return closed_lines_; }
private:
    void completeKLine() {
        if (current_.timestamp != 0) {
            closed_lines_.push_back(current_);
            while (closed_lines_.size() > 200) closed_lines_.pop_front();
        }
        resetCurrent();
    }
    void resetCurrent() { current_ = KLine{0,0,0,0,0}; }
    const int interval_ms_;
    KLine current_;
    std::deque<KLine> closed_lines_;
};

// ---------- 突破检测器 ----------
class BreakoutDetector {
public:
    static bool checkLong(const std::deque<KLine>& klines) {
        if (klines.size() < 5) return false;
        const KLine& curr = klines.back();
        double max_close = 0;
        for (size_t i = klines.size() - 5; i < klines.size() - 1; ++i)
            max_close = std::max(max_close, klines[i].close);
        bool cond1 = curr.close > max_close;
        bool cond2 = (klines[klines.size()-3].close > klines[klines.size()-3].open) &&
                     (klines[klines.size()-4].close > klines[klines.size()-4].open) &&
                     (klines[klines.size()-5].close > klines[klines.size()-5].open);
        return cond1 && cond2;
    }
    static bool checkShort(const std::deque<KLine>& klines) {
        if (klines.size() < 5) return false;
        const KLine& curr = klines.back();
        double min_close = std::numeric_limits<double>::max();
        for (size_t i = klines.size() - 5; i < klines.size() - 1; ++i)
            min_close = std::min(min_close, klines[i].close);
        bool cond1 = curr.close < min_close;
        bool cond2 = (klines[klines.size()-3].close < klines[klines.size()-3].open) &&
                     (klines[klines.size()-4].close < klines[klines.size()-4].open) &&
                     (klines[klines.size()-5].close < klines[klines.size()-5].open);
        return cond1 && cond2;
    }
};

// ---------- 上下文 ----------
struct SymbolContext {
    Indicators indicators;
    KLineManager kline_mgr{300};   // 改为5分钟（300秒）
    int64_t last_breakout_kline_ts{0};
    // 成交指标字段（从 trade_metrics_pipe 读取）
    std::atomic<int> active_buy_count{0};
    std::atomic<double> large_buy_ratio{0.0};
    std::atomic<double> active_buy_ratio{0.0};
};
std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

// ---------- 价格管道读取线程（真实成交数据）----------
void price_pipe_reader() {
    const char* pipe_path = "/tmp/price_pipe";
    while (keep_running) {
        int fd = open(pipe_path, O_RDONLY);
        if (fd == -1) { sleep(1); continue; }
        char buf[4096];
        std::string leftover;
        while (keep_running) {
            int n = read(fd, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                std::string data = leftover + std::string(buf);
                size_t pos = 0;
                while ((pos = data.find('\n')) != std::string::npos) {
                    std::string line = data.substr(0, pos);
                    data.erase(0, pos+1);
                    try {
                        json j = json::parse(line);
                        std::string sym = j["symbol"];
                        double price = j["price"];
                        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        std::shared_lock lock(contexts_mutex);
                        auto it = contexts.find(sym);
                        if (it != contexts.end()) {
                            it->second.indicators.update(price);
                            it->second.kline_mgr.update(price, now_ms);
                        }
                    } catch (...) {}
                }
                leftover = data;
            } else if (n == 0) break;
        }
        close(fd);
    }
}

// ---------- 成交指标管道读取 ----------
void trade_metrics_reader() {
    const char* pipe_path = "/tmp/trade_metrics_pipe";
    while (keep_running) {
        int fd = open(pipe_path, O_RDONLY);
        if (fd == -1) { sleep(1); continue; }
        char buf[4096];
        std::string leftover;
        while (keep_running) {
            int n = read(fd, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                std::string data = leftover + std::string(buf);
                size_t pos = 0;
                while ((pos = data.find('\n')) != std::string::npos) {
                    std::string line = data.substr(0, pos);
                    data.erase(0, pos+1);
                    try {
                        json j = json::parse(line);
                        if (j.contains("symbol")) {
                            std::string sym = j["symbol"];
                            int active_buy = j.value("active_buy_count", 0);
                            double large_ratio = j.value("large_buy_ratio", 0.0);
                            double buy_ratio = j.value("active_buy_ratio", 0.0);
                            std::shared_lock lock(contexts_mutex);
                            auto it = contexts.find(sym);
                            if (it != contexts.end()) {
                                it->second.active_buy_count = active_buy;
                                it->second.large_buy_ratio = large_ratio;
                                it->second.active_buy_ratio = buy_ratio;
                            }
                        }
                    } catch (...) {}
                }
                leftover = data;
            } else if (n == 0) break;
        }
        close(fd);
    }
}

// ---------- 反馈管道（预留在线学习）----------
void feedback_listener() {
    const char* fifo_path = "/tmp/quant_feedback";
    mkfifo(fifo_path, 0666);
    while (keep_running) {
        int fd = open(fifo_path, O_RDONLY);
        if (fd == -1) { sleep(1); continue; }
        char buf[1024];
        int n = read(fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            try {
                json j = json::parse(buf);
                std::string sym = j.value("symbol", "");
                std::string side = j.value("side", "");
                double pnl = j.value("pnl", 0.0);
                spdlog::info("反馈收到: {} {} 盈亏={:.2f}%", sym, side, pnl);
                // 可在此处扩展在线学习逻辑
            } catch (...) {}
        }
        close(fd);
    }
}

// ---------- 信号检测循环 ----------
void run_detection() {
    while (keep_running) {
        auto start = std::chrono::steady_clock::now();
        {
            std::shared_lock lock(contexts_mutex);
            for (auto& [sym, ctx] : contexts) {
                try {
                    const auto& klines = ctx.kline_mgr.getClosedKLines();
                    if (klines.size() >= 5) {
                        int64_t curr_ts = klines.back().timestamp;
                        if (curr_ts != ctx.last_breakout_kline_ts) {
                            bool long_sig = BreakoutDetector::checkLong(klines);
                            bool short_sig = BreakoutDetector::checkShort(klines);
                            if (long_sig || short_sig) {
                                ctx.last_breakout_kline_ts = curr_ts;
                                json b_msg;
                                b_msg["type"] = "SIGNAL";
                                b_msg["symbol"] = sym;
                                b_msg["side"] = long_sig ? "LONG" : "SHORT";
                                b_msg["price"] = klines.back().close;
                                b_msg["score"] = 100.0;
                                b_msg["timestamp"] = std::time(nullptr);
                                b_msg["current_price"] = ctx.indicators.price();
                                b_msg["source"] = "BREAKOUT";
                                // 简单止损止盈（可调整）
                                double p = b_msg["price"];
                                if (long_sig) {
                                    b_msg["take_profit"] = p * 1.02;
                                    b_msg["stop_loss"] = p * 0.98;
                                } else {
                                    b_msg["take_profit"] = p * 0.98;
                                    b_msg["stop_loss"] = p * 1.02;
                                }
                                std::cout << b_msg.dump() << std::endl;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("检测异常 [{}]: {}", sym, e.what());
                }
            }
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < std::chrono::milliseconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(5) - elapsed);
    }
}

// ---------- 获取监控币种（使用 curl）----------
std::vector<std::string> fetch_top_symbols(double min_vol = 80000000.0) {
    CURL *curl = curl_easy_init();
    std::vector<std::string> result;
    if (curl) {
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/24hr");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            [](void* contents, size_t size, size_t nmemb, std::string* s) -> size_t {
                size_t total = size * nmemb;
                s->append((char*)contents, total);
                return total;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            try {
                auto data = json::parse(response);
                for (auto& item : data) {
                    std::string sym = item["symbol"];
                    if (sym.size()>4 && sym.compare(sym.size()-4,4,"USDT")==0 && sym.find('_')==std::string::npos) {
                        double vol = std::stod(item["quoteVolume"].get<std::string>());
                        if (vol >= min_vol) result.push_back(sym);
                    }
                }
            } catch(...) {}
        }
        curl_easy_cleanup(curl);
    }
    // 如果获取失败（结果为空），返回一个兜底列表
    if (result.empty()) {
        result = {"BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","BNBUSDT"};
    }
    spdlog::info("监控币种数量: {}", result.size());
    return result;
}

// ---------- 主函数 ----------
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_level(spdlog::level::info);
    spdlog::info(">>> 趋势突破引擎 [真实成交数据 + K线突破信号] 启动...");

    auto symbols = fetch_top_symbols(80000000.0);
    {
        std::unique_lock lock(contexts_mutex);
        for (const auto& sym : symbols)
            contexts.emplace(std::piecewise_construct, std::forward_as_tuple(sym), std::forward_as_tuple());
    }
    spdlog::info("引擎启动，监控 {} 个合约", symbols.size());

    std::thread price_thread(price_pipe_reader);
    std::thread metrics_thread(trade_metrics_reader);
    std::thread feedback_thread(feedback_listener);
    std::thread detect_thread(run_detection);
    spdlog::info("✅ 所有线程已启动 (价格管道 + 成交指标 + 反馈 + 突破信号)");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        spdlog::info("💓 保活心跳, 监控 {} 个合约", contexts.size());
    }

    if (price_thread.joinable()) price_thread.join();
    if (metrics_thread.joinable()) metrics_thread.join();
    if (feedback_thread.joinable()) feedback_thread.join();
    if (detect_thread.joinable()) detect_thread.join();
    return 0;
}