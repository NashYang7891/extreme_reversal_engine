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

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ---------- 安全类型转换 (你的贡献) ----------
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

// 兜底合约列表
const std::vector<std::string> ULTIMATE_FALLBACK = {
    "BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
    "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
    "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
    "TAGUSDT","BSBUSDT","GENIUSUSDT"
};

// 获取 top_n 个合约，失败或不足时回退到兜底列表
std::vector<std::string> fetch_top_symbols(int top_n = 100, double min_vol = 30000000.0) {
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
            } catch (...) { spdlog::error("解析 ticker 失败，将使用兜底列表"); }
        } else {
            spdlog::error("获取 ticker 失败: {}，将使用兜底列表", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    } else {
        spdlog::error("curl 初始化失败，将使用兜底列表");
    }

    std::sort(tickers.begin(), tickers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::string> result;
    for (size_t i = 0; i < tickers.size() && i < (size_t)top_n; ++i)
        result.push_back(tickers[i].first);

    if (result.size() < 10) {
        spdlog::warn("实时合约不足 10 个，切换为兜底列表");
        return ULTIMATE_FALLBACK;
    }
    spdlog::info("最终监控 {} 个合约, 前3: {}", result.size(),
                 result.size()>=3 ? result[0]+","+result[1]+","+result[2] : "");
    return result;
}

struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
    std::atomic<int64_t> last_active_time{0};
};

std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

// A层（数据不足60个微价格不输出）
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

// 核心业务处理（带时效检查）
void process_json_msg(const json& msg) {
    if (!msg.contains("stream") || !msg.contains("data")) return;

    std::string stream = msg["stream"];
    auto& data = msg["data"];

    int64_t ts = 0;
    if (data.contains("T")) ts = safe_get_int64(data, "T");
    else if (data.contains("E")) ts = safe_get_int64(data, "E");

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();

    // 1.5秒门禁
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
        spdlog::error("数据处理崩溃 [{}]: {}", sym, e.what());
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
                streams.push_back(s + "@aggTrade");
            }
            json sub_msg = {{"method","SUBSCRIBE"}, {"params",streams}, {"id",1}};
            ws_stream.write(net::buffer(sub_msg.dump()));
            spdlog::info("WebSocket 连接成功，已订阅 {} 个流", streams.size());

            beast::flat_buffer buffer;
            while (keep_running) {
                ws_stream.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();

                // 积压清理，上限 5 次
                int cleanup_limit = 5;
                while (cleanup_limit-- > 0 && ws_stream.next_layer().next_layer().available() > 0) {
                    beast::error_code ec;
                    ws_stream.read(buffer, ec);
                    if (ec) break;
                    process_json_msg(json::parse(beast::buffers_to_string(buffer.data())));
                    buffer.clear();
                }

                process_json_msg(msg);
            }
        } catch (const std::exception& e) {
            spdlog::error("WebSocket 线程严重故障: {}，3秒后重连", e.what());
        } catch (...) {
            spdlog::error("WebSocket 线程未知异常，3秒后重连");
        }
        if (keep_running) std::this_thread::sleep_for(std::chrono::seconds(3));
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

            auto now_steady = std::chrono::steady_clock::now();
            if (now_steady - last_heartbeat > std::chrono::minutes(30)) {
                json hb;
                hb["type"] = "HEARTBEAT";
                hb["symbols"] = contexts.size();
                hb["timestamp"] = std::time(nullptr);
                std::cout << hb.dump() << std::endl;
                last_heartbeat = now_steady;
            }

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
                            } else {
                                b_msg["stop_loss"]   = p + stop_dist;
                                b_msg["take_profit"] = std::min(tp_atr, tp_pct_down);
                            }

                            if (sig.side == "LONG") {
                                if (b_msg["stop_loss"] < p * 0.95) b_msg["stop_loss"] = p * 0.95;
                                if (b_msg["take_profit"] <= p) b_msg["take_profit"] = p * 1.02;
                            } else {
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
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::info(">>> 极端反转引擎 [融合稳健版] 启动中...");

    try {
        auto symbols = fetch_top_symbols(100, 30000000.0);
        if (symbols.empty()) {
            spdlog::critical("无可用合约，引擎退出");
            return 1;
        }
        {
            std::unique_lock lock(contexts_mutex);
            for (const auto& sym : symbols) {
                contexts.emplace(std::piecewise_construct,
                                 std::forward_as_tuple(sym),
                                 std::forward_as_tuple());
            }
        }
        spdlog::info("引擎启动，监控 {} 个合约", symbols.size());

        std::thread ws_thread(run_websocket, symbols);
        std::thread detect_thread(run_detection);

        ws_thread.join();
        detect_thread.join();
    } catch (const std::exception& e) {
        spdlog::error("主程序抛出未捕获异常: {}", e.what());
        return 1;
    }
    return 0;
}