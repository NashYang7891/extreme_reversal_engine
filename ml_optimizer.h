#ifndef ML_OPTIMIZER_H
#define ML_OPTIMIZER_H

#include <string>
#include <deque>
#include <mutex>

class MLOptimizer {
public:
    MLOptimizer(int history_size = 100);
    ~MLOptimizer() = default;

    // 预测综合得分（基于当前订单簿和指标）
    double predict(double rsi, double kdj, double cci) const;

    // 更新交易结果（side: "LONG"/"SHORT", pnl_percent: 盈亏百分比，正为盈利）
    void update_result(const std::string& side, double pnl_percent);

    // 获取当前权重
    double get_w_rsi() const { return w_rsi_; }
    double get_w_kdj() const { return w_kdj_; }
    double get_w_cci() const { return w_cci_; }

    // 获取同方向最近成功率调整系数（0.5~1.5）
    double get_success_rate_adjustment(const std::string& side) const;

    // 获取当前综合评分（给信号检测器用）
    double get_score() const { return recent_scores.empty() ? 0.5 : recent_scores.back(); }

private:
    // 权重（可学习）
    double w_rsi_;
    double w_kdj_;
    double w_cci_;

    // 历史记录
    struct SignalRecord {
        std::string side;
        double result;   // 盈亏百分比，0表示未平仓
    };
    std::deque<SignalRecord> history_;

    mutable std::mutex mutex_;

    void optimize_weights(); // 根据历史结果调整权重
    double compute_success_rate(const std::string& side, int lookback = 20) const;
};

#endif