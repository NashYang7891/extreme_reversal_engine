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
private:
    std::deque<double> prices_;
    double ema20_ = 0.0;
    size_t max_size_;
    void update_ema();
    double k_ = 50.0;
    double d_ = 50.0;
};