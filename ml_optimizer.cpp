#include "ml_optimizer.h"
#include <numeric>
#include <algorithm>

MLOptimizer::MLOptimizer(int) {}

void MLOptimizer::record_outcome(double profit) {
    profit_history_.push_back(profit);
    if (profit_history_.size() > MAX_HIST) profit_history_.pop_front();
}

void MLOptimizer::optimize() {
    if (profit_history_.empty()) return;
    double avg = std::accumulate(profit_history_.begin(), profit_history_.end(), 0.0) / profit_history_.size();
    if (avg > 0) {
        w_rsi_ += learning_rate_ * 0.1; w_kdj_ += learning_rate_ * 0.1; w_cci_ += learning_rate_ * 0.1;
    } else {
        w_rsi_ -= learning_rate_ * 0.1; w_kdj_ -= learning_rate_ * 0.1; w_cci_ -= learning_rate_ * 0.1;
    }
    normalize();
}

void MLOptimizer::normalize() {
    w_rsi_ = std::max(0.05, w_rsi_); w_kdj_ = std::max(0.05, w_kdj_); w_cci_ = std::max(0.05, w_cci_);
    double total = w_rsi_ + w_kdj_ + w_cci_;
    w_rsi_ /= total; w_kdj_ /= total; w_cci_ /= total;
}