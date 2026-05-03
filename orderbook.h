#pragma once
#include <map>
#include <deque>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

class OrderBook {
public:
    void update_depth(const json& data);
    void add_agg_trade(bool is_buy, double volume);
    double micro_price() const;
    double imbalance() const;
    double best_bid() const;
    double best_ask() const;
    double buy_volume() const;
    double sell_volume() const;
private:
    std::map<double, double> bids, asks;
    std::deque<double> buy_trades, sell_trades;
    double cum_buy = 0, cum_sell = 0;
    static constexpr size_t MAX_TRADE = 50;
    void prune();
};