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

// 终极兜底列表（去重后20个主流+高波动品种）
const std::vector<std::string> ULTIMATE_FALLBACK = {
    "BTCUSDT", "ETHUSDT", "SOLUSDT", "XRPUSDT", "DOGEUSDT", "TRXUSDT",
    "BNBUSDT", "ZECUSDT", "BIOUSDT", "ORDIUSDT", "TSTUSDT", "BABYUSDT",
    "FHEUSDT", "BUSDT", "AIGENSYNUSDT", "AKTUSDT", "PARTIUSDT",
    "TAGUSDT", "BSBUSDT", "GENIUSUSDT"
};

std::vector<std::string> fetch_top_symbols(int top_n = 30, double min_vol = 30000000.0) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        spdlog::error("curl 初始化失败，返回兜底列表");
        return ULTIMATE_FALLBACK;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/24hr");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        spdlog::error("获取 24hr ticker 失败: {}，使用兜底列表", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return ULTIMATE_FALLBACK;
    }

    std::vector<std::pair<std::string, double>> tickers;
    try {
        auto data = json::parse(response);
        for (auto& item : data) {
            std::string symbol = item["symbol"];
            if (symbol.size() > 4 && symbol.compare(symbol.size()-4, 4, "USDT") == 0 &&
                symbol.find('_') == std::string::npos && symbol != "USDCUSDT") {
                double vol = std::stod(item["quoteVolume"].get<std::string>());
                tickers.emplace_back(symbol, vol);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("解析 ticker 失败: {}，使用兜底列表", e.what());
        curl_easy_cleanup(curl);
        return ULTIMATE_FALLBACK;
    }
    curl_easy_cleanup(curl);

    std::sort(tickers.begin(), tickers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::string> result;
    for (auto& [sym, vol] : tickers) {
        if (vol >= min_vol) {
            result.push_back(sym);
            if ((int)result.size() >= top_n) break;
        }
    }

    if (result.empty()) {
        spdlog::warn("无合约满足成交量门槛，使用兜底列表");
        result = ULTIMATE_FALLBACK;
    }

    spdlog::info("将监控 {} 个合约, 前3: {}", result.size(),
                 result.size() >= 3 ? result[0]+","+result[1]+","+result[2] : "");
    return result;
}

struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
};

// 全局上下文 + 读写锁
std::map<std::string, SymbolContext> contexts;
std::shared_mutex contexts_mutex;

void run_websocket(const std::vector<std::string>& symbols) {
    while (keep_running) {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tls_client};
            ctx.set_options(ssl::context::default_workarounds |
                            ssl::context::no_sslv2 | ssl::context::no_sslv3);
            ctx.set_default_verify_paths();
            ctx.set_verify_mode(ssl::verify_peer);

            ws::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve("fstream.binance.com", "443");
            net::connect(ws.next_layer().next_layer(), results.begin(), results.end());
            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake("fstream.binance.com", "/stream");

            std::vector<std::string> streams;
            for (auto& sym : symbols) {
                std::string s = sym;
                for (char& c : s) c = std::tolower(c);
                streams.push_back(s + "@depth@100ms");
                streams.push_back(s + "@aggTrade");
            }
            json sub_msg = {{"method", "SUBSCRIBE"}, {"params", streams}, {"id", 1}};
            ws.write(net::buffer(sub_msg.dump()));

            beast::flat_buffer buffer;
            while (keep_running) {
                ws.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();
                if (msg.contains("stream") && msg.contains("data")) {
                    std::string stream = msg["stream"];
                    const auto& data = msg["data"];
                    size_t pos = stream.find('@');
                    if (pos == std::string::npos) continue;
                    std::string sym = stream.substr(0, pos);
                    for (char& c : sym) c = std::toupper(c);
                    
                    std::unique_lock lock(contexts_mutex);
                    auto it = contexts.find(sym);
                    if (it == contexts.end()) continue;

                    if (stream.find("@depth") != std::string::npos) {
                        it->second.orderbook.update_depth(data);
                        double mp = it->second.orderbook.micro_price();
                        if (mp > 0) it->second.indicators.update(mp);
                    } else if (stream.find("@aggTrade") != std::string::npos) {
                        double price = std::stod(data["p"].get<std::string>());
                        double qty   = std::stod(data["q"].get<std::string>());
                        bool isMaker = data["m"];
                        it->second.orderbook.add_agg_trade(!isMaker, qty);
                    }
                }
            }
        } catch (std::exception const& e) {
            spdlog::error("WebSocket 异常: {}，3秒后重连", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
}

void run_detection() {
    while (keep_running) {
        auto start = std::chrono::steady_clock::now();
        {
            std::shared_lock lock(contexts_mutex);   // 读锁
            for (auto& [sym, ctx] : contexts) {
                try {
                    auto sig = ctx.detector.check(ctx.orderbook);
                    if (sig.valid) {
                        json out;
                        out["symbol"] = sym;
                        out["side"]   = sig.side;
                        out["price"]  = sig.price;
                        out["score"]  = sig.score;
                        out["timestamp"] = std::time(nullptr);
                        std::cout << out.dump() << std::endl;
                    }
                } catch (const std::exception& e) {
                    spdlog::error("检测 {} 时异常: {}", sym, e.what());
                } catch (...) {
                    spdlog::error("检测 {} 时未知异常", sym);
                }
            }
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < std::chrono::milliseconds(100))
            std::this_thread::sleep_for(std::chrono::milliseconds(100) - elapsed);
    }
}

int main() {
    auto symbols = fetch_top_symbols(30, 30000000.0);
    // 初始化 contexts（写锁）
    {
        std::unique_lock lock(contexts_mutex);
        for (auto& sym : symbols) contexts.emplace(sym, SymbolContext{});
    }

    spdlog::info("引擎已成功启动，正在监控 {} 个合约", symbols.size());

    std::thread ws_thread(run_websocket, symbols);
    std::thread detect_thread(run_detection);

    ws_thread.join();
    detect_thread.join();
    return 0;
}