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

    // ---------- 非对称参数（做空防守加强） ----------
    // 做多：极度严苛，等待深坑 + 止跌横盘
    constexpr double LONG_DEV_THRESH  = 3.5;
    constexpr double LONG_OSC_MAX     = 0.15;
    constexpr double LONG_WALL_MIN    = 0.65;
    constexpr double LONG_RSI_MAX     = 25;

    // 做空：大幅提高门槛，防止过早介入
    constexpr double SHORT_DEV_THRESH = 3.5;      // 从 2.5 提高到 3.5
    constexpr double SHORT_OSC_MIN    = 0.65;
    constexpr double SHORT_WALL_MAX   = 0.5;      // 从 0.7 收紧到 0.5
    constexpr double SHORT_RSI_MIN    = 80;       // 新增：RSI 必须 > 80
    constexpr double SHORT_ULTRA_DEV  = 5.0;      // 提高到 5.0σ 才无条件开空

    // ---------- 做空信号 (优先) ----------
    // 超强特权：偏离度超过 5.0σ 直接开空，无视其他条件
    if (dev < -SHORT_ULTRA_DEV) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = std::min(100.0, (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0);
        return sig;
    }

    // 普通做空：必须同时满足偏离度、振荡器、挂单壁、动能衰减、RSI
    bool decay_short = check_momentum_decay("SHORT");
    if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX &&
        decay_short && ind_.rsi(14) > SHORT_RSI_MIN) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = std::min(100.0, (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0);
        return sig;
    }

    // ---------- 做多信号 ----------
    bool accel_3up = consecutive_accel(ind_.prices(), 3, true);
    bool decay_long = check_momentum_decay("LONG");
    bool long_decay_ok = decay_long && accel_3up;

    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN &&
        long_decay_ok && ind_.rsi(14) < LONG_RSI_MAX) {
        sig.valid = true; sig.side = "LONG";
        sig.price = solve_critical_price(ob, "LONG");
        sig.score = std::min(100.0, dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0);
        return sig;
    }

    return sig;
}