#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <deque>
#include <map>
#include <nlohmann/json.hpp>
#include <cstdint>

using json = nlohmann::json;

class OrderBook {
public:
    OrderBook() = default;

    void update_depth(const json& data);
    double micro_price() const;               // 返回中间价 (best_bid+best_ask)/2
    double imbalance() const;
    double best_bid() const;
    double best_ask() const;
    double buy_volume() const;
    double sell_volume() const;
    double recent_volume(int window_ms) const;
    void add_agg_trade(bool is_buy, double volume, int64_t trade_time_ms = 0);

private:
    std::map<double, double, std::greater<double>> bids;
    std::map<double, double> asks;
    struct Trade { double volume; int64_t timestamp_ms; };
    std::deque<Trade> trades;
    double cum_buy = 0.0, cum_sell = 0.0;
    static const size_t MAX_TRADE = 5000;
};

#endif