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

// libcurl 写回调
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// 获取币安 USDT 永续合约成交量排行，过滤并返回前 N 个品种
std::vector<std::string> fetch_top_symbols(int top_n = 30, double min_vol = 30000000.0) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        spdlog::error("curl 初始化失败，使用兜底列表");
        // 基于你提供的排行榜（去重）组成默认监控列表
        return {"BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
                "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
                "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
                "TAGUSDT","BSBUSDT","GENIUSUSDT"};
    }

    std::string response;
    // 一次性获取所有 USDT 永续合约的 24 小时行情（包含成交量）
    curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/24hr");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        spdlog::error("获取 24hr ticker 失败: {}，使用兜底列表", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return {"BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
                "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
                "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
                "TAGUSDT","BSBUSDT","GENIUSUSDT"};
    }

    // 解析成交量数据
    std::vector<std::pair<std::string, double>> tickers;
    try {
        auto data = json::parse(response);
        for (auto& item : data) {
            std::string symbol = item["symbol"];
            // 筛选：以 USDT 结尾、不含 '_' (排除非永续)，排除稳定币对
            if (symbol.size() > 4 && symbol.compare(symbol.size()-4, 4, "USDT") == 0 &&
                symbol.find('_') == std::string::npos &&
                symbol != "USDCUSDT") {
                double vol = std::stod(item["quoteVolume"].get<std::string>());
                tickers.emplace_back(symbol, vol);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("解析 ticker 数据异常: {}，使用兜底列表", e.what());
        curl_easy_cleanup(curl);
        return {"BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
                "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
                "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
                "TAGUSDT","BSBUSDT","GENIUSUSDT"};
    }
    curl_easy_cleanup(curl);

    // 按成交量降序排列
    std::sort(tickers.begin(), tickers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // 过滤成交量门槛，取前 top_n 个
    std::vector<std::string> result;
    for (auto& [sym, vol] : tickers) {
        if (vol >= min_vol) {
            result.push_back(sym);
            if ((int)result.size() >= top_n) break;
        }
    }

    // 若实时筛选结果为空，回退到兜底列表
    if (result.empty()) {
        spdlog::warn("当前无合约满足 {:.0f} 万美元成交量门槛，使用兜底列表", min_vol / 10000.0);
        result = {"BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","TRXUSDT",
                  "BNBUSDT","ZECUSDT","BIOUSDT","ORDIUSDT","TSTUSDT","BABYUSDT",
                  "FHEUSDT","BUSDT","AIGENSYNUSDT","AKTUSDT","PARTIUSDT",
                  "TAGUSDT","BSBUSDT","GENIUSUSDT"};
    }

    spdlog::info("将监控 {} 个合约, 前3: {}", result.size(),
                 result.size() >= 3 ? result[0]+","+result[1]+","+result[2] : "");
    return result;
}

// 每个合约的完整状态
struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{3};
    SignalDetector detector{ml, indicators};
};

// WebSocket 线程（自动重连 + 生产级 SSL）
void run_websocket(std::map<std::string, SymbolContext>& contexts, const std::vector<std::string>& symbols) {
    while (keep_running) {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tls_client};
            ctx.set_options(ssl::context::default_workarounds |
                            ssl::context::no_sslv2 |
                            ssl::context::no_sslv3);
            // 生产级证书验证
            ctx.set_default_verify_paths();
            ctx.set_verify_mode(ssl::verify_peer);

            ws::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve("fstream.binance.com", "443");
            net::connect(ws.next_layer().next_layer(), results.begin(), results.end());
            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake("fstream.binance.com", "/stream");

            // 构造订阅流（所有 symbol 转小写）
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
                        bool isMaker = data["m"];  // true = 卖方主动
                        it->second.orderbook.add_agg_trade(!isMaker, qty);
                    }
                }
            }
        } catch (std::exception const& e) {
            spdlog::error("WebSocket 异常: {}，3 秒后重连...", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
}

int main() {
    auto symbols = fetch_top_symbols(30, 30000000.0);
    if (symbols.empty()) {
        spdlog::critical("无法获取任何合约，引擎退出");
        return 1;
    }

    std::map<std::string, SymbolContext> contexts;
    for (auto& sym : symbols) contexts.emplace(sym, SymbolContext{});

    // 启动 WebSocket 线程
    std::thread ws_thread(run_websocket, std::ref(contexts), symbols);

    // 信号检测线程（每 100ms 遍历，异常隔离）
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