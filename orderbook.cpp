#include "orderbook.h"
#include <numeric>
#include <spdlog/spdlog.h>

void OrderBook::update_depth(const json& data) {
    try {
        const json* bids_json = nullptr;
        const json* asks_json = nullptr;
        if (data.contains("b") && data.contains("a")) {
            bids_json = &data["b"];
            asks_json = &data["a"];
        } else if (data.contains("bids") && data.contains("asks")) {
            bids.clear(); asks.clear();
            bids_json = &data["bids"];
            asks_json = &data["asks"];
        } else {
            static int err_count = 0;
            if (++err_count % 100 == 1)
                spdlog::warn("无法识别的深度推送格式，已跳过");
            return;
        }
        if (bids_json) {
            for (auto& item : *bids_json) {
                try {
                    double price = std::stod(item[0].get<std::string>());
                    double qty   = std::stod(item[1].get<std::string>());
                    if (qty > 0) bids[price] = qty;
                    else bids.erase(price);
                } catch (...) {}
            }
        }
        if (asks_json) {
            for (auto& item : *asks_json) {
                try {
                    double price = std::stod(item[0].get<std::string>());
                    double qty   = std::stod(item[1].get<std::string>());
                    if (qty > 0) asks[price] = qty;
                    else asks.erase(price);
                } catch (...) {}
            }
        }
    } catch (const std::exception& e) {
        static int err_cnt = 0;
        if (++err_cnt % 100 == 1)
            spdlog::warn("深度解析异常: {}，已丢弃本条数据", e.what());
    }
}

double OrderBook::micro_price() const {
    if (bids.empty() || asks.empty()) return 0.0;
    double best_bid_ = bids.begin()->first;
    double best_ask_ = asks.begin()->first;
    return (best_bid_ + best_ask_) / 2.0;
}

double OrderBook::imbalance() const {
    double bv = buy_volume();
    double sv = sell_volume();
    double total = bv + sv;
    if (total <= 1e-12) return 0.5;
    return bv / total;
}

double OrderBook::best_bid() const { return bids.empty() ? 0.0 : bids.begin()->first; }
double OrderBook::best_ask() const { return asks.empty() ? 0.0 : asks.begin()->first; }

double OrderBook::buy_volume() const {
    return std::accumulate(bids.begin(), bids.end(), 0.0,
        [](double sum, const auto& p) { return sum + p.second; });
}
double OrderBook::sell_volume() const {
    return std::accumulate(asks.begin(), asks.end(), 0.0,
        [](double sum, const auto& p) { return sum + p.second; });
}

double OrderBook::recent_volume(int window_ms) const {
    if (trades.empty()) return 0.0;
    int64_t now_ms = trades.back().timestamp_ms;
    double sum = 0.0;
    for (auto it = trades.rbegin(); it != trades.rend(); ++it) {
        if (now_ms - it->timestamp_ms <= window_ms)
            sum += it->volume;
        else break;
    }
    return sum;
}

void OrderBook::add_agg_trade(bool is_buy, double volume, int64_t trade_time_ms) {
    if (trade_time_ms == 0) {
        trade_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    trades.push_back({volume, trade_time_ms});
    if (is_buy) cum_buy += volume;
    else cum_sell += volume;
    while (trades.size() > MAX_TRADE) trades.pop_front();
}