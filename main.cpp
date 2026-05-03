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

// libcurl 回调
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// 获取币安 USDT 永续合约，按 24h 成交量过滤，取前 30 个
std::vector<std::string> fetch_top_symbols(int top_n = 30, double min_vol = 30000000.0) {
    std::vector<std::string> result;
    CURL *curl = curl_easy_init();
    if (!curl) return result;
    std::string response;

    // 1. 获取所有合约信息
    curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/exchangeInfo");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_perform(curl);
    json exInfo = json::parse(response);
    response.clear();

    std::vector<std::string> all_symbols;
    for (auto& s : exInfo["symbols"]) {
        if (s["quoteAsset"] == "USDT" && s["contractType"] == "PERPETUAL" && s["status"] == "TRADING") {
            all_symbols.push_back(s["symbol"]);
        }
    }

    // 2. 获取每个合约的 24h 成交量
    std::map<std::string, double> vol_map;
    for (auto& sym : all_symbols) {
        std::string url = "https://fapi.binance.com/fapi/v1/ticker/24hr?symbol=" + sym;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_perform(curl);
        try {
            json ticker = json::parse(response);
            double vol = std::stod(ticker["quoteVolume"].get<std::string>());
            vol_map[sym] = vol;
        } catch (...) {
            vol_map[sym] = 0.0;
        }
        response.clear();
    }
    curl_easy_cleanup(curl);

    // 3. 过滤并排序
    std::vector<std::pair<std::string, double>> eligible;
    for (auto& sym : all_symbols) {
        if (vol_map[sym] >= min_vol)
            eligible.emplace_back(sym, vol_map[sym]);
    }
    std::sort(eligible.begin(), eligible.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (int i = 0; i < std::min(top_n, (int)eligible.size()); ++i)
        result.push_back(eligible[i].first);
    spdlog::info("将监控 {} 个合约, 前3: {}", result.size(),
                 result.size()>0 ? result[0] : "无");
    return result;
}

// 每个合约的完整状态
struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
};

// WebSocket 线程函数（自动重连）
void run_websocket(std::map<std::string, SymbolContext>& contexts, const std::vector<std::string>& symbols) {
    while (keep_running) {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tls_client};
            ctx.set_options(ssl::context::default_workarounds |
                            ssl::context::no_sslv2 |
                            ssl::context::no_sslv3);
            ctx.set_verify_mode(ssl::verify_none); // 测试环境，生产请用默认验证

            ws::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve("fstream.binance.com", "443");
            net::connect(ws.next_layer().next_layer(), results.begin(), results.end());
            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake("fstream.binance.com", "/stream");

            // 构造订阅流（转为小写）
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
            spdlog::error("WebSocket 异常: {}，3秒后重连...", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
}

int main() {
    auto symbols = fetch_top_symbols(30, 30000000.0);
    if (symbols.empty()) {
        spdlog::error("没有符合条件的合约");
        return 1;
    }

    std::map<std::string, SymbolContext> contexts;
    for (auto& sym : symbols) contexts.emplace(sym, SymbolContext{});

    // 启动 WebSocket 线程（自动重连）
    std::thread ws_thread(run_websocket, std::ref(contexts), symbols);

    // 检测线程（每 100ms 遍历所有合约，带异常保护）
    std::thread detect_thread([&](){
        while (keep_running) {
            auto start = std::chrono::steady_clock::now();
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
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < std::chrono::milliseconds(100))
                std::this_thread::sleep_for(std::chrono::milliseconds(100) - elapsed);
        }
    });

    spdlog::info("引擎已成功启动，正在监控 {} 个合约", symbols.size());
    ws_thread.join();
    detect_thread.join();
    return 0;
}