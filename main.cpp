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

const std::vector<std::string> ULTIMATE_FALLBACK = {
    "BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
    "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
    "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
    "TAGUSDT","BSBUSDT","GENIUSUSDT"
};

std::vector<std::string> fetch_top_symbols(int top_n = 100, double min_vol = 30000000.0) {
    CURL *curl = curl_easy_init();
    if (!curl) { spdlog::error("curl init fail"); return ULTIMATE_FALLBACK; }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/24hr");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) { spdlog::error("ticker fail"); curl_easy_cleanup(curl); return ULTIMATE_FALLBACK; }
    std::vector<std::pair<std::string, double>> tickers;
    try {
        auto data = json::parse(response);
        for (auto& item : data) {
            std::string sym = item["symbol"];
            if (sym.size()>4 && sym.compare(sym.size()-4,4,"USDT")==0 &&
                sym.find('_')==std::string::npos && sym!="USDCUSDT") {
                tickers.emplace_back(sym, std::stod(item["quoteVolume"].get<std::string>()));
            }
        }
    } catch (...) { spdlog::error("parse fail"); curl_easy_cleanup(curl); return ULTIMATE_FALLBACK; }
    curl_easy_cleanup(curl);
    std::sort(tickers.begin(), tickers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> result;
    for (auto& [sym,vol] : tickers) { if (vol >= min_vol) { result.push_back(sym); if ((int)result.size() >= top_n) break; } }
    if (result.empty()) { spdlog::warn("no symbols"); result = ULTIMATE_FALLBACK; }
    spdlog::info("监控 {} 个合约, 前3: {}", result.size(),
                 result.size()>=3 ? result[0]+","+result[1]+","+result[2] : "");
    return result;
}

struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
    std::atomic<int64_t> last_active_time{0};
    std::atomic<int64_t> last_a_push_ms{0};
};

std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

// A层（冷启动放行，涨跌幅≥1.2%、量比≥1.5）
bool active_layer(const OrderBook& ob, const Indicators& ind, double& out_change, double& out_vol_ratio) {
    double change_3m = ind.price_change_pct(3*60);
    if (std::abs(change_3m) > 0.20) return false;
    if (std::abs(change_3m) < 0.012) return false;

    double recent_vol = ob.recent_volume(3*60*1000);
    double avg_vol = ind.get_volume_ema();
    if (avg_vol <= 1e-9) {
        // EMA未初始化：直接放行，量比记为1.5
        out_change = change_3m;
        out_vol_ratio = 1.5;
        return true;
    }
    double vol_ratio = recent_vol / avg_vol;
    if (vol_ratio < 1.5) return false;

    out_change = change_3m;
    out_vol_ratio = vol_ratio;
    return true;
}

void run_websocket(const std::vector<std::string>& symbols) {
    while (keep_running) {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tls_client};
            ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
            ctx.set_default_verify_paths(); ctx.set_verify_mode(ssl::verify_peer);
            ws::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve("fstream.binance.com","443");
            net::connect(ws.next_layer().next_layer(), results.begin(), results.end());
            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake("fstream.binance.com","/stream");
            std::vector<std::string> streams;
            for (auto& sym : symbols) {
                std::string s = sym;
                for (char& c : s) c = std::tolower(c);
                streams.push_back(s+"@depth@100ms");
                streams.push_back(s+"@aggTrade");
            }
            json sub_msg = {{"method","SUBSCRIBE"}, {"params",streams}, {"id",1}};
            ws.write(net::buffer(sub_msg.dump()));
            beast::flat_buffer buffer;
            while (keep_running) {
                ws.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();
                if (msg.contains("stream") && msg.contains("data")) {
                    std::string stream = msg["stream"]; auto& data = msg["data"];
                    size_t pos = stream.find('@'); if (pos==std::string::npos) continue;
                    std::string sym = stream.substr(0,pos);
                    for (char& c : sym) c = std::toupper(c);
                    std::unique_lock lock(contexts_mutex);
                    auto it = contexts.find(sym); if (it==contexts.end()) continue;
                    if (stream.find("@depth")!=std::string::npos) {
                        it->second.orderbook.update_depth(data);
                        double mp = it->second.orderbook.micro_price();
                        if (mp > 0) it->second.indicators.update(mp);
                    } else if (stream.find("@aggTrade")!=std::string::npos) {
                        double price = std::stod(data["p"].get<std::string>());
                        double qty   = std::stod(data["q"].get<std::string>());
                        bool isMaker = data["m"];
                        int64_t trade_time = std::stoll(data["T"].get<std::string>());
                        it->second.orderbook.add_agg_trade(!isMaker, qty, trade_time);
                        // 每次成交更新成交量 EMA
                        double recent_vol = it->second.orderbook.recent_volume(3*60*1000);
                        it->second.indicators.update_volume(recent_vol);
                    }
                }
            }
        } catch (std::exception const& e) {
            spdlog::error("WS异常: {}，3s后重连", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(3));
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

            // 每30分钟心跳
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
                        if (now_ms - ctx.last_a_push_ms.load() < 120000) continue;
                        ctx.last_a_push_ms = now_ms;
                        ctx.last_active_time = now_ms;
                        json a_msg;
                        a_msg["type"] = "A_ACTIVE";
                        a_msg["symbol"] = sym;
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

                            double atr = ctx.indicators.atr();
                            if (atr > 1e-9) {
                                double raw_atr_stop = atr * 2.0;
                                double pct_stop = sig.price * 0.02;          // 2%硬止损
                                double stop = std::max(raw_atr_stop, pct_stop);
                                double tp = sig.price + atr * 3.0;
                                if (sig.side == "LONG") {
                                    b_msg["stop_loss"]   = sig.price - stop;
                                    b_msg["take_profit"] = std::max(tp, sig.price * 1.015);
                                } else {
                                    b_msg["stop_loss"]   = sig.price + stop;
                                    b_msg["take_profit"] = std::min(tp, sig.price * 0.985);
                                }
                            } else {
                                if (sig.side == "LONG") {
                                    b_msg["stop_loss"]   = sig.price * 0.98;
                                    b_msg["take_profit"] = sig.price * 1.03;
                                } else {
                                    b_msg["stop_loss"]   = sig.price * 1.02;
                                    b_msg["take_profit"] = sig.price * 0.97;
                                }
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
    auto symbols = fetch_top_symbols(100, 30000000.0);
    {
        std::unique_lock lock(contexts_mutex);
        for (auto& sym : symbols) contexts.emplace(std::piecewise_construct,
                                                   std::forward_as_tuple(sym),
                                                   std::forward_as_tuple());
    }
    spdlog::info("引擎启动，监控 {} 个合约 (最终自锁修复版)", symbols.size());
    std::thread ws_thread(run_websocket, symbols);
    std::thread detect_thread(run_detection);
    ws_thread.join();
    detect_thread.join();
    return 0;
}