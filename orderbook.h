#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <deque>
#include <nlohmann/json.hpp>
#include <cstdint>

using json = nlohmann::json;

class OrderBook {
public:
    OrderBook() = default;

    // 处理成交数据（trade）
    void update_trade(const json& data);

    // 获取最新成交价
    double last_price() const { return last_price_; }

    // 获取最近窗口内的成交量（USDT）
    double recent_volume(int window_ms) const;

    // 获取累计主动买卖量（USDT）
    double buy_volume() const { return cum_buy_; }
    double sell_volume() const { return cum_sell_; }

    // 清空旧数据
    void prune();

private:
    struct Trade {
        double price;
        double volume;      // 以 USDT 计的名义价值
        int64_t timestamp_ms;
    };
    std::deque<Trade> trades;
    double last_price_ = 0.0;
    double cum_buy_ = 0.0;
    double cum_sell_ = 0.0;
    static const size_t MAX_TRADE = 5000;
};

#endif