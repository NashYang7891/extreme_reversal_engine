#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <nlohmann/json.hpp>

class OrderBook {
public:
    void update_trade(const nlohmann::json&) {}
    double last_price() const { return 0.0; }
    double recent_volume(int) const { return 0.0; }
    double buy_volume() const { return 0.0; }
    double sell_volume() const { return 0.0; }
};

#endif