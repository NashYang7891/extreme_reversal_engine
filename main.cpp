#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <fstream>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <openssl/err.h>

#include "indicators.h"

using json = nlohmann::json;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

std::atomic<bool> keep_running{true};
std::atomic<uint64_t> price_updates_received{0};
std::atomic<uint64_t> price_updates_applied{0};
std::atomic<uint64_t> price_updates_unknown_symbol{0};
std::atomic<uint64_t> price_bad_messages{0};
std::atomic<uint64_t> ws_messages_received{0};
std::atomic<uint64_t> signal_messages_written{0};
std::atomic<uint64_t> signal_write_failures{0};
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
std::mutex signal_pipe_mutex;
int signal_pipe_fd = -1;

bool open_write_fifo(const char* pipe_path, int& fd) {
    mkfifo(pipe_path, 0666);
    fd = open(pipe_path, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        if (errno != ENXIO && errno != ENOENT) {
            spdlog::warn("signal FIFO open failed [{}]: {}", pipe_path, std::strerror(errno));
        }
        return false;
    }
    spdlog::info("signal FIFO writer connected [{}]", pipe_path);
    return true;
}

void close_signal_pipe() {
    if (signal_pipe_fd != -1) {
        close(signal_pipe_fd);
        signal_pipe_fd = -1;
    }
}

double json_to_double(const json& value, double fallback = 0.0) {
    try {
        if (value.is_number()) return value.get<double>();
        if (value.is_string()) return std::stod(value.get<std::string>());
    } catch (const std::exception&) {
    }
    return fallback;
}

bool write_signal_message(const json& message) {
    const char* pipe_path = "/tmp/engine_signals";
    std::string payload = message.dump() + "\n";
    std::lock_guard<std::mutex> lock(signal_pipe_mutex);

    if (signal_pipe_fd == -1 && !open_write_fifo(pipe_path, signal_pipe_fd)) {
        signal_write_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ssize_t written = write(signal_pipe_fd, payload.data(), payload.size());
    if (written == static_cast<ssize_t>(payload.size())) {
        signal_messages_written.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    signal_write_failures.fetch_add(1, std::memory_order_relaxed);
    close_signal_pipe();
    return false;
}

void apply_price_update(const std::string& sym, double price) {
    if (sym.empty() || price <= 0.0) return;
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
}

std::string build_stream_path(const std::vector<std::string>& symbols, const std::string& mode) {
    std::ostringstream path;
    path << "/stream?streams=";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) path << "/";
        std::string lower = symbols[i];
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        path << lower << "@" << mode;
    }
    return path.str();
}

bool handle_market_message(const std::string& payload) {
    try {
        json msg = json::parse(payload);
        json data = msg.value("data", msg);
        if (!data.contains("s") || !data.contains("p")) return false;

        std::string symbol = data.value("s", "");
        double price = json_to_double(data["p"]);
        apply_price_update(symbol, price);
        ws_messages_received.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        price_bad_messages.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("market message parse failed: {}", e.what());
        return false;
    }
}

void run_binance_stream_once(const std::vector<std::string>& symbols, const std::string& mode) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);
    const std::string host = "fstream.binance.com";
    const std::string port = "443";
    const std::string target = build_stream_path(symbols, mode);

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
        throw beast::system_error{ec};
    }

    auto const results = resolver.resolve(host, port);
    net::connect(beast::get_lowest_layer(ws), results);
    ws.next_layer().handshake(ssl::stream_base::client);
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "extreme-reversal-engine");
    }));
    ws.handshake(host, target);

    spdlog::info("Binance stream connected mode={} symbols={}", mode, symbols.size());

    beast::flat_buffer buffer;
    while (keep_running) {
        buffer.clear();
        ws.read(buffer);
        handle_market_message(beast::buffers_to_string(buffer.data()));
    }

    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
}

void market_data_loop(std::vector<std::string> symbols) {
    const std::vector<std::string> modes = {"aggTrade", "trade", "markPrice@1s"};
    while (keep_running) {
        for (const auto& mode : modes) {
            uint64_t before = ws_messages_received.load(std::memory_order_relaxed);
            try {
                run_binance_stream_once(symbols, mode);
            } catch (const std::exception& e) {
                spdlog::warn("Binance stream error mode={}: {}", mode, e.what());
            }

            uint64_t after = ws_messages_received.load(std::memory_order_relaxed);
            if (after > before) break;
            spdlog::warn("Binance stream mode={} produced no messages, trying fallback", mode);
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
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
    write_signal_message(a_msg);
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

                    write_signal_message(b_msg);
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
    const char* symbols_path = std::getenv("SYMBOLS_PATH");
    if (symbols_path == nullptr || std::strlen(symbols_path) == 0) {
        symbols_path = "/tmp/extreme_reversal_symbols.json";
    }

    std::vector<std::string> result;
    try {
        std::ifstream input(symbols_path);
        if (input.good()) {
            json data = json::parse(input);
            for (const auto& item : data) {
                if (item.is_string()) {
                    std::string sym = item.get<std::string>();
                    if (!sym.empty()) result.push_back(sym);
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("failed to load symbols file [{}]: {}", symbols_path, e.what());
    }

    if (result.empty()) {
        result = {"BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT", "XRPUSDT", "DOGEUSDT"};
        spdlog::warn("using fallback symbols: {}", result.size());
    } else {
        spdlog::info("loaded symbols from {}: {}", symbols_path, result.size());
    }

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

    std::thread market_thread(market_data_loop, symbols);
    std::thread detect_thread(run_detection);
    spdlog::info("market data and detection threads started");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::shared_lock<std::shared_mutex> lock(contexts_mutex);
        spdlog::info(
            "heartbeat, symbols={}, ws_messages={}, price_received={}, price_applied={}, unknown_symbol={}, bad_price_msg={}, a_active={}, breakout={}, signals_written={}, signal_write_failures={}",
            contexts.size(),
            ws_messages_received.load(std::memory_order_relaxed),
            price_updates_received.load(std::memory_order_relaxed),
            price_updates_applied.load(std::memory_order_relaxed),
            price_updates_unknown_symbol.load(std::memory_order_relaxed),
            price_bad_messages.load(std::memory_order_relaxed),
            a_active_signals.load(std::memory_order_relaxed),
            breakout_signals.load(std::memory_order_relaxed),
            signal_messages_written.load(std::memory_order_relaxed),
            signal_write_failures.load(std::memory_order_relaxed));
    }

    if (market_thread.joinable()) market_thread.join();
    if (detect_thread.joinable()) detect_thread.join();
    close_signal_pipe();
    return 0;
}
