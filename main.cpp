#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include "indicators.h"

using json = nlohmann::json;

std::atomic<bool> keep_running{true};
std::atomic<uint64_t> price_updates_received{0};
std::atomic<uint64_t> price_updates_applied{0};
std::atomic<uint64_t> price_updates_unknown_symbol{0};
std::atomic<uint64_t> price_bad_messages{0};
std::atomic<uint64_t> a_active_signals{0};
std::atomic<uint64_t> breakout_signals{0};

struct KLine {
    double open;
    double high;
    double low;
    double close;
    int64_t timestamp;
};

class KLineManager {
public:
    explicit KLineManager(int interval_sec) : interval_ms_(interval_sec * 1000) { resetCurrent(); }

    void update(double price, int64_t now_ms) {
        if (current_.timestamp == 0) {
            current_.timestamp = (now_ms / interval_ms_) * interval_ms_;
            current_.open = current_.high = current_.low = current_.close = price;
            return;
        }

        int64_t kline_start = (now_ms / interval_ms_) * interval_ms_;
        if (kline_start > current_.timestamp) {
            completeKLine();
            current_.timestamp = kline_start;
            current_.open = current_.high = current_.low = current_.close = price;
            return;
        }

        current_.high = std::max(current_.high, price);
        current_.low = std::min(current_.low, price);
        current_.close = price;
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

    void resetCurrent() { current_ = KLine{0, 0, 0, 0, 0}; }

    const int interval_ms_;
    KLine current_;
    std::deque<KLine> closed_lines_;
};

class BreakoutDetector {
public:
    static bool checkLong(const std::deque<KLine>& klines) {
        if (klines.size() < 5) return false;

        const KLine& curr = klines.back();
        double max_close = 0.0;
        for (size_t i = klines.size() - 5; i < klines.size() - 1; ++i) {
            max_close = std::max(max_close, klines[i].close);
        }

        bool cond1 = curr.close > max_close;
        bool cond2 = (klines[klines.size() - 3].close > klines[klines.size() - 3].open) &&
                     (klines[klines.size() - 4].close > klines[klines.size() - 4].open) &&
                     (klines[klines.size() - 5].close > klines[klines.size() - 5].open);
        return cond1 && cond2;
    }

    static bool checkShort(const std::deque<KLine>& klines) {
        if (klines.size() < 5) return false;

        const KLine& curr = klines.back();
        double min_close = std::numeric_limits<double>::max();
        for (size_t i = klines.size() - 5; i < klines.size() - 1; ++i) {
            min_close = std::min(min_close, klines[i].close);
        }

        bool cond1 = curr.close < min_close;
        bool cond2 = (klines[klines.size() - 3].close < klines[klines.size() - 3].open) &&
                     (klines[klines.size() - 4].close < klines[klines.size() - 4].open) &&
                     (klines[klines.size() - 5].close < klines[klines.size() - 5].open);
        return cond1 && cond2;
    }
};

struct SymbolContext {
    Indicators indicators;
    KLineManager kline_mgr{300};
    int64_t last_breakout_kline_ts{0};
    double last_a_price{0.0};
    int64_t last_a_time_ms{0};
    std::atomic<int> active_buy_count{0};
    std::atomic<double> large_buy_ratio{0.0};
    std::atomic<double> active_buy_ratio{0.0};
};

std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

bool open_read_fifo(const char* pipe_path, int& fd) {
    mkfifo(pipe_path, 0666);
    fd = open(pipe_path, O_RDONLY);
    if (fd == -1) {
        spdlog::warn("FIFO open failed [{}]: {}", pipe_path, std::strerror(errno));
        sleep(1);
        return false;
    }
    spdlog::info("FIFO reader connected [{}]", pipe_path);
    return true;
}

bool read_fifo_chunk(int fd, const char* pipe_path, char* buf, size_t buf_size, ssize_t& n) {
    n = read(fd, buf, buf_size - 1);
    if (n > 0) {
        buf[n] = '\0';
        return true;
    }

    if (n == 0) {
        spdlog::warn("FIFO writer disconnected [{}], reopening", pipe_path);
        return false;
    }

    if (errno == EINTR) {
        return true;
    }

    spdlog::warn("FIFO read failed [{}]: {}", pipe_path, std::strerror(errno));
    return false;
}

double json_to_double(const json& value, double fallback = 0.0) {
    try {
        if (value.is_number()) return value.get<double>();
        if (value.is_string()) return std::stod(value.get<std::string>());
    } catch (const std::exception&) {
    }
    return fallback;
}

void price_pipe_reader() {
    const char* pipe_path = "/tmp/price_pipe";
    while (keep_running) {
        int fd = -1;
        if (!open_read_fifo(pipe_path, fd)) continue;

        char buf[4096];
        std::string leftover;
        while (keep_running) {
            ssize_t n = 0;
            if (!read_fifo_chunk(fd, pipe_path, buf, sizeof(buf), n)) break;
            if (n <= 0) continue;

            std::string data = leftover + std::string(buf, static_cast<size_t>(n));
            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                data.erase(0, pos + 1);
                if (line.empty()) continue;

                try {
                    json j = json::parse(line);
                    if (!j.contains("symbol") || !j.contains("price")) continue;

                    std::string sym = j.value("symbol", "");
                    double price = json_to_double(j["price"]);
                    if (sym.empty() || price <= 0.0) continue;
                    price_updates_received.fetch_add(1, std::memory_order_relaxed);

                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();

                    std::unique_lock<std::shared_mutex> lock(contexts_mutex);
                    auto it = contexts.find(sym);
                    if (it != contexts.end()) {
                        it->second.indicators.update(price);
                        it->second.kline_mgr.update(price, now_ms);
                        price_updates_applied.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        price_updates_unknown_symbol.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (const std::exception& e) {
                    price_bad_messages.fetch_add(1, std::memory_order_relaxed);
                    spdlog::warn("price_pipe bad message: {}", e.what());
                }
            }

            leftover = data;
            if (leftover.size() > 8192) leftover.clear();
        }

        close(fd);
        sleep(1);
    }
}

void trade_metrics_reader() {
    const char* pipe_path = "/tmp/trade_metrics_pipe";
    while (keep_running) {
        int fd = -1;
        if (!open_read_fifo(pipe_path, fd)) continue;

        char buf[4096];
        std::string leftover;
        while (keep_running) {
            ssize_t n = 0;
            if (!read_fifo_chunk(fd, pipe_path, buf, sizeof(buf), n)) break;
            if (n <= 0) continue;

            std::string data = leftover + std::string(buf, static_cast<size_t>(n));
            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                data.erase(0, pos + 1);
                if (line.empty()) continue;

                try {
                    json j = json::parse(line);
                    std::string sym = j.value("symbol", "");
                    if (sym.empty()) continue;

                    int active_buy = j.value("active_buy_count", 0);
                    double large_ratio = j.value("large_buy_ratio", 0.0);
                    double buy_ratio = j.value("active_buy_ratio", 0.0);

                    std::unique_lock<std::shared_mutex> lock(contexts_mutex);
                    auto it = contexts.find(sym);
                    if (it != contexts.end()) {
                        it->second.active_buy_count = active_buy;
                        it->second.large_buy_ratio = large_ratio;
                        it->second.active_buy_ratio = buy_ratio;
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("trade_metrics bad message: {}", e.what());
                }
            }

            leftover = data;
            if (leftover.size() > 8192) leftover.clear();
        }

        close(fd);
        sleep(1);
    }
}

void feedback_listener() {
    const char* fifo_path = "/tmp/quant_feedback";
    while (keep_running) {
        int fd = -1;
        if (!open_read_fifo(fifo_path, fd)) continue;

        char buf[1024];
        std::string leftover;
        while (keep_running) {
            ssize_t n = 0;
            if (!read_fifo_chunk(fd, fifo_path, buf, sizeof(buf), n)) break;
            if (n <= 0) continue;

            std::string data = leftover + std::string(buf, static_cast<size_t>(n));
            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                data.erase(0, pos + 1);
                if (line.empty()) continue;

                try {
                    json j = json::parse(line);
                    std::string sym = j.value("symbol", "");
                    std::string side = j.value("side", "");
                    double pnl = j.value("pnl", 0.0);
                    spdlog::info("feedback received: {} {} pnl={:.2f}%", sym, side, pnl);
                } catch (const std::exception& e) {
                    spdlog::warn("feedback bad message: {}", e.what());
                }
            }

            leftover = data;
            if (leftover.size() > 8192) leftover.clear();
        }

        close(fd);
        sleep(1);
    }
}

void check_active_layer(SymbolContext& ctx, const std::string& sym, int64_t now_ms) {
    if (ctx.indicators.prices().size() < 20) return;

    double cur_price = ctx.indicators.price();
    double change_3m = ctx.indicators.price_change_pct(3 * 60);
    if (std::abs(change_3m) < 0.005) return;
    if (std::abs(change_3m) > 0.20) return;
    if (now_ms - ctx.last_a_time_ms < 30000) return;

    ctx.last_a_time_ms = now_ms;

    json a_msg;
    a_msg["type"] = "A_ACTIVE";
    a_msg["symbol"] = sym;
    a_msg["price"] = cur_price;
    a_msg["change_pct"] = change_3m * 100.0;
    a_msg["vol_ratio"] = 1.0;
    a_msg["timestamp"] = std::time(nullptr);

    double atr = ctx.indicators.atr();
    double ema20 = ctx.indicators.ema20();
    if (atr > 1e-9) {
        double dev = (ema20 - cur_price) / atr;
        if (std::abs(dev) < 50.0) a_msg["dev"] = dev;
    }

    a_active_signals.fetch_add(1, std::memory_order_relaxed);
    std::cout << a_msg.dump() << std::endl;
}

void run_detection() {
    while (keep_running) {
        auto start = std::chrono::steady_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        {
            std::unique_lock<std::shared_mutex> lock(contexts_mutex);
            for (auto& [sym, ctx] : contexts) {
                try {
                    check_active_layer(ctx, sym, now_ms);

                    const auto& klines = ctx.kline_mgr.getClosedKLines();
                    if (klines.size() < 5) continue;

                    int64_t curr_ts = klines.back().timestamp;
                    if (curr_ts == ctx.last_breakout_kline_ts) continue;

                    bool long_sig = BreakoutDetector::checkLong(klines);
                    bool short_sig = BreakoutDetector::checkShort(klines);
                    if (!long_sig && !short_sig) continue;

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

                    double p = b_msg["price"];
                    if (long_sig) {
                        b_msg["take_profit"] = p * 1.02;
                        b_msg["stop_loss"] = p * 0.98;
                    } else {
                        b_msg["take_profit"] = p * 0.98;
                        b_msg["stop_loss"] = p * 1.02;
                    }

                    std::cout << b_msg.dump() << std::endl;
                    breakout_signals.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::exception& e) {
                    spdlog::warn("detection error [{}]: {}", sym, e.what());
                }
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < std::chrono::milliseconds(10)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10) - elapsed);
        }
    }
}

std::vector<std::string> fetch_top_symbols(double min_vol = 80000000.0) {
    (void)min_vol;
    std::vector<std::string> result = {
        "BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT", "XRPUSDT", "DOGEUSDT",
        "ADAUSDT", "AVAXUSDT", "LINKUSDT", "LTCUSDT", "BCHUSDT", "TRXUSDT",
        "DOTUSDT", "UNIUSDT", "NEARUSDT", "APTUSDT", "ARBUSDT", "OPUSDT",
        "FILUSDT", "ETCUSDT", "SUIUSDT", "WIFUSDT", "PEPEUSDT", "1000SHIBUSDT"
    };
    spdlog::info("monitoring symbols: {}", result.size());
    return result;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_level(spdlog::level::info);
    spdlog::info(">>> Trend breakout engine starting [real trades + A-layer + K-line breakout]");

    auto symbols = fetch_top_symbols(80000000.0);
    {
        std::unique_lock<std::shared_mutex> lock(contexts_mutex);
        for (const auto& sym : symbols) {
            contexts.emplace(std::piecewise_construct, std::forward_as_tuple(sym), std::forward_as_tuple());
        }
    }
    spdlog::info("engine initialized, symbols={}", symbols.size());

    std::thread price_thread(price_pipe_reader);
    std::thread metrics_thread(trade_metrics_reader);
    std::thread feedback_thread(feedback_listener);
    std::thread detect_thread(run_detection);
    spdlog::info("all threads started");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::shared_lock<std::shared_mutex> lock(contexts_mutex);
        spdlog::info(
            "heartbeat, symbols={}, price_received={}, price_applied={}, unknown_symbol={}, bad_price_msg={}, a_active={}, breakout={}",
            contexts.size(),
            price_updates_received.load(std::memory_order_relaxed),
            price_updates_applied.load(std::memory_order_relaxed),
            price_updates_unknown_symbol.load(std::memory_order_relaxed),
            price_bad_messages.load(std::memory_order_relaxed),
            a_active_signals.load(std::memory_order_relaxed),
            breakout_signals.load(std::memory_order_relaxed));
    }

    if (price_thread.joinable()) price_thread.join();
    if (metrics_thread.joinable()) metrics_thread.join();
    if (feedback_thread.joinable()) feedback_thread.join();
    if (detect_thread.joinable()) detect_thread.join();
    return 0;
}
