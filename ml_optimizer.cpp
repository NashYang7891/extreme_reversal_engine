#include "ml_optimizer.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <spdlog/spdlog.h>

MLOptimizer::MLOptimizer(int history_size) : w_rsi_(0.4), w_kdj_(0.3), w_cci_(0.3) {
    double sum = w_rsi_ + w_kdj_ + w_cci_;
    if (sum > 0) {
        w_rsi_ /= sum; w_kdj_ /= sum; w_cci_ /= sum;
    }
}

double MLOptimizer::predict(double rsi, double kdj, double cci) const {
    double norm_rsi = std::clamp(rsi / 100.0, 0.0, 1.0);
    double norm_kdj = std::clamp(kdj / 100.0, 0.0, 1.0);
    double norm_cci = std::clamp((cci + 200.0) / 400.0, 0.0, 1.0);
    double raw = w_rsi_ * norm_rsi + w_kdj_ * norm_kdj + w_cci_ * norm_cci;
    return std::clamp(raw, 0.0, 1.0);
}

void MLOptimizer::update_result(const std::string& side, double pnl_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.push_back({side, pnl_percent});
    if (history_.size() > 200) history_.pop_front();
    optimize_weights();

    double score = std::clamp((pnl_percent + 10.0) / 20.0, 0.0, 1.0);
    recent_scores.push_back(score);
    if (recent_scores.size() > 100) recent_scores.pop_front();

    double rate = compute_success_rate(side, 20);
    spdlog::info("ML: {} 结果 {:.2f}%, 新权重 R={:.3f} K={:.3f} C={:.3f}, 成功率={:.1f}%",
                 side, pnl_percent, w_rsi_, w_kdj_, w_cci_, rate*100);
}

double MLOptimizer::get_success_rate_adjustment(const std::string& side) const {
    double rate = compute_success_rate(side, 20);
    double adj = 0.5 + rate;
    return std::clamp(adj, 0.5, 1.5);
}

double MLOptimizer::compute_success_rate(const std::string& side, int lookback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int cnt = 0, win = 0;
    for (auto it = history_.rbegin(); it != history_.rend() && cnt < lookback; ++it) {
        if (it->side == side && it->result != 0.0) {
            cnt++;
            if (it->result > 0) win++;
        }
    }
    if (cnt == 0) return 0.5;
    return static_cast<double>(win) / cnt;
}

void MLOptimizer::optimize_weights() {
    int lookback = 20;
    double avg_pnl = 0.0;
    int n = 0;
    for (auto it = history_.rbegin(); it != history_.rend() && n < lookback; ++it) {
        if (it->result != 0.0) {
            avg_pnl += it->result;
            n++;
        }
    }
    if (n == 0) return;
    avg_pnl /= n;
    if (avg_pnl < -0.5) {
        w_rsi_ *= 0.98; w_kdj_ *= 1.01; w_cci_ *= 1.01;
    } else if (avg_pnl > 1.0) {
        w_rsi_ *= 1.01; w_kdj_ *= 0.99; w_cci_ *= 0.99;
    } else {
        w_rsi_ *= 0.995; w_kdj_ *= 1.002; w_cci_ *= 1.003;
    }
    double sum = w_rsi_ + w_kdj_ + w_cci_;
    if (sum > 0) {
        w_rsi_ /= sum; w_kdj_ /= sum; w_cci_ /= sum;
    }
    w_rsi_ = std::clamp(w_rsi_, 0.1, 0.8);
    w_kdj_ = std::clamp(w_kdj_, 0.1, 0.8);
    w_cci_ = std::clamp(w_cci_, 0.1, 0.8);
}