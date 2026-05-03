#pragma once
#include <map>
#include <deque>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

class OrderBook {
public:
    void update_depth(const json& data);       // 币安 depth@100ms
    void add_agg_trade(bool is_buy, double volume); // 聚合成交（买方主动 = true）
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