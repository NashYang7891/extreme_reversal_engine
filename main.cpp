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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "orderbook.h"
#include "indicators.h"
#include "ml_optimizer.h"
#include "signal_detector.h"

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
    "BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","DOGEUSDT","BNBUSDT"
};

std::vector<std::string> fetch_top_symbols(int top_n = 100, double min_vol = 80000000.0) {
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
            } catch (...) { spdlog::error("解析 ticker 失败，使用兜底列表"); }
        } else {
            spdlog::error("获取 ticker 失败: {}，使用兜底列表", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    } else {
        spdlog::error("curl 初始化失败，使用兜底列表");
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
    spdlog::info("最终监控 {} 个合约 (24h成交额≥8000万U), 前3: {}", result.size(),
                 result.size()>=3 ? result[0]+","+result[1]+","+result[2] : "");
    return result;
}

struct SymbolContext {
    OrderBook orderbook;
    Indicators indicators;
    MLOptimizer ml{100};
    SignalDetector detector{ml, indicators};
    std::atomic<int64_t> last_active_time{0};
    std::atomic<int64_t> last_b_push_ms{0};
    std::atomic<double> last_active_change{0.0};
    double highest_since_reset = 0.0;
    double lowest_since_reset = std::numeric_limits<double>::max();
    int consecutive_highs = 0;
    int consecutive_lows = 0;

    // 成交指标
    std::atomic<int> active_buy_count{0};
    std::atomic<int> active_sell_count{0};
    std::atomic<double> large_buy_ratio{0.0};
    std::atomic<double> active_buy_ratio{0.0};
    std::atomic<int64_t> last_metrics_time{0};
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
            if (mp > 0) {
                it->second.indicators.update(mp);
                auto& ctx = it->second;
                if (mp > ctx.highest_since_reset) {
                    ctx.highest_since_reset = mp;
                    ctx.consecutive_highs++;
                    ctx.consecutive_lows = 0;
                } else if (mp < ctx.lowest_since_reset) {
                    ctx.lowest_since_reset = mp;
                    ctx.consecutive_lows++;
                    ctx.consecutive_highs = 0;
                } else {
                    ctx.consecutive_highs = 0;
                    ctx.consecutive_lows = 0;
                }
            }
        }
    } catch (const std::exception& e) {
        static int cnt = 0;
        if (++cnt % 100 == 1)
            spdlog::warn("数据处理异常 [{}]: {}", sym, e.what());
    }
}

void run_websocket(const std::vector<std::string>& symbols) {
    while (keep_running) {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tlsv12_client};
            ctx.set_verify_mode(ssl::verify_peer);
            ctx.set_default_verify_paths();

            ws::stream<beast::ssl_stream<tcp::socket>> ws_stream(ioc, ctx);
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve("fstream.binance.com", "443");
            net::connect(ws_stream.next_layer().next_layer(), results.begin(), results.end());

            SSL_set_tlsext_host_name(ws_stream.next_layer().native_handle(), "fstream.binance.com");

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
            spdlog::info("WebSocket 连接成功，已订阅 {} 个 depth 流", streams.size());

            ws_stream.control_callback(
                [&](beast::websocket::frame_type kind, beast::string_view payload) {
                    if (kind == beast::websocket::frame_type::ping) {
                        spdlog::debug("收到 Ping，回复 Pong");
                        beast::websocket::ping_data pdata(payload.data(), payload.size());
                        ws_stream.pong(pdata);
                    }
                });

            beast::flat_buffer buffer;
            while (keep_running) {
                ws_stream.read(buffer);
                auto msg_str = beast::buffers_to_string(buffer.data());
                buffer.clear();
                try {
                    auto msg = json::parse(msg_str);
                    process_json_msg(msg);
                } catch (const std::exception& e) {
                    spdlog::error("JSON 解析失败: {}", e.what());
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("WebSocket 异常: {}，10秒后重连", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
}

// 读取成交指标管道
void trade_metrics_reader() {
    const char* pipe_path = "/tmp/trade_metrics_pipe";
    while (keep_running) {
        int fd = open(pipe_path, O_RDONLY);
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        char buf[4096];
        std::string leftover;
        while (keep_running) {
            int n = read(fd, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                std::string data = leftover + std::string(buf);
                size_t pos = 0;
                while ((pos = data.find('\n')) != std::string::npos) {
                    std::string line = data.substr(0, pos);
                    data.erase(0, pos+1);
                    try {
                        json j = json::parse(line);
                        if (j.contains("symbol")) {
                            std::string sym = j["symbol"];
                            int active_buy = j.value("active_buy_count", 0);
                            int active_sell = j.value("active_sell_count", 0);
                            double large_ratio = j.value("large_buy_ratio", 0.0);
                            double buy_ratio = j.value("active_buy_ratio", 0.0);
                            int64_t ts = j.value("timestamp", 0LL);

                            std::shared_lock lock(contexts_mutex);
                            auto it = contexts.find(sym);
                            if (it != contexts.end()) {
                                it->second.active_buy_count = active_buy;
                                it->second.active_sell_count = active_sell;
                                it->second.large_buy_ratio = large_ratio;
                                it->second.active_buy_ratio = buy_ratio;
                                it->second.last_metrics_time = ts;
                                spdlog::debug("成交指标 {}: 主动买={}, 大单比={:.2f}, 主动买比={:.2f}",
                                              sym, active_buy, large_ratio, buy_ratio);
                            }
                        }
                    } catch (const std::exception& e) {
                        spdlog::warn("成交指标解析失败: {}", e.what());
                    }
                }
                leftover = data;
            } else if (n == 0) {
                break;
            }
        }
        close(fd);
    }
}

void feedback_listener() {
    const char* fifo_path = "/tmp/quant_feedback";
    mkfifo(fifo_path, 0666);
    while (keep_running) {
        int fd = open(fifo_path, O_RDONLY);
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        char buf[1024];
        int n = read(fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            try {
                json j = json::parse(buf);
                std::string sym = j.value("symbol", "");
                std::string side = j.value("side", "");
                double pnl = j.value("pnl", 0.0);
                if (!sym.empty() && !side.empty()) {
                    std::shared_lock lock(contexts_mutex);
                    auto it = contexts.find(sym);
                    if (it != contexts.end()) {
                        it->second.ml.update_result(side, pnl);
                        spdlog::info("反馈收到: {} {} 盈亏={:.2f}%", sym, side, pnl);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("反馈解析失败: {}", e.what());
            }
        }
        close(fd);
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
                        ctx.last_active_change = change_pct;

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
                            // ---------- 成交指标过滤（阈值可调） ----------
                            bool metrics_ok = false;
                            if (sig.side == "LONG") {
                                // 做多：主动买笔数 > 20 且 大单占比 > 0.05（放宽至5%）
                                if (ctx.active_buy_count > 20 && ctx.large_buy_ratio > 0.05) {
                                    metrics_ok = true;
                                } else {
                                    spdlog::info("[SIGNAL_REJECTED] {} LONG Filtered: BuyCount={}, LargeRatio={:.3f}",
                                                 sym, ctx.active_buy_count.load(), ctx.large_buy_ratio.load());
                                }
                            } else { // SHORT
                                // 做空：主动买入笔数 < 25 且 主动买入占比 < 0.55（放宽至55%）
                                if (ctx.active_buy_count < 25 && ctx.active_buy_ratio < 0.55) {
                                    metrics_ok = true;
                                } else {
                                    spdlog::info("[SIGNAL_REJECTED] {} SHORT Filtered: BuyCount={}, ActiveRatio={:.3f}",
                                                 sym, ctx.active_buy_count.load(), ctx.active_buy_ratio.load());
                                }
                            }

                            if (!metrics_ok) {
                                // 不满足指标，丢弃信号，不推送
                                continue;
                            }
                            // ---------- 原有信号方向检查 ----------
                            if (sig.side == "LONG" && ctx.last_active_change.load() > -0.025)
                                continue;
                            if (sig.side == "SHORT" && ctx.last_active_change.load() < 0.012)
                                continue;

                            if (now_ms - ctx.last_b_push_ms.load() < 10000) continue;
                            ctx.last_b_push_ms = now_ms;

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

                            if (sig.side == "LONG") {
                                double profit_dist = std::max(atr * 8.0, p * 0.025);
                                b_msg["take_profit"] = p + profit_dist;
                                double stop_dist = std::max(atr * 3.0, p * 0.015);
                                if (stop_dist > p * 0.04) stop_dist = p * 0.04;
                                b_msg["stop_loss"] = p - stop_dist;
                                if (b_msg["take_profit"] <= p) b_msg["take_profit"] = p * 1.02;
                                if (b_msg["stop_loss"] < p * 0.95) b_msg["stop_loss"] = p * 0.95;
                            } else {
                                double profit_dist = std::max(atr * 5.0, p * 0.03);
                                b_msg["take_profit"] = p - profit_dist;
                                double stop_dist_short = std::max(atr * 3.0, p * 0.015);
                                if (stop_dist_short > p * 0.04) stop_dist_short = p * 0.04;
                                b_msg["stop_loss"] = p + stop_dist_short;
                                if (b_msg["stop_loss"] > p * 1.08) b_msg["stop_loss"] = p * 1.08;
                                if (b_msg["take_profit"] >= p) b_msg["take_profit"] = p - profit_dist;
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
    setvbuf(stdout, NULL, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_level(spdlog::level::info);  // 可改为 debug 查看指标值
    spdlog::info(">>> 极端反转引擎 [深度流+中间价+在线学习+成交指标] 启动...");

    auto symbols = fetch_top_symbols(100, 80000000.0);
    {
        std::unique_lock lock(contexts_mutex);
        for (const auto& sym : symbols)
            contexts.emplace(std::piecewise_construct, std::forward_as_tuple(sym), std::forward_as_tuple());
    }
    spdlog::info("引擎启动，监控 {} 个合约", symbols.size());

    std::thread ws_thread(run_websocket, symbols);
    std::thread detect_thread(run_detection);
    std::thread feedback_thread(feedback_listener);
    std::thread metrics_thread(trade_metrics_reader);
    spdlog::info("✅ 所有线程已启动 (深度流 + 在线学习 + 成交指标)");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        spdlog::info("💓 保活心跳, 监控 {} 个合约", contexts.size());
    }

    if (ws_thread.joinable()) ws_thread.join();
    if (detect_thread.joinable()) detect_thread.join();
    if (feedback_thread.joinable()) feedback_thread.join();
    if (metrics_thread.joinable()) metrics_thread.join();
    return 0;
}