#include "indicators.h"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <spdlog/spdlog.h>

Indicators::Indicators(size_t max_size) : max_size_(max_size) {}

void Indicators::update(double price) {
    if (price <= 0) return;
    prices_.push_back(price);
    if (prices_.size() > max_size_) prices_.pop_front();
    update_ema();
    // 计算 ATR 并存储历史
    if (prices_.size() > 14) {
        double atr_val = atr(14);
        atr_history_.push_back(atr_val);
        if (atr_history_.size() > 200) atr_history_.pop_front();
    }
    auto now = std::chrono::system_clock::now().time_since_epoch();
    last_update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void Indicators::update_ema() {
    if (prices_.empty()) return;
    double alpha = 2.0 / 21.0;
    if (ema20_ == 0.0) ema20_ = prices_.back();
    else ema20_ = alpha * prices_.back() + (1.0 - alpha) * ema20_;
}

double Indicators::atr(int period) const {
    if (prices_.size() < static_cast<size_t>(period + 1)) return 0.0;
    double sum = 0.0;
    size_t start = prices_.size() - period;
    for (size_t i = start; i < prices_.size(); ++i)
        sum += std::abs(prices_[i] - prices_[i-1]);
    return sum / period;
}

double Indicators::atr_ma(int period) const {
    if (atr_history_.size() < static_cast<size_t>(period)) return atr(14); // 回退
    double sum = 0.0;
    auto it = atr_history_.rbegin();
    for (int i = 0; i < period && it != atr_history_.rend(); ++i, ++it)
        sum += *it;
    return sum / period;
}

double Indicators::rsi(int period) const {
    if (prices_.size() < static_cast<size_t>(period + 1)) return 50.0;
    double gain = 0.0, loss = 0.0;
    size_t start = prices_.size() - period;
    for (size_t i = start; i < prices_.size(); ++i) {
        double diff = prices_[i] - prices_[i-1];
        if (diff > 0) gain += diff;
        else if (diff < 0) loss -= diff;
    }
    double avg_gain = gain / period, avg_loss = loss / period;
    if (avg_loss == 0.0) return 100.0;
    double rs = avg_gain / avg_loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

double Indicators::kdj_j(int period) const {
    if (prices_.size() < static_cast<size_t>(period)) return 50.0;
    auto start = prices_.end() - period;
    double low = *std::min_element(start, prices_.end());
    double high = *std::max_element(start, prices_.end());
    if (high == low) return 50.0;
    double rsv = (prices_.back() - low) / (high - low) * 100.0;
    double k = rsv, d = rsv;
    return 3.0 * k - 2.0 * d;
}

double Indicators::cci(int period) const {
    if (prices_.size() < static_cast<size_t>(period)) return 0.0;
    auto start = prices_.end() - period;
    std::vector<double> tp(start, prices_.end());
    double ma = std::accumulate(tp.begin(), tp.end(), 0.0) / tp.size();
    double md = 0.0;
    for (double v : tp) md += std::abs(v - ma);
    md /= tp.size();
    if (md == 0.0) return 0.0;
    return (tp.back() - ma) / (0.015 * md);
}

double Indicators::composite_oscillator(double w_rsi, double w_kdj, double w_cci) const {
    double r = std::clamp(rsi(14) / 100.0, 0.0, 1.0);
    double k = std::clamp(kdj_j(9) / 100.0, 0.0, 1.0);
    double c = std::clamp((cci(20) + 200.0) / 400.0, 0.0, 1.0);
    return w_rsi * r + w_kdj * k + w_cci * c;
}

double Indicators::price_change_pct(int window_sec, int offset_sec) const {
    size_t total = prices_.size();
    if (total < static_cast<size_t>(window_sec + offset_sec + 1)) return 0.0;
    size_t cur = total - 1 - offset_sec;
    size_t prev = cur - window_sec;
    double prev_price = prices_[prev];
    if (prev_price == 0.0) return 0.0;
    return (prices_[cur] - prev_price) / prev_price;
}

void Indicators::update_volume(double volume) {
    if (vol_ema_ == 0.0) vol_ema_ = volume;
    else vol_ema_ = VOL_EMA_ALPHA * volume + (1.0 - VOL_EMA_ALPHA) * vol_ema_;
}

bool Indicators::is_stale(int stale_ms) const {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - last_update_ms) > stale_ms;
}