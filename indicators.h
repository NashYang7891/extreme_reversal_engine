#pragma once
#include <deque>
#include <vector>
#include <cstddef>

class Indicators {
public:
    Indicators(size_t max_size = 300);
    void update(double micro_price);
    double atr(int period = 14) const;
    double rsi(int period = 14) const;
    double kdj_j(int period = 9);
    double cci(int period = 20) const;
    double ema20() const;
    double composite_oscillator(double w_rsi, double w_kdj, double w_cci);
    const std::deque<double>& prices() const { return prices_; }
    double price() const { return prices_.empty() ? 0.0 : prices_.back(); }

    // 窗口涨跌幅：当前价与 offset_sec 前再往前 window_sec 的价格比较
    double price_change_pct(int window_sec, int offset_sec = 0) const;

    // 成交量指数移动平均
    void update_volume(double volume);
    double get_volume_ema() const { return vol_ema_; }

private:
    std::deque<double> prices_;
    double ema20_ = 0.0;
    size_t max_size_;
    void update_ema();
    double k_ = 50.0;
    double d_ = 50.0;
    double vol_ema_ = 0.0;
    static constexpr double VOL_EMA_ALPHA = 0.05;
};