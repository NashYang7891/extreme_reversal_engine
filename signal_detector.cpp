#include "signal_detector.h"
#include <cmath>

SignalDetector::SignalDetector(MLOptimizer& ml, Indicators& ind) : ml_(ml), ind_(ind) {}

double SignalDetector::objective(const OrderBook& ob, double price, const std::string& side) {
    double dev = (ind_.ema20() - price) / (ind_.atr() + 1e-12);
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

bool SignalDetector::check_momentum_decay(const OrderBook& ob, const std::string& side) {
    const auto& prices = ind_.prices();
    if (prices.size() < 6) return false;
    std::vector<double> diffs;
    for (size_t i = prices.size()-5; i < prices.size(); ++i)
        diffs.push_back(prices[i] - prices[i-1]);
    double accel = diffs.back() - diffs[diffs.size()-2];
    if (side == "LONG")
        return prices.back() <= *std::min_element(prices.begin(), prices.end()-2) && accel > 0;
    else
        return prices.back() >= *std::max_element(prices.begin(), prices.end()-2) && accel < 0;
}

Signal SignalDetector::check(const OrderBook& ob) {
    Signal sig;
    double dev = (ind_.ema20() - ind_.price()) / (ind_.atr() + 1e-12);
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());
    double wall = ob.imbalance();
    bool decay_long = check_momentum_decay(ob, "LONG");
    bool decay_short = check_momentum_decay(ob, "SHORT");

    if (dev > 2.3 && osc < 0.28 && wall > 0.75 && decay_long) {
        sig.valid = true; sig.side = "LONG";
        sig.price = solve_critical_price(ob, "LONG");
        sig.score = dev * 30.0 + (1-osc)*30.0 + wall*40.0;
    } else if (dev < -2.3 && osc > 0.72 && wall < 0.25 && decay_short) {
        sig.valid = true; sig.side = "SHORT";
        sig.price = solve_critical_price(ob, "SHORT");
        sig.score = (-dev)*30.0 + osc*30.0 + (1-wall)*40.0;
    }
    return sig;
}