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

// 新动能衰减：加速度过零 + 成交量背离
bool SignalDetector::check_momentum_decay(const std::string& side) {
    const auto& prices = ind_.prices();
    if (prices.size() < 4) return false;

    // 1. 计算最近一笔加速度 (二阶微分)
    double v0 = prices.back() - prices[prices.size()-2];
    double v1 = prices[prices.size()-2] - prices[prices.size()-3];
    double accel = v0 - v1;

    // 2. 量价背离：当前价格创新低/新高，但主动成交量变化
    //    这里用 OrderBook 中的 trade delta（买方-卖方）做近似
    //    由外部传入，可暂时跳过，只靠加速度
    if (side == "LONG") {
        // 下跌衰竭：加速度由负转正或零轴上方，且价格处于相对低位
        return accel > 0 && prices.back() <= *std::min_element(prices.end()-5, prices.end()-1);
    } else {
        // 上涨衰竭：加速度由正转负或零轴下方，且价格处于相对高位
        return accel < 0 && prices.back() >= *std::max_element(prices.end()-5, prices.end()-1);
    }
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

    // 平衡版参数
    constexpr double LONG_DEV_THRESH  = 2.0;
    constexpr double LONG_OSC_MAX     = 0.35;
    constexpr double LONG_WALL_MIN    = 0.65;
    constexpr double SHORT_DEV_THRESH = 2.0;
    constexpr double SHORT_OSC_MIN    = 0.65;
    constexpr double SHORT_WALL_MAX   = 0.35;

    bool decay_long = check_momentum_decay("LONG");
    bool decay_short = check_momentum_decay("SHORT");

    // 调试日志：每10次输出一次 dev 和 osc
    static int count = 0;
    if (++count % 10 == 0) {
        spdlog::debug("{}: dev={:.2f} osc={:.2f} wall={:.2f} decay_long={} decay_short={}",
                      "sym", dev, osc, wall, decay_long, decay_short);
    }

    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN && decay_long) {
        sig.valid = true; sig.side = "LONG";
        sig.price = solve_critical_price(ob, "LONG");
        sig.score = dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0;
    } else if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX && decay_short) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0;
    }
    return sig;
}