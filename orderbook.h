#pragma once
#include <map>
#include <deque>
#include <chrono>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

class OrderBook {
public:
    void update_depth(const json& data);
    void add_agg_trade(bool is_buy, double volume, int64_t trade_time_ms = 0);
    double micro_price() const;
    double imbalance() const;
    double best_bid() const;
    double best_ask() const;
    double buy_volume() const;
    double sell_volume() const;
    // 获取最近 window_ms 内的累计成交量
    double recent_volume(int window_ms) const;
private:
    std::map<double, double> bids, asks;
    struct Trade { double volume; int64_t timestamp_ms; };
    std::deque<Trade> trades;
    static constexpr size_t MAX_TRADE = 1000;
    void prune();
};