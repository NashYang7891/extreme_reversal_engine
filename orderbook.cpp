#include "orderbook.h"
#include <numeric>

void OrderBook::update_depth(const json& data) {
    bids.clear(); asks.clear();
    if (data.contains("b")) {                   // 币安用 "b" 表示 bids
        for (auto& b : data["b"]) {
            double price = std::stod(b[0].get<std::string>());
            double qty   = std::stod(b[1].get<std::string>());
            bids[price] = qty;
        }
    }
    if (data.contains("a")) {                   // 币安用 "a" 表示 asks
        for (auto& a : data["a"]) {
            double price = std::stod(a[0].get<std::string>());
            double qty   = std::stod(a[1].get<std::string>());
            asks[price] = qty;
        }
    }
}

void OrderBook::add_agg_trade(bool is_buy, double volume) {
    if (is_buy) {
        buy_trades.push_back(volume);
        cum_buy += volume;
    } else {
        sell_trades.push_back(volume);
        cum_sell += volume;
    }
    prune();
}

void OrderBook::prune() {
    while (buy_trades.size() > MAX_TRADE) {
        cum_buy -= buy_trades.front();
        buy_trades.pop_front();
    }
    while (sell_trades.size() > MAX_TRADE) {
        cum_sell -= sell_trades.front();
        sell_trades.pop_front();
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