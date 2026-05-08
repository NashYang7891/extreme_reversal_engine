#ifndef SIGNAL_DETECTOR_H
#define SIGNAL_DETECTOR_H

#include "ml_optimizer.h"
#include "indicators.h"
#include "orderbook.h"

struct Signal {
    bool valid = false;
    std::string side;   // "LONG" or "SHORT"
    double price = 0.0;
    double score = 0.0;
};

class SignalDetector {
public:
    SignalDetector(MLOptimizer& ml, Indicators& ind);

    // 主检测函数
    Signal check(const OrderBook& ob);

private:
    MLOptimizer& ml_;
    Indicators& ind_;

    // 辅助函数：动量衰减检测（不依赖订单簿）
    bool check_momentum_decay(const std::string& side);
};

#endif