#include "signal_detector.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

SignalDetector::SignalDetector(MLOptimizer& ml, Indicators& ind) : ml_(ml), ind_(ind) {}

static bool consecutive_accel(const std::deque<double>& prices, int count, bool positive) {
    if (prices.size() < count + 3) return false;
    for (size_t i = prices.size() - count; i < prices.size(); ++i) {
        double v0 = prices[i] - prices[i-1];
        double v1 = prices[i-1] - prices[i-2];
        double a = v0 - v1;
        if ((positive && a <= 0) || (!positive && a >= 0)) return false;
    }
    return true;
}

bool SignalDetector::check_momentum_decay(const std::string& side) {
    const auto& prices = ind_.prices();
    if (prices.size() < 7) return false;
    double v0 = prices.back() - prices[prices.size()-2];
    double v1 = prices[prices.size()-2] - prices[prices.size()-3];
    double accel0 = v0 - v1;
    double accel1 = v1 - (prices[prices.size()-3] - prices[prices.size()-4]);

    bool price_rising = prices.back() > prices[prices.size()-5];
    bool price_falling = prices.back() < prices[prices.size()-5];

    if (side == "LONG") {
        bool is_low = true;
        double cur = prices.back();
        for (size_t i = prices.size()-5; i < prices.size()-1; ++i)
            if (prices[i] < cur) { is_low = false; break; }
        return (accel0 > 0 && accel1 > 0) || (price_rising && is_low);
    } else {
        bool is_high = true;
        double cur = prices.back();
        for (size_t i = prices.size()-5; i < prices.size()-1; ++i)
            if (prices[i] > cur) { is_high = false; break; }
        return (accel0 < 0 && accel1 < 0) || (price_falling && is_high);
    }
}

Signal SignalDetector::check(const OrderBook& ob) {
    Signal sig;
    if (ind_.prices().size() < 60) return sig;
    double atr = ind_.atr();
    if (atr <= 0) return sig;
    double ema20 = ind_.ema20();
    double price = ob.last_price();
    if (price <= 0) return sig;

    double dev = (ema20 - price) / atr;
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());

    double buy_vol = ob.buy_volume();
    double sell_vol = ob.sell_volume();
    double total_vol = buy_vol + sell_vol;
    double wall = (total_vol > 1e-9) ? (buy_vol / total_vol) : 0.5;
    wall = std::clamp(wall, 0.001, 0.999);

    constexpr double LONG_DEV_THRESH = 3.0;
    constexpr double LONG_OSC_MAX = 0.18;
    constexpr double LONG_WALL_MIN = 0.60;
    constexpr double LONG_RSI_MAX = 30;
    constexpr double SHORT_DEV_THRESH = 3.0;
    constexpr double SHORT_OSC_MIN = 0.75;
    constexpr double SHORT_WALL_MAX = 0.5;
    constexpr double SHORT_RSI_MIN = 80;
    constexpr double ULTRA_EXTREME_SIGMA = 5.0;

    bool decay_long = check_momentum_decay("LONG");
    bool decay_short = check_momentum_decay("SHORT");
    bool ultra = std::abs(dev) > ULTRA_EXTREME_SIGMA;

    // 做多信号
    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN &&
        (decay_long || ultra) && ind_.rsi(14) < LONG_RSI_MAX) {
        sig.valid = true;
        sig.side = "LONG";
        sig.price = price;
        double raw_score = std::min(100.0, dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0);
        double adj = ml_.get_success_rate_adjustment("LONG");
        sig.score = std::clamp(raw_score * adj, 0.0, 100.0);
        if (sig.score < 60.0) sig.valid = false;
        return sig;
    }

    // 做空信号
    if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX &&
        (decay_short || ultra) && ind_.rsi(14) > SHORT_RSI_MIN) {
        sig.valid = true;
        sig.side = "SHORT";
        sig.price = price;
        double raw_score = std::min(100.0, (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0);
        double adj = ml_.get_success_rate_adjustment("SHORT");
        sig.score = std::clamp(raw_score * adj, 0.0, 100.0);
        if (sig.score < 60.0) sig.valid = false;
        return sig;
    }

    return sig;
}