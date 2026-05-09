#ifndef SIGNAL_DETECTOR_H
#define SIGNAL_DETECTOR_H

#include "ml_optimizer.h"
#include "indicators.h"
#include "orderbook.h"

struct Signal {
    bool valid = false;
    std::string side;
    double price = 0.0;
    double score = 0.0;
};

class SignalDetector {
public:
    SignalDetector(MLOptimizer& ml, Indicators& ind);
    Signal check(const OrderBook& ob, int active_buy_count = 0, double large_buy_ratio = 0.0, double active_buy_ratio = 0.0);

private:
    MLOptimizer& ml_;
    Indicators& ind_;
    bool check_momentum_decay(const std::string& side);
};

#endif