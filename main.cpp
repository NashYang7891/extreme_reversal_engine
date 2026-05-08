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
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "orderbook.h"
#include "indicators.h"
#include "ml_optimizer.h"
#include "signal_detector.h"

using json = nlohmann::json;

std::atomic<bool> keep_running{true};

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// 获取币种列表（与Python端保持一致：成交额≥8000万U）
std::vector<std::string> fetch_top_symbols(int top_n = 100, double min_vol = 80000000.0) {
    std::vector<std::pair<std::string, double>> tickers;
    CURL *curl = curl_easy_init();
    if (curl) {
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/24hr");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            try {
                auto data = json::parse(response);
                for (auto& item : data) {
                    std::string sym = item["symbol"];
                    if (sym.size()>4 && sym.compare(sym.size()-4,4,"USDT")==0 &&
                        sym.find('_')==std::string::npos && sym!="USDCUSDT") {
                        double vol = std::stod(item["quoteVolume"].get<std::string>());
                        if (vol >= min_vol) tickers.emplace_back(sym, vol);
                    }
                }
            } catch (...) { spdlog::error("解析 ticker 失败"); }
        }
        curl_easy_cleanup(curl);
    }
    std::sort(tickers.begin(), tickers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> result;
    for (size_t i = 0; i < tickers.size() && i < (size_t)top_n; ++i)
        result.push_back(tickers[i].first);
    spdlog::info("监控 {} 个合约 (24h成交额≥{}万U)", result.size(), min_vol/10000);
    return result;
}

struct SymbolContext {
    OrderBook orderbook;   // 无实际用途
    Indicators indicators;
    MLOptimizer ml{100};
    SignalDetector detector{ml, indicators};
    std::atomic<int64_t> last_active_time{0};
    std::atomic<int64_t> last_b_push_ms{0};
    std::atomic<double> last_active_change{0.0};
};

std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

// 简化后的活跃层：仅依赖价格变化和序列长度，不再需要成交量
bool active_layer(Indicators& ind, double& out_change) {
    if (ind.prices().size() < 60) return false;
    double change_3m = ind.price_change_pct(3*60);
    if (std::abs(change_3m) > 0.20) return false;
    if (std::abs(change_3m) < 0.012) return false;
    out_change = change_3m;
    return true;
}

// 从管道读取成交数据并更新指标
void trade_pipe_reader() {
    const char* pipe_path = "/tmp/trade_pipe";
    while (keep_running) {
        int fd = open(pipe_path, O_RDONLY);
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        char buf[1024];
        while (keep_running) {
            int n = read(fd, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                try {
                    json j = json::parse(buf);
                    std::string sym = j.value("symbol", "");
                    double price = j.value("price", 0.0);
                    if (!sym.empty() && price > 0) {
                        std::shared_lock lock(contexts_mutex);
                        auto it = contexts.find(sym);
                        if (it != contexts.end()) {
                            it->second.indicators.update(price);
                            spdlog::debug("更新 {} 价格: {}", sym, price);
                        }
                    }
                } catch (...) {}
            }
        }
        close(fd);
    }
}

void feedback_listener() {
    const char* fifo_path = "/tmp/quant_feedback";
    mkfifo(fifo_path, 0666);
    while (keep_running) {
        int fd = open(fifo_path, O_RDONLY);
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        char buf[1024];
        int n = read(fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            try {
                json j = json::parse(buf);
                std::string sym = j.value("symbol", "");
                std::string side = j.value("side", "");
                double pnl = j.value("pnl", 0.0);
                if (!sym.empty() && !side.empty()) {
                    std::shared_lock lock(contexts_mutex);
                    auto it = contexts.find(sym);
                    if (it != contexts.end()) {
                        it->second.ml.update_result(side, pnl);
                        spdlog::info("反馈收到: {} {} 盈亏={:.2f}%", sym, side, pnl);
                    }
                }
            } catch (...) {}
        }
        close(fd);
    }
}

void run_detection() {
    while (keep_running) {
        auto start = std::chrono::steady_clock::now();
        {
            std::shared_lock lock(contexts_mutex);
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            for (auto& [sym, ctx] : contexts) {
                if (ctx.indicators.is_stale(60000)) continue;
                try {
                    double change_pct = 0.0;
                    if (active_layer(ctx.indicators, change_pct)) {
                        ctx.last_active_time = now_ms;
                        ctx.last_active_change = change_pct;

                        json a_msg;
                        a_msg["type"] = "A_ACTIVE";
                        a_msg["symbol"] = sym;
                        a_msg["current_price"] = ctx.indicators.price();
                        a_msg["price"] = ctx.indicators.price();
                        a_msg["change_pct"] = change_pct * 100.0;
                        a_msg["timestamp"] = std::time(nullptr);
                        double atr = ctx.indicators.atr();
                        if (atr > 1e-9) {
                            double dev = (ctx.indicators.ema20() - ctx.indicators.price()) / atr;
                            if (std::abs(dev) < 50.0) a_msg["dev"] = dev;
                        }
                        std::cout << a_msg.dump() << std::endl;
                    }

                    int64_t last_active = ctx.last_active_time.load();
                    if (last_active > 0 && (now_ms - last_active) < 15*60*1000) {
                        auto sig = ctx.detector.check(ctx.orderbook);  // orderbook 占位
                        if (sig.valid) {
                            if (sig.side == "LONG" && ctx.last_active_change.load() > -0.025)
                                continue;
                            if (sig.side == "SHORT" && ctx.last_active_change.load() < 0.012)
                                continue;
                            if (now_ms - ctx.last_b_push_ms.load() < 10000) continue;
                            ctx.last_b_push_ms = now_ms;

                            json b_msg;
                            b_msg["type"] = "SIGNAL";
                            b_msg["symbol"] = sym;
                            b_msg["side"]   = sig.side;
                            b_msg["price"]  = sig.price;
                            b_msg["score"]  = sig.score;
                            b_msg["timestamp"] = std::time(nullptr);
                            b_msg["current_price"] = ctx.indicators.price();

                            double atr = ctx.indicators.atr();
                            double p   = sig.price;
                            if (sig.side == "LONG") {
                                double profit_dist = std::max(atr * 8.0, p * 0.025);
                                b_msg["take_profit"] = p + profit_dist;
                                double stop_dist = std::max(atr * 3.0, p * 0.015);
                                if (stop_dist > p * 0.04) stop_dist = p * 0.04;
                                b_msg["stop_loss"] = p - stop_dist;
                                if (b_msg["take_profit"] <= p) b_msg["take_profit"] = p * 1.02;
                                if (b_msg["stop_loss"] < p * 0.95) b_msg["stop_loss"] = p * 0.95;
                            } else {
                                double profit_dist = std::max(atr * 5.0, p * 0.03);
                                b_msg["take_profit"] = p - profit_dist;
                                double stop_dist_short = std::max(atr * 3.0, p * 0.015);
                                if (stop_dist_short > p * 0.04) stop_dist_short = p * 0.04;
                                b_msg["stop_loss"] = p + stop_dist_short;
                                if (b_msg["stop_loss"] > p * 1.08) b_msg["stop_loss"] = p * 1.08;
                                if (b_msg["take_profit"] >= p) b_msg["take_profit"] = p - profit_dist;
                            }
                            std::cout << b_msg.dump() << std::endl;
                            ctx.last_active_time = 0;
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

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_level(spdlog::level::debug);
    spdlog::info(">>> 极端反转引擎 [Python桥接成交数据 + 在线学习] 启动...");

    auto symbols = fetch_top_symbols(100, 80000000.0);
    {
        std::unique_lock lock(contexts_mutex);
        for (const auto& sym : symbols)
            contexts.emplace(std::piecewise_construct, std::forward_as_tuple(sym), std::forward_as_tuple());
    }

    std::thread pipe_thread(trade_pipe_reader);
    std::thread detect_thread(run_detection);
    std::thread feedback_thread(feedback_listener);
    spdlog::info("✅ 所有线程已启动 (Python桥接成交数据)");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        spdlog::info("💓 保活心跳, 监控 {} 个合约", contexts.size());
    }

    if (pipe_thread.joinable()) pipe_thread.join();
    if (detect_thread.joinable()) detect_thread.join();
    if (feedback_thread.joinable()) feedback_thread.join();
    return 0;
}