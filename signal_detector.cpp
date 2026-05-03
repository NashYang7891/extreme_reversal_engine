#include "signal_detector.h"
#include <spdlog/spdlog.h>
#include <cmath>

SignalDetector::SignalDetector(MLOptimizer& ml, Indicators& ind) : ml_(ml), ind_(ind) {}

double SignalDetector::objective(const OrderBook& ob, double price, const std::string& side) {
    double atr = ind_.atr();
    if (atr <= 0) return 0.0;
    double dev = (ind_.ema20() - price) / atr;
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());
    double wall = ob.imbalance();
    if (side == "LONG")
        return dev * 0.4 + (1.0 - osc) * 0.4 + wall * 0.2;
    else
        return (-dev) * 0.4 + osc * 0.4 + (1.0 - wall) * 0.2;
}

double SignalDetector::solve_critical_price(const OrderBook& ob, const std::string& side) {
    double lo = std::min(ind_.price() * 0.98, ob.best_bid());
    double hi = std::max(ind_.price() * 1.02, ob.best_ask());
    const double phi = 0.618;
    for (int i = 0; i < 30; ++i) {
        double c = hi - (hi - lo) * phi;
        double d = lo + (hi - lo) * phi;
        if (objective(ob, c, side) > objective(ob, d, side))
            hi = d;
        else
            lo = c;
    }
    return (lo + hi) / 2.0;
}

bool SignalDetector::check_momentum_slowing(const std::string& side) {
    // 15分钟斜率衰减：当前15分钟涨跌幅 < 前15分钟涨幅的一半
    double cur_change = ind_.price_change_pct(15 * 60);           // 最近15分钟
    double prev_change = ind_.price_change_pct(15 * 60, 15 * 60); // 前一个15分钟
    if (std::abs(prev_change) < 1e-6) return false;
    if (side == "LONG")
        return (cur_change - prev_change) < -0.5 * std::abs(prev_change); // 上涨速度锐减
    else
        return (cur_change - prev_change) > 0.5 * std::abs(prev_change);  // 下跌速度锐减
}

Signal SignalDetector::check(const OrderBook& ob) {
    Signal sig;
    if (ind_.prices().size() < 60) return sig;
    double atr = ind_.atr();
    if (atr <= 0) return sig;
    double ema20 = ind_.ema20();
    double price = ind_.price();
    if (price <= 0) return sig;

    double dev = (ema20 - price) / atr;
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());
    double wall = ob.imbalance();

    constexpr double LONG_DEV_THRESH  = 2.3;
    constexpr double LONG_OSC_MAX     = 0.30;
    constexpr double LONG_WALL_MIN    = 0.65;
    constexpr double SHORT_DEV_THRESH = 2.3;
    constexpr double SHORT_OSC_MIN    = 0.70;
    constexpr double SHORT_WALL_MAX   = 0.35;

    bool slow_long = check_momentum_slowing("LONG");
    bool slow_short = check_momentum_slowing("SHORT");

    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN && slow_long) {
        sig.valid = true; sig.side = "LONG";
        sig.price = solve_critical_price(ob, "LONG");
        sig.score = dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0;
    } else if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX && slow_short) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0;
    }
    return sig;
}