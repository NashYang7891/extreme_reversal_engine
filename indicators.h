#pragma once
#include <deque>
#include <vector>
#include <cstddef>
#include <chrono>

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

    double price_change_pct(int window_sec, int offset_sec = 0) const;
    void update_volume(double volume);
    double get_volume_ema() const { return vol_ema_; }

    // 时效性检查：距上次更新是否超过 timeout_ms 毫秒
    bool is_stale(int64_t timeout_ms = 5000) const {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return (now_ms - last_update_ms) > timeout_ms;
    }
    int64_t get_last_update_ms() const { return last_update_ms; }

private:
    std::deque<double> prices_;
    double ema20_ = 0.0;
    size_t max_size_;
    void update_ema();
    double k_ = 50.0;
    double d_ = 50.0;
    double vol_ema_ = 0.0;
    static constexpr double VOL_EMA_ALPHA = 0.05;

    int64_t last_update_ms = 0;  // 最后一次 update 的时间戳 (ms)
};