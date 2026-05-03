#pragma once
#include "orderbook.h"
#include "indicators.h"
#include "ml_optimizer.h"

struct Signal { bool valid = false; std::string side; double price; double score; };

class SignalDetector {
public:
    SignalDetector(MLOptimizer& ml, Indicators& ind);
    Signal check(const OrderBook& ob);
private:
    MLOptimizer& ml_;
    Indicators& ind_;
    double solve_critical_price(const OrderBook& ob, const std::string& side);
    double objective(const OrderBook& ob, double price, const std::string& side);
    bool check_momentum_slowing(const std::string& side);
};