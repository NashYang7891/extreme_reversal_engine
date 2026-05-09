#ifndef INDICATORS_H
#define INDICATORS_H

#include <deque>
#include <chrono>

class Indicators {
public:
    explicit Indicators(size_t max_size = 500);
    void update(double price);
    double price() const { return prices_.empty() ? 0.0 : prices_.back(); }
    const std::deque<double>& prices() const { return prices_; }
    double ema20() const { return ema20_; }
    double atr(int period = 14) const;
    double atr_ma(int period = 100) const;  // 新增：ATR 移动平均
    double rsi(int period = 14) const;
    double kdj_j(int period = 9) const;
    double cci(int period = 20) const;
    double composite_oscillator(double w_rsi, double w_kdj, double w_cci) const;
    double price_change_pct(int window_sec, int offset_sec = 0) const;
    double get_volume_ema() const { return vol_ema_; }
    void update_volume(double volume);
    bool is_stale(int stale_ms) const;

private:
    void update_ema();
    std::deque<double> prices_;
    size_t max_size_;
    double ema20_ = 0.0;
    double vol_ema_ = 0.0;
    static constexpr double VOL_EMA_ALPHA = 0.1;
    int64_t last_update_ms = 0;
    mutable std::deque<double> atr_history_; // 存储最近的 ATR 值，用于计算移动平均
};

#endif