#pragma once
#include <deque>

class MLOptimizer {
public:
    MLOptimizer(int param_count = 3);
    void record_outcome(double profit);
    void optimize();
    double get_w_rsi() const { return w_rsi_; }
    double get_w_kdj() const { return w_kdj_; }
    double get_w_cci() const { return w_cci_; }
private:
    double w_rsi_ = 0.33, w_kdj_ = 0.33, w_cci_ = 0.34;
    std::deque<double> profit_history_;
    static constexpr size_t MAX_HIST = 50;
    double learning_rate_ = 0.01;
    void normalize();
};