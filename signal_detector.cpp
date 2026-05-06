#include "signal_detector.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

SignalDetector::SignalDetector(MLOptimizer& ml, Indicators& ind) : ml_(ml), ind_(ind) {}

double SignalDetector::objective(const OrderBook& ob, double price, const std::string& side) {
    double atr = ind_.atr();
    if (atr <= 0) return 0.0;
    double dev = (ind_.ema20() - price) / atr;
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());
    double wall = ob.imbalance();
    if (side == "LONG") return dev * 0.4 + (1.0 - osc) * 0.4 + wall * 0.2;
    else return (-dev) * 0.4 + osc * 0.4 + (1.0 - wall) * 0.2;
}

double SignalDetector::solve_critical_price(const OrderBook& ob, const std::string& side) {
    double lo = std::min(ind_.price() * 0.98, ob.best_bid());
    double hi = std::max(ind_.price() * 1.02, ob.best_ask());
    const double phi = 0.618;
    for (int i = 0; i < 30; ++i) {
        double c = hi - (hi - lo) * phi;
        double d = lo + (hi - lo) * phi;
        if (objective(ob, c, side) > objective(ob, d, side)) hi = d; else lo = c;
    }
    return (lo + hi) / 2.0;
}

// 连续 N 个加速度均为正/负的判定
static bool consecutive_accel(const std::deque<double>& prices, int count, bool positive) {
    if (prices.size() < count + 3) return false;
    int cnt = 0;
    for (size_t i = prices.size() - count; i < prices.size(); ++i) {
        double v0 = prices[i] - prices[i-1];
        double v1 = prices[i-1] - prices[i-2];
        double a = v0 - v1;
        if (positive && a > 0) cnt++;
        else if (!positive && a < 0) cnt++;
        else return false;
    }
    return true;
}

bool SignalDetector::check_momentum_decay(const std::string& side) {
    const auto& prices = ind_.prices();
    if (prices.size() < 7) return false;

    double v0 = prices.back() - prices[prices.size()-2];
    double v1 = prices[prices.size()-2] - prices[prices.size()-3];
    double v2 = prices[prices.size()-3] - prices[prices.size()-4];
    double accel0 = v0 - v1;
    double accel1 = v1 - v2;

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
    double atr = ind_.atr(); if (atr <= 0) return sig;
    double ema20 = ind_.ema20(); double price = ind_.price();
    if (price <= 0) return sig;

    double dev = (ema20 - price) / atr;
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());
    double wall_raw = ob.imbalance();
    double wall = wall_raw;
    if (wall_raw <= 0.001 || wall_raw >= 0.999) wall = 0.5;

    // ---------- 重新平衡的参数 ----------
    // 做多：深度超卖才出手
    constexpr double LONG_DEV_THRESH  = 3.0;     // 偏离度 >= 3.0σ
    constexpr double LONG_OSC_MAX     = 0.18;    // 振荡器 <= 0.18
    constexpr double LONG_WALL_MIN    = 0.60;    // 买方深度 >= 0.60
    constexpr double LONG_RSI_MAX     = 30;      // RSI <= 30

    // 做空：适度收紧，减少噪音
    constexpr double SHORT_DEV_THRESH = 3.0;     // 偏离度 <= -3.0σ
    constexpr double SHORT_OSC_MIN    = 0.75;    // 振荡器 >= 0.75
    constexpr double SHORT_WALL_MAX   = 0.5;     // 卖方深度 <= 0.5 (即买方占比低)
    constexpr double SHORT_RSI_MIN    = 80;      // RSI >= 80

    bool decay_long = check_momentum_decay("LONG");
    bool decay_short = check_momentum_decay("SHORT");

    // 极端特殊待遇：偏离度 >5σ 仍可无视动能衰减
    constexpr double ULTRA_EXTREME_SIGMA = 5.0;
    bool is_ultra = (std::abs(dev) > ULTRA_EXTREME_SIGMA);

    // --- 做多信号 ---
    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN &&
        (decay_long || (is_ultra && dev > LONG_DEV_THRESH)) && ind_.rsi(14) < LONG_RSI_MAX) {
        sig.valid = true; sig.side = "LONG";
        sig.price = solve_critical_price(ob, "LONG");
        sig.score = std::min(100.0, dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0);
        return sig;
    }

    // --- 做空信号 ---
    if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX &&
        (decay_short || (is_ultra && dev < -SHORT_DEV_THRESH)) && ind_.rsi(14) > SHORT_RSI_MIN) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = std::min(100.0, (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0);
        return sig;
    }

    return sig;
}