#include "indicators.h"
#include <cmath>
#include <numeric>
#include <algorithm>

Indicators::Indicators(size_t max_size) : max_size_(max_size) {}

void Indicators::update(double micro_price) {
    prices_.push_back(micro_price);
    if (prices_.size() > max_size_) prices_.pop_front();
    update_ema();
}

void Indicators::update_ema() {
    if (prices_.empty()) return;
    const double alpha = 2.0 / 21.0;
    ema20_ = (ema20_ == 0.0) ? prices_.back() : (alpha * prices_.back() + (1-alpha) * ema20_);
}

double Indicators::atr(int period) const {
    if (prices_.size() < period + 1) return 0.0;
    double sum = 0.0;
    for (size_t i = prices_.size() - period; i < prices_.size(); ++i)
        sum += std::abs(prices_[i] - prices_[i-1]);
    return sum / period;
}

double Indicators::rsi(int period) const {
    if (prices_.size() < period + 1) return 50.0;
    double gain = 0.0, loss = 0.0;
    for (size_t i = prices_.size() - period; i < prices_.size(); ++i) {
        double diff = prices_[i] - prices_[i-1];
        if (diff > 0) gain += diff; else loss -= diff;
    }
    double avg_gain = gain / period, avg_loss = loss / period;
    if (avg_loss == 0) return 100.0;
    double rs = avg_gain / avg_loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

double Indicators::kdj_j(int period) const {
    if (prices_.size() < period) return 50.0;
    double low = *std::min_element(prices_.end() - period, prices_.end());
    double high = *std::max_element(prices_.end() - period, prices_.end());
    if (high == low) return 50.0;
    double rsv = (prices_.back() - low) / (high - low) * 100.0;
    static double k = 50.0, d = 50.0;
    k = 0.6667 * k + 0.3333 * rsv;
    d = 0.6667 * d + 0.3333 * k;
    return 3.0 * k - 2.0 * d;
}

double Indicators::cci(int period) const {
    if (prices_.size() < period) return 0.0;
    std::vector<double> tp(prices_.end() - period, prices_.end());
    double ma = std::accumulate(tp.begin(), tp.end(), 0.0) / tp.size();
    double md = 0.0;
    for (double v : tp) md += std::abs(v - ma);
    md /= tp.size();
    if (md == 0) return 0.0;
    return (tp.back() - ma) / (0.015 * md);
}

double Indicators::composite_oscillator(double w_rsi, double w_kdj, double w_cci) const {
    double r = rsi() / 100.0;
    double k = kdj_j() / 100.0;
    double c = (cci() + 200.0) / 400.0;
    return w_rsi * r + w_kdj * k + w_cci * c;
}