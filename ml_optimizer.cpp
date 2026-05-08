#include "ml_optimizer.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <spdlog/spdlog.h>

MLOptimizer::MLOptimizer(int history_size) 
    : w_rsi_(0.4), w_kdj_(0.3), w_cci_(0.3) {
    double sum = w_rsi_ + w_kdj_ + w_cci_;
    w_rsi_ /= sum; w_kdj_ /= sum; w_cci_ /= sum;
}

double MLOptimizer::predict(double rsi, double kdj, double cci) const {
    // 归一化输入
    double norm_rsi = std::clamp(rsi / 100.0, 0.0, 1.0);
    double norm_kdj = std::clamp((kdj) / 100.0, 0.0, 1.0);   // 假设kdj在0~100
    double norm_cci = std::clamp((cci + 200.0) / 400.0, 0.0, 1.0);
    double raw = w_rsi_ * norm_rsi + w_kdj_ * norm_kdj + w_cci_ * norm_cci;
    return std::clamp(raw, 0.0, 1.0);
}

void MLOptimizer::update_result(const std::string& side, double pnl_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 找到最近一条同方向且结果==0的记录进行填充
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->side == side && it->result == 0.0) {
            it->result = pnl_percent;
            break;
        }
    }
    // 也可以直接添加新记录（如果是外部直接调用）
    // 保证历史长度
    if (history_.size() > 200) history_.pop_front();
    optimize_weights();
    spdlog::info("ML: 更新 {} 结果 {:.2f}%, 新权重 R={:.3f} K={:.3f} C={:.3f}", 
                 side, pnl_percent, w_rsi_, w_kdj_, w_cci_);
}

double MLOptimizer::get_success_rate_adjustment(const std::string& side) const {
    double rate = compute_success_rate(side, 20);
    // 成功率 30% -> 0.7, 50% -> 1.0, 80% -> 1.3，范围0.5~1.5
    double adj = 0.5 + rate;   // rate 0~1
    adj = std::clamp(adj, 0.5, 1.5);
    return adj;
}

double MLOptimizer::compute_success_rate(const std::string& side, int lookback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int cnt = 0, win = 0;
    for (auto it = history_.rbegin(); it != history_.rend() && cnt < lookback; ++it) {
        if (it->side == side && it->result != 0.0) {
            cnt++;
            if (it->result > 0.0) win++;
        }
    }
    if (cnt == 0) return 0.5;
    return static_cast<double>(win) / cnt;
}

void MLOptimizer::optimize_weights() {
    // 基于最近20笔交易的盈亏调整权重（简单爬山法）
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

    // 如果平均亏损，则降低当前主要权重（RSI）并提高另两个
    if (avg_pnl < -0.5) {
        w_rsi_ *= 0.98;
        w_kdj_ *= 1.01;
        w_cci_ *= 1.01;
    } else if (avg_pnl > 1.0) {
        // 盈利好，增强主要权重
        w_rsi_ *= 1.01;
        w_kdj_ *= 0.99;
        w_cci_ *= 0.99;
    } else {
        // 轻微调整
        w_rsi_ *= 0.995;
        w_kdj_ *= 1.002;
        w_cci_ *= 1.003;
    }
    double sum = w_rsi_ + w_kdj_ + w_cci_;
    w_rsi_ /= sum; w_kdj_ /= sum; w_cci_ /= sum;
    w_rsi_ = std::clamp(w_rsi_, 0.1, 0.8);
    w_kdj_ = std::clamp(w_kdj_, 0.1, 0.8);
    w_cci_ = std::clamp(w_cci_, 0.1, 0.8);
}