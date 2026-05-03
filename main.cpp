#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <curl/curl.h>
#include "orderbook.h"
#include "indicators.h"
#include "signal_detector.h"
#include "ml_optimizer.h"

using json = nlohmann::json;
namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

std::atomic<bool> keep_running{true};

// 获取热门合约列表
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::vector<std::string> fetch_top_symbols(int top_n = 30, double min_vol = 30000000.0) {
    std::vector<std::string> result;
    CURL *curl = curl_easy_init();
    if (!curl) return result;
    std::string response;
    // 1. 获取所有 SWAP 合约
    curl_easy_setopt(curl, CURLOPT_URL, "https://www.okx.com/api/v5/public/instruments?instType=SWAP");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_perform(curl);
    json insts = json::parse(response);
    response.clear();

    // 2. 获取 tickers
    curl_easy_setopt(curl, CURLOPT_URL, "https://www.okx.com/api/v5/market/tickers?instType=SWAP");
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_perform(curl);
    json tickers = json::parse(response);
    curl_easy_cleanup(curl);

    std::map<std::string, double> vol_map;
    for (auto& t : tickers["data"]) {
        if (t.contains("instId") && t.contains("volCcy24h"))
            vol_map[t["instId"]] = std::stod(t["volCcy24h"].get<std::string>());
    }

    std::vector<std::pair<std::string, double>> eligible;
    for (auto& item : insts["data"]) {
        std::string id = item["instId"];
        if (item["settleCcy"] == "USDT" && item["state"] == "live") {
            double vol = vol_map[id];
            if (vol >= min_vol)
                eligible.emplace_back(id, vol);
        }
    }
    std::sort(eligible.begin(), eligible.end(),
              [](auto& a, auto& b){ return a.second > b.second; });
    for (int i = 0; i < std::min(top_n, (int)eligible.size()); ++i)
        result.push_back(eligible[i].first);
    spdlog::info("将监控 {} 个合约，前5: {}", result.size(),
                 result.size()>0?result[0]:"");
    return result;
}

// 每个合约的完整状态
struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
};

int main() {
    auto symbols = fetch_top_symbols(30, 30000000.0);
    if (symbols.empty()) {
        spdlog::error("没有符合条件的合约");
        return 1;
    }

    std::map<std::string, SymbolContext> contexts;
    for (auto& sym : symbols) contexts.emplace(sym, SymbolContext{});

    // WebSocket
    net::io_context ioc;
    ws::stream<tcp::socket> ws(ioc);
    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve("ws.okx.com", "8443");
    net::connect(ws.next_layer(), results.begin(), results.end());
    ws.handshake("ws.okx.com", "/ws/v5/public");

    // 构造订阅请求
    json sub_args = json::array();
    for (auto& sym : symbols) {
        sub_args.push_back({{"channel", "books5"}, {"instId", sym}});
        sub_args.push_back({{"channel", "trades"}, {"instId", sym}});
    }
    json sub_msg = {{"op", "subscribe"}, {"args", sub_args}};
    ws.write(net::buffer(sub_msg.dump()));

    // 接收线程
    std::thread ws_thread([&](){
        beast::flat_buffer buffer;
        while (keep_running) {
            try {
                ws.read(buffer);
                auto msg = json::parse(beast::buffers_to_string(buffer.data()));
                buffer.clear();
                if (msg.contains("arg")) {
                    std::string ch = msg["arg"]["channel"];
                    std::string instId = msg["arg"]["instId"];
                    auto it = contexts.find(instId);
                    if (it == contexts.end()) continue;
                    if (ch == "books5") {
                        it->second.orderbook.update(msg["data"][0]);
                    } else if (ch == "trades") {
                        for (auto& trade : msg["data"]) {
                            bool isBuy = trade["side"] == "buy";
                            double sz = std::stod(trade["sz"].get<std::string>());
                            it->second.orderbook.add_trade(isBuy, sz);
                        }
                    }
                }
            } catch (std::exception const& e) {
                spdlog::error("WebSocket error: {}", e.what());
                break;
            }
        }
    });

    // 检测线程：每100ms循环所有symbol
    std::thread detect_thread([&](){
        while (keep_running) {
            auto start = std::chrono::steady_clock::now();
            for (auto& [sym, ctx] : contexts) {
                // 更新微价格
                double mp = ctx.orderbook.micro_price();
                if (mp > 0) ctx.indicators.update(mp);
                // 检测信号
                auto sig = ctx.detector.check(ctx.orderbook);
                if (sig.valid) {
                    json out;
                    out["symbol"] = sym;
                    out["side"] = sig.side;
                    out["price"] = sig.price;
                    out["score"] = sig.score;
                    out["timestamp"] = std::time(nullptr);
                    std::cout << out.dump() << std::endl;
                }
            }
            auto elapsed = std::chrono::steady_clock::now() - start;
            std::this_thread::sleep_for(std::chrono::milliseconds(100) - elapsed);
        }
    });

    ws_thread.join();
    detect_thread.join();
    return 0;
}