#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <cctype>
#include <shared_mutex>
#include <curl/curl.h>
#include "orderbook.h"
#include "indicators.h"
#include "signal_detector.h"
#include "ml_optimizer.h"

using json = nlohmann::json;
namespace beast = boost::beast;
namespace ws   = beast::websocket;
namespace net  = boost::asio;
namespace ssl  = boost::asio::ssl;
using tcp = net::ip::tcp;

std::atomic<bool> keep_running{true};

// 绝对兜底列表（10个主流币）
const std::vector<std::string> SYMBOLS = {
    "BTCUSDT", "ETHUSDT", "SOLUSDT", "XRPUSDT", "DOGEUSDT",
    "TRXUSDT", "BNBUSDT", "LINKUSDT", "LTCUSDT", "ADAUSDT"
};

struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
    std::atomic<int64_t> last_active_time{0};
};

std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

// A层（数据不足60点不输出）
bool active_layer(const OrderBook& ob, Indicators& ind, double& out_change, double& out_vol_ratio) {
    if (ind.prices().size() < 60) return false;

    double change_3m = ind.price_change_pct(3*60);
    if (std::abs(change_3m) > 0.20) return false;
    if (std::abs(change_3m) < 0.012) return false;

    double recent_vol = ob.recent_volume(3*60*1000);
    double avg_vol = ind.get_volume_ema();
    if (avg_vol <= 1e-9) {
        ind.update_volume(recent_vol);
        out_change = change_3m;
        out_vol_ratio = 1.0;
        return true;
    }

    double vol_ratio = recent_vol / avg_vol;
    if (vol_ratio < 1.5) return false;

    ind.update_volume(recent_vol);
    out_change = change_3m;
    out_vol_ratio = vol_ratio;
    return true;
}

void process_json_msg(const json& msg) {
    if (!msg.contains("stream") || !msg.contains("data")) return;

    std::string stream = msg["stream"];
    auto& data = msg["data"];

    size_t pos = stream.find('@');
    if (pos == std::string::npos) return;
    std::string sym = stream.substr(0, pos);
    for (char& c : sym) c = std::toupper(c);

    std::unique_lock lock(contexts_mutex);
    auto it = contexts.find(sym);
    if (it == contexts.end()) return;

    try {
        if (stream.find("@depth") != std::string::npos) {
            it->second.orderbook.update_depth(data);
            double mp = it->second.orderbook.micro_price();
            if (mp > 0) it->second.indicators.update(mp);
        }
    } catch (const std::exception& e) {
        spdlog::warn("数据处理异常 [{}]: {}", sym, e.what());
    }
}

void run_websocket(const std::vector<std::string>& symbols) {
    while (keep_running) {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tls_client};
            ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
            ctx.set_default_verify_paths(); ctx.set_verify_mode(ssl::verify_peer);
            ws::stream<beast::ssl_stream<tcp::socket>> ws_stream(ioc, ctx);
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve("fstream.binance.com", "443");
            net::connect(ws_stream.next_layer().next_layer(), results.begin(), results.end());
            ws_stream.next_layer().handshake(ssl::stream_base::client);
            ws_stream.handshake("fstream.binance.com", "/stream");

            std::vector<std::string> streams;
            for (const auto& sym : symbols) {
                std::string s = sym;
                for (char& c : s) c = std::tolower(c);
                streams.push_back(s + "@depth@500ms");
            }
            json sub_msg = {{"method","SUBSCRIBE"}, {"params",streams}, {"id",1}};
            ws_stream.write(net::buffer(sub_msg.dump()));
            spdlog::info("WebSocket 连接成功，已订阅 {} 个流", streams.size());

            beast::flat_buffer buffer;
            while (keep_running) {
                ws_stream.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();
                process_json_msg(msg);
            }
        } catch (const std::exception& e) {
            spdlog::error("WebSocket 异常: {}，10秒后重连", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
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
                    double change_pct = 0.0, vol_ratio = 0.0;
                    if (active_layer(ctx.orderbook, ctx.indicators, change_pct, vol_ratio)) {
                        ctx.last_active_time = now_ms;

                        json a_msg;
                        a_msg["type"] = "A_ACTIVE";
                        a_msg["symbol"] = sym;
                        a_msg["current_price"] = ctx.indicators.price();
                        a_msg["price"] = ctx.indicators.price();
                        a_msg["change_pct"] = change_pct * 100.0;
                        a_msg["vol_ratio"] = vol_ratio;
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
                        auto sig = ctx.detector.check(ctx.orderbook);
                        if (sig.valid) {
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
                            double stop_dist = std::max(atr * 2.0, p * 0.02);
                            if (stop_dist < p * 0.01) stop_dist = p * 0.01;

                            if (sig.side == "LONG") {
                                b_msg["stop_loss"]   = p - stop_dist;
                                b_msg["take_profit"] = p + atr * 3.0;
                            } else {
                                b_msg["stop_loss"]   = p + stop_dist;
                                b_msg["take_profit"] = p - atr * 3.0;
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
    // 强制 stdout 无缓冲，让 Python 实时接收
    setvbuf(stdout, NULL, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::info(">>> 极端反转引擎 [最终稳定版] 启动...");

    auto symbols = SYMBOLS;
    {
        std::unique_lock lock(contexts_mutex);
        for (const auto& sym : symbols)
            contexts.emplace(std::piecewise_construct, std::forward_as_tuple(sym), std::forward_as_tuple());
    }
    spdlog::info("引擎启动，监控 {} 个合约", symbols.size());

    std::thread ws_thread(run_websocket, symbols);
    std::thread detect_thread(run_detection);
    spdlog::info("✅ 所有线程已启动");

    // 永久保活
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        spdlog::info("💓 保活心跳, 监控 {} 个合约", contexts.size());
    }

    if (ws_thread.joinable()) ws_thread.join();
    if (detect_thread.joinable()) detect_thread.join();
    return 0;
}