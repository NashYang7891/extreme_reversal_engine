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
#include <csignal>
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
std::atomic<int64_t> last_data_time_ms{0};

// 信号处理：记录崩溃地址
void signal_handler(int sig) {
    spdlog::critical("*** C++ 引擎收到信号 {}，即将退出 ***", sig);
    keep_running = false;
    std::exit(sig);
}

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

int64_t safe_get_int64(const json& j, const std::string& key) {
    if (!j.contains(key)) return 0;
    if (j[key].is_number()) return j[key].get<int64_t>();
    if (j[key].is_string()) return std::stoll(j[key].get<std::string>());
    return 0;
}

double safe_get_double(const json& j, const std::string& key) {
    if (!j.contains(key)) return 0.0;
    if (j[key].is_number()) return j[key].get<double>();
    if (j[key].is_string()) return std::stod(j[key].get<std::string>());
    return 0.0;
}

// 内置 20 个主流合约，完全离线可用
const std::vector<std::string> ULTIMATE_FALLBACK = {
    "BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
    "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
    "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
    "TAGUSDT","BSBUSDT","GENIUSUSDT"
};

// 直接返回兜底列表，跳过所有网络请求
std::vector<std::string> fetch_top_symbols(int top_n = 10, double min_vol = 30000000.0) {
    spdlog::info("使用内置合约列表 (离线模式)");
    return {ULTIMATE_FALLBACK.begin(), ULTIMATE_FALLBACK.begin() + std::min(top_n, (int)ULTIMATE_FALLBACK.size())};
}

struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
    std::atomic<int64_t> last_active_time{0};
    std::atomic<int64_t> last_a_push_5s_ms{0};
};

std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

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

    int64_t ts = 0;
    if (data.contains("T")) ts = safe_get_int64(data, "T");
    else if (data.contains("E")) ts = safe_get_int64(data, "E");

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    last_data_time_ms = now_ms;
    if (ts > 0 && std::abs(now_ms - ts) > 1500) return;

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
        } else if (stream.find("@aggTrade") != std::string::npos) {
            double p = safe_get_double(data, "p");
            double q = safe_get_double(data, "q");
            bool isMaker = data["m"].get<bool>();
            it->second.orderbook.add_agg_trade(!isMaker, q, ts);
        }
    } catch (const std::exception& e) {
        spdlog::error("数据处理异常 [{}]: {}", sym, e.what());
    }
}

void run_websocket(const std::vector<std::string>& symbols) {
    int retry = 0;
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
                streams.push_back(s + "@depth@100ms");
                streams.push_back(s + "@aggTrade");
            }
            json sub_msg = {{"method","SUBSCRIBE"}, {"params",streams}, {"id",1}};
            ws_stream.write(net::buffer(sub_msg.dump()));
            spdlog::info("WebSocket 连接成功，订阅 {} 个流", streams.size());
            retry = 0;

            beast::flat_buffer buffer;
            while (keep_running) {
                ws_stream.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();
                process_json_msg(msg);
                // 不积压清理
            }
        } catch (const std::exception& e) {
            spdlog::error("WebSocket 异常: {}，下次重试 {}s", e.what(), 3 + retry * 2);
        } catch (...) {
            spdlog::error("WebSocket 未知异常");
        }
        if (keep_running) {
            int w = 3 + retry * 2;
            if (w > 30) w = 30;
            std::this_thread::sleep_for(std::chrono::seconds(w));
            retry++;
        }
    }
}

void run_detection() {
    auto last_heartbeat = std::chrono::steady_clock::now();
    while (keep_running) {
        auto start = std::chrono::steady_clock::now();
        {
            std::shared_lock lock(contexts_mutex);
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            if (std::chrono::steady_clock::now() - last_heartbeat > std::chrono::minutes(30)) {
                json hb;
                hb["type"] = "HEARTBEAT";
                hb["symbols"] = contexts.size();
                hb["timestamp"] = std::time(nullptr);
                std::cout << hb.dump() << std::endl;
                last_heartbeat = std::chrono::steady_clock::now();
            }

            for (auto& [sym, ctx] : contexts) {
                if (ctx.indicators.is_stale(60000)) continue;
                try {
                    double change_pct = 0.0, vol_ratio = 0.0;
                    if (active_layer(ctx.orderbook, ctx.indicators, change_pct, vol_ratio)) {
                        if (now_ms - ctx.last_a_push_5s_ms.load() < 5000) continue;
                        ctx.last_a_push_5s_ms = now_ms;
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
                            a_msg["dev"] = (ctx.indicators.ema20() - ctx.indicators.price()) / atr;
                            if (std::abs(a_msg["dev"].get<double>()) < 50.0) ;
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
                            double raw_atr_stop = (atr > 1e-9) ? (atr * 2.0) : (p * 0.02);
                            double pct_stop     = p * 0.02;
                            double stop_dist    = std::max(raw_atr_stop, pct_stop);
                            if (stop_dist < p * 0.01) stop_dist = p * 0.01;
                            double tp_atr = (atr > 1e-9) ? (p + atr * 3.0) : (p * 1.03);
                            double tp_pct_up   = p * 1.03;
                            double tp_pct_down = p * 0.98;

                            if (sig.side == "LONG") {
                                b_msg["stop_loss"]   = p - stop_dist;
                                b_msg["take_profit"] = std::max(tp_atr, tp_pct_up);
                                if (b_msg["stop_loss"] < p * 0.95) b_msg["stop_loss"] = p * 0.95;
                                if (b_msg["take_profit"] <= p) b_msg["take_profit"] = p * 1.02;
                            } else {
                                b_msg["stop_loss"]   = p + stop_dist;
                                b_msg["take_profit"] = std::min(tp_atr, tp_pct_down);
                                if (b_msg["stop_loss"] > p * 1.05) b_msg["stop_loss"] = p * 1.05;
                                if (b_msg["take_profit"] >= p) b_msg["take_profit"] = p * 0.98;
                            }
                            std::cout << b_msg.dump() << std::endl;
                            ctx.last_active_time = 0;
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::error("检测 {} 异常: {}", sym, e.what());
                } catch (...) {
                    spdlog::error("检测 {} 未知异常", sym);
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
        
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::info(">>> 极端反转引擎 [系统服务调试版] 启动...");

    try {
        auto symbols = fetch_top_symbols(10, 0);   // 10个合约，离线模式
        if (symbols.empty()) {
            spdlog::critical("无可用合约，退出");
            return 1;
        }
        {
            std::unique_lock lock(contexts_mutex);
            for (const auto& sym : symbols)
                contexts.emplace(std::piecewise_construct, std::forward_as_tuple(sym), std::forward_as_tuple());
        }
        spdlog::info("引擎启动，监控 {} 个合约", symbols.size());

        std::thread ws_thread(run_websocket, symbols);
        std::thread detect_thread(run_detection);
        spdlog::info("✅ 所有线程已启动");

        // 主线程健康检查
        const int64_t STALE_THRESHOLD_MS = 300000;
        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (last_data_time_ms.load() > 0 && (now_ms - last_data_time_ms.load()) > STALE_THRESHOLD_MS) {
                spdlog::critical("❌ 数据停滞 {} 秒，主动退出", (now_ms - last_data_time_ms.load()) / 1000);
                keep_running = false;
            }
        }
        if (ws_thread.joinable()) ws_thread.join();
        if (detect_thread.joinable()) detect_thread.join();
    } catch (const std::exception& e) {
        spdlog::error("main 异常: {}", e.what());
        return 1;
    } catch (...) {
        spdlog::error("main 未知异常");
        return 1;
    }
    return 0;
}