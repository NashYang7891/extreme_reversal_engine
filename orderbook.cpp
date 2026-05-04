#include "orderbook.h"
#include <numeric>
#include <spdlog/spdlog.h>

void OrderBook::update_depth(const json& data) {
    try {
        // 确定数据源（增量格式使用 "b"/"a"，快照格式使用 "bids"/"asks"）
        const json* bids_json = nullptr;
        const json* asks_json = nullptr;

        if (data.contains("b") && data.contains("a")) {
            // 增量格式
            bids_json = &data["b"];
            asks_json = &data["a"];
        } else if (data.contains("bids") && data.contains("asks")) {
            // 全量快照格式（depth5）
            bids.clear();
            asks.clear();
            bids_json = &data["bids"];
            asks_json = &data["asks"];
        } else {
            // 无法识别的格式，直接丢弃
            static int err_count = 0;
            if (++err_count % 100 == 1)
                spdlog::warn("未知的深度推送格式，已跳过");
            return;
        }

        // 更新买盘
        for (auto& item : *bids_json) {
            try {
                double price = std::stod(item[0].get<std::string>());
                double qty   = std::stod(item[1].get<std::string>());
                if (qty > 0)
                    bids[price] = qty;
                else
                    bids.erase(price);
            } catch (...) {
                // 单条数据异常，忽略
            }
        }

        // 更新卖盘
        for (auto& item : *asks_json) {
            try {
                double price = std::stod(item[0].get<std::string>());
                double qty   = std::stod(item[1].get<std::string>());
                if (qty > 0)
                    asks[price] = qty;
                else
                    asks.erase(price);
            } catch (...) {
                // 单条数据异常，忽略
            }
        }
    } catch (const std::exception& e) {
        static int err_cnt = 0;
        if (++err_cnt % 100 == 1)
            spdlog::warn("深度解析异常: {}，已丢弃本条数据", e.what());
    }
}

void OrderBook::add_agg_trade(bool is_buy, double volume, int64_t trade_time_ms) {
    if (trade_time_ms == 0) {
        trade_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    trades.push_back({volume, trade_time_ms});
    if (is_buy) cum_buy += volume;
    else cum_sell += volume;
    prune();
}

void OrderBook::prune() {
    while (trades.size() > MAX_TRADE) {
        // 此处为简化，未精确扣除 cum_buy/sell，但只影响 long-term 统计，不参与核心定价
        trades.pop_front();
    }
}

double OrderBook::micro_price() const {
    if (bids.empty() || asks.empty()) return 0.0;
    double best_bid_ = bids.rbegin()->first;
    double best_ask_ = asks.begin()->first;
    double bv = buy_volume();
    double sv = sell_volume();
    double total = bv + sv;
    if (total <= 1e-12)   // 防止除零
        return (best_bid_ + best_ask_) / 2.0;
    return (best_ask_ * bv + best_bid_ * sv) / total;
}

double OrderBook::imbalance() const {
    double bv = buy_volume();
    double sv = sell_volume();
    double total = bv + sv;
    if (total <= 1e-12) return 0.5;   // 中性值
    return bv / total;
}

double OrderBook::best_bid() const { return bids.empty() ? 0.0 : bids.rbegin()->first; }
double OrderBook::best_ask() const { return asks.empty() ? 0.0 : asks.begin()->first; }

double OrderBook::buy_volume() const {
    return std::accumulate(bids.begin(), bids.end(), 0.0,
        [](double sum, const auto& p) { return sum + p.second; });
}
double OrderBook::sell_volume() const {
    return std::accumulate(asks.begin(), asks.end(), 0.0,
        [](double sum, const auto& p) { return sum + p.second; });
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