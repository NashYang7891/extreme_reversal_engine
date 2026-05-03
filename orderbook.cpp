#include "orderbook.h"
#include <numeric>
#include <cstdint>

void OrderBook::update_depth(const json& data) {
    bids.clear(); asks.clear();
    if (data.contains("b")) {
        for (auto& b : data["b"]) {
            double price = std::stod(b[0].get<std::string>());
            double qty   = std::stod(b[1].get<std::string>());
            bids[price] = qty;
        }
    }
    if (data.contains("a")) {
        for (auto& a : data["a"]) {
            double price = std::stod(a[0].get<std::string>());
            double qty   = std::stod(a[1].get<std::string>());
            asks[price] = qty;
        }
    }
}

void OrderBook::add_agg_trade(bool is_buy, double volume, int64_t trade_time_ms) {
    if (trade_time_ms == 0) {
        trade_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    trades.push_back({volume, trade_time_ms});
    prune();
}

void OrderBook::prune() {
    while (trades.size() > MAX_TRADE) {
        trades.pop_front();
    }
}

double OrderBook::micro_price() const {
    if (bids.empty() || asks.empty()) return 0.0;
    double best_bid_ = bids.rbegin()->first;
    double best_ask_ = asks.begin()->first;
    double bv = buy_volume(), sv = sell_volume();
    double total = bv + sv;
    if (total == 0) return (best_bid_ + best_ask_) / 2.0;
    return (best_ask_ * bv + best_bid_ * sv) / total;
}

double OrderBook::imbalance() const {
    double bv = buy_volume(), sv = sell_volume();
    double total = bv + sv;
    return total == 0 ? 0.5 : bv / total;
}

double OrderBook::best_bid() const { return bids.empty() ? 0.0 : bids.rbegin()->first; }
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
        else
            break;
    }
    return sum;
}