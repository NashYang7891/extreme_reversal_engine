#ifndef INDICATORS_H
#define INDICATORS_H

#include <deque>
#include <chrono>

class Indicators {
public:
    explicit Indicators(size_t max_size = 500);

    // 核心更新接口（价格来自成交数据）
    void update(double price);

    // 价格序列和基础指标
    double price() const { return prices_.empty() ? 0.0 : prices_.back(); }
    const std::deque<double>& prices() const { return prices_; }
    double ema20() const { return ema20_; }
    
    // 技术指标（均带默认周期）
    double atr(int period = 14) const;
    double rsi(int period = 14) const;
    double kdj_j(int period = 9) const;   // 纯计算，不修改状态
    double cci(int period = 20) const;
    
    // 复合振荡器（供 ML 权重使用）
    double composite_oscillator(double w_rsi, double w_kdj, double w_cci) const;

    // 价格变化百分比（窗口秒数）
    double price_change_pct(int window_sec, int offset_sec = 0) const;

    // 成交量 EMA（用于活跃层）
    double get_volume_ema() const { return vol_ema_; }
    void update_volume(double volume);

    // 检查数据是否陈旧（超过 stale_ms 毫秒未更新）
    bool is_stale(int stale_ms) const;

private:
    void update_ema();
    
    std::deque<double> prices_;
    size_t max_size_;
    double ema20_ = 0.0;

    double vol_ema_ = 0.0;
    static constexpr double VOL_EMA_ALPHA = 0.1;

    int64_t last_update_ms = 0;
};

#endif