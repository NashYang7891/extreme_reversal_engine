#include "signal_detector.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

SignalDetector::SignalDetector(MLOptimizer& ml, Indicators& ind) : ml_(ml), ind_(ind) {}

// ======================== 动量衰减检测（保持不变） ========================
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

// ======================== 主检测函数（完全基于成交数据） ========================
Signal SignalDetector::check(const OrderBook& ob) {
    Signal sig;
    if (ind_.prices().size() < 60) return sig;
    double atr = ind_.atr(); 
    if (atr <= 0) return sig;
    double ema20 = ind_.ema20(); 
    double price = ob.last_price();          // 关键修改：使用成交价，不再用 ind_.price()
    if (price <= 0) return sig;

    // 计算偏离度 (基于当前成交价)
    double dev = (ema20 - price) / atr;

    // 复合振荡器（保留原有逻辑，不依赖订单簿）
    double osc = ind_.composite_oscillator(ml_.get_w_rsi(), ml_.get_w_kdj(), ml_.get_w_cci());

    // 买卖压力指标：使用累计主动成交额比例 (避免深度操纵)
    double buy_vol = ob.buy_volume();
    double sell_vol = ob.sell_volume();
    double total_vol = buy_vol + sell_vol;
    double wall = (total_vol > 1e-9) ? (buy_vol / total_vol) : 0.5;
    // 防止极端值
    if (wall <= 0.001 || wall >= 0.999) wall = 0.5;

    // ---------- 可调整的参数 ----------
    // 做多条件
    constexpr double LONG_DEV_THRESH  = 3.0;     // dev >= 3.0σ
    constexpr double LONG_OSC_MAX     = 0.18;    // 振荡器 <= 0.18
    constexpr double LONG_WALL_MIN    = 0.60;    // 买方成交额占比 >= 60%
    constexpr double LONG_RSI_MAX     = 30;      // RSI <= 30

    // 做空条件
    constexpr double SHORT_DEV_THRESH = 3.0;     // dev <= -3.0σ
    constexpr double SHORT_OSC_MIN    = 0.75;    // 振荡器 >= 0.75
    constexpr double SHORT_WALL_MAX   = 0.5;     // 买方成交额占比 <= 50% (即卖方主导)
    constexpr double SHORT_RSI_MIN    = 80;      // RSI >= 80

    bool decay_long  = check_momentum_decay("LONG");
    bool decay_short = check_momentum_decay("SHORT");

    constexpr double ULTRA_EXTREME_SIGMA = 5.0;
    bool is_ultra = (std::abs(dev) > ULTRA_EXTREME_SIGMA);

    // --- 做多信号 ---
    if (dev > LONG_DEV_THRESH && osc < LONG_OSC_MAX && wall > LONG_WALL_MIN &&
        (decay_long || is_ultra) && ind_.rsi(14) < LONG_RSI_MAX) {
        sig.valid = true;
        sig.side = "LONG";
        sig.price = price;   // 直接使用当前成交价，不再求解复杂临界价
        // 评分可以基于偏离度、振荡器、压力指标综合
        double score = std::min(100.0, dev * 30.0 + (1.0 - osc) * 30.0 + wall * 40.0);
        sig.score = std::max(0.0, std::min(100.0, score));
        return sig;
    }

    // --- 做空信号 ---
    if (dev < -SHORT_DEV_THRESH && osc > SHORT_OSC_MIN && wall < SHORT_WALL_MAX &&
        (decay_short || is_ultra) && ind_.rsi(14) > SHORT_RSI_MIN) {
        sig.valid = true;
        sig.side = "SHORT";
        sig.price = price;
        double score = std::min(100.0, (-dev) * 30.0 + osc * 30.0 + (1.0 - wall) * 40.0);
        sig.score = std::max(0.0, std::min(100.0, score));
        return sig;
    }

    return sig;
}