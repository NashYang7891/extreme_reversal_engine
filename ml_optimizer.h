#ifndef ML_OPTIMIZER_H
#define ML_OPTIMIZER_H

#include <string>
#include <deque>
#include <mutex>

class MLOptimizer {
public:
    MLOptimizer(int history_size = 100);
    double predict(double rsi, double kdj, double cci) const;
    void update_result(const std::string& side, double pnl_percent);
    double get_w_rsi() const { return w_rsi_; }
    double get_w_kdj() const { return w_kdj_; }
    double get_w_cci() const { return w_cci_; }
    double get_success_rate_adjustment(const std::string& side) const;
    double get_score() const { return recent_scores.empty() ? 0.5 : recent_scores.back(); }

private:
    double w_rsi_, w_kdj_, w_cci_;
    struct SignalRecord { std::string side; double result; };
    std::deque<SignalRecord> history_;
    mutable std::mutex mutex_;
    std::deque<double> recent_scores;
    void optimize_weights();
    double compute_success_rate(const std::string& side, int lookback = 20) const;
};

#endif