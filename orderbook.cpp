#include "orderbook.h"
#include <spdlog/spdlog.h>
#include <numeric>
#include <chrono>

void OrderBook::update_trade(const json& data) {
    try {
        int64_t trade_time = data["T"].get<int64_t>();
        double price = std::stod(data["p"].get<std::string>());
        double qty = std::stod(data["q"].get<std::string>());
        bool is_buyer_maker = data["m"].get<bool>();

        double notional = price * qty;

        if (is_buyer_maker) {
            cum_sell_ += notional;
        } else {
            cum_buy_ += notional;
        }

        trades.push_back({price, notional, trade_time});
        last_price_ = price;
        prune();
    } catch (const std::exception& e) {
        static int err_cnt = 0;
        if (++err_cnt % 100 == 1)
            spdlog::warn("成交数据解析异常: {}，已丢弃", e.what());
    }
}

void OrderBook::prune() {
    while (trades.size() > MAX_TRADE) {
        auto& oldest = trades.front();
        if (oldest.timestamp_ms < trades.back().timestamp_ms - 3600000) {
            trades.pop_front();
        } else {
            break;
        }
    }
}

double OrderBook::recent_volume(int window_ms) const {
    if (trades.empty()) return 0.0;
    int64_t now_ms = trades.back().timestamp_ms;
    double sum = 0.0;
    for (auto it = trades.rbegin(); it != trades.rend(); ++it) {
        if (now_ms - it->timestamp_ms <= window_ms)
            sum += it->volume;
        else
            break;
    }
    return sum;
}