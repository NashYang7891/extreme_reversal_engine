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
        // 连续两个加速度为正，且处于低位，或价格已开始回升
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

    // ---------- 非对称参数 ----------
    // 做多：极其严苛（只在深插针后绝望时刻出手）
    constexpr double LONG_DEV_THRESH  = 3.2;      // 原 2.4 → 3.2
    constexpr double LONG_OSC_MAX     = 0.15;     // 原 0.35 → 0.15
    constexpr double LONG_WALL_MIN    = 0.65;
    constexpr double LONG_RSI_MAX     = 30;

    // 做空：保持原参数，捕捉上涨衰竭
    constexpr double SHORT_DEV_THRESH = 1.9;
    constexpr double SHORT_OSC_MIN    = 0.65;
    constexpr double SHORT_WALL_MAX   = 0.4;

    bool decay_long = check_momentum_decay("LONG");
    bool decay_short = check_momentum_decay("SHORT");

    static int log_cnt = 0;
    if (++log_cnt % 100 == 0)
        spdlog::info("B层: dev={:.2f} osc={:.2f} wall={:.2f} rsi={:.1f} decay_long={} decay_short={}",
                     dev, osc, wall, ind_.rsi(14), decay_long, decay_short);

    // 做多信号（在极值处触发）
    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN && decay_long &&
        ind_.rsi(14) < LONG_RSI_MAX) {
        sig.valid = true; sig.side = "LONG";
        sig.price = solve_critical_price(ob, "LONG");
        sig.score = std::min(100.0, dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0);
    }
    // 做空信号（参数保持不变）
    else if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX && decay_short) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = std::min(100.0, (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0);
    }
    return sig;
}