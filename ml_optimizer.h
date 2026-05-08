#ifndef ML_OPTIMIZER_H
#define ML_OPTIMIZER_H

#include <vector>
#include "orderbook.h"   // 引用真正的 OrderBook 定义

class MLOptimizer {
public:
    MLOptimizer(int history_length = 3);
    ~MLOptimizer();

    double predict(const OrderBook& orderbook);
    void update(const OrderBook& orderbook, double result);
    double get_score() const;

private:
    int history_len;
    std::vector<double> recent_scores;
    // 其他内部实现...
};

#endif