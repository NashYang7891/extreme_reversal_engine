#include "orderbook.h"
#include <spdlog/spdlog.h>
#include <numeric>
#include <chrono>

void OrderBook::update_trade(const json& data) {
    try {
        // 币安 trade 流格式: {"e":"trade","E":时间戳,"s":"符号","p":"价格","q":"数量","T":成交时间,"m":买方是否是做市商, ...}
        int64_t trade_time = data["T"].get<int64_t>();
        double price = std::stod(data["p"].get<std::string>());
        double qty = std::stod(data["q"].get<std::string>());      // 币数量
        bool is_buyer_maker = data["m"].get<bool>();               // 如果为 true，表示买方是挂单方（即卖方主动吃单）

        // 计算名义价值 (USDT)
        double notional = price * qty;

        // 判断主动方向：如果买方是 maker，则主动方是 seller（即卖单吃单，所以是空头主动）
        // 更直观：is_buyer_maker == true 表示买单挂单，卖单成交 → 卖盘主动，计入 sell_volume
        //         is_buyer_maker == false 表示卖单挂单，买单成交 → 买盘主动，计入 buy_volume
        if (is_buyer_maker) {
            cum_sell_ += notional;
        } else {
            cum_buy_ += notional;
        }

        trades.push_back({price, notional, trade_time});
        last_price_ = price;
        prune();
    } catch (const std::exception& e) {
        static int err_cnt = 0;
        if (++err_cnt % 100 == 1)
            spdlog::warn("成交数据解析异常: {}，已丢弃", e.what());
    }
}

void OrderBook::prune() {
    while (trades.size() > MAX_TRADE) {
        auto& oldest = trades.front();
        if (oldest.timestamp_ms < trades.back().timestamp_ms - 3600000) { // 清理超过1小时的
            if (oldest.volume > 0) {
                // 这里不维护 cum_buy_/cum_sell_ 的精确减去，因为累计值用于快照，允许一定误差
            }
            trades.pop_front();
        } else {
            break;
        }
    }
}

double OrderBook::recent_volume(int window_ms) const {
    if (trades.empty()) return 0.0;
    int64_t now_ms = trades.back().timestamp_ms;
    double sum = 0.0;
    for (auto it = trades.rbegin(); it != trades.rend(); ++it) {
        if (now_ms - it->timestamp_ms <= window_ms)
            sum += it->volume;
        else
            break;
    }
    return sum;
}