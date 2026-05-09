#!/usr/bin/env python3
import time
import json
import os
import sys
import threading
from collections import deque
import ccxt
from dotenv import load_dotenv
from pathlib import Path

basedir = Path(__file__).resolve().parent
load_dotenv(basedir / '.env')

exchange = ccxt.binance({
    'apiKey': os.getenv("BINANCE_API_KEY"),
    'secret': os.getenv("BINANCE_SECRET_KEY"),
    'enableRateLimit': True,
    'options': {'defaultType': 'future'},
})

# 监控的币种列表，应与 C++ 引擎保持一致（24h成交额≥8000万U）
def fetch_symbols():
    tickers = exchange.fetch_tickers()
    symbols = []
    for sym, t in tickers.items():
        if sym.endswith('USDT') and t.get('quoteVolume') and t['quoteVolume'] >= 80_000_000:
            symbols.append(sym)
    return symbols

# 存储每个币种的最近一分钟成交记录（滑动窗口）
trade_history = {}  # symbol -> deque of (timestamp_ms, amount, is_buyer_maker)
WINDOW_MS = 60_000

def update_trade_history(symbol, trades):
    now_ms = int(time.time() * 1000)
    if symbol not in trade_history:
        trade_history[symbol] = deque()
    for t in trades:
        trade_time = t['timestamp']
        trade_history[symbol].append((
            trade_time,
            float(t['amount']),
            t['isBuyerMaker']  # True: 买方是maker（主动卖出），False: 主动买入
        ))
    # 清理过期
    while trade_history[symbol] and trade_history[symbol][0][0] < now_ms - WINDOW_MS:
        trade_history[symbol].popleft()

def compute_metrics(symbol):
    history = trade_history.get(symbol, [])
    if len(history) < 10:
        return None
    now_ms = int(time.time() * 1000)
    # 过去1分钟的数据
    active_buy_count = 0
    active_sell_count = 0
    large_buy_volume = 0   # 大单买入量（USDT）
    total_volume = 0
    price_direction = 0  # 简化的价格变化（后来计算）
    for (ts, amount, is_buyer_maker) in history:
        notional = amount * (exchange.fetch_ticker(symbol)['last'] if 'last' else 0)  # 需要实时价格，这里简化
        # 实际应传入当前价格，但为性能，使用最近一次价格
        # 为了避免频繁调用API，我们在主循环中统一获取当前价格
        pass
    # 由于需要当前价格，建议在主循环中获取一次价格，然后计算
    # 暂且返回空
    return None

# 更简洁的实现：在主循环中一次性获取所有币种的最新成交和价格
def main():
    symbols = fetch_symbols()
    print(f"监控 {len(symbols)} 个币种，开始计算成交指标", flush=True)
    pipe_path = "/tmp/trade_metrics_pipe"
    if not os.path.exists(pipe_path):
        os.mkfifo(pipe_path)
    pipe_fd = os.open(pipe_path, os.O_WRONLY | os.O_NONBLOCK)

    while True:
        try:
            # 获取所有币种的最新价格（用于计算名义价值）
            tickers = exchange.fetch_tickers([f"{s}/USDT" for s in symbols])  # 批量获取
            now_ms = int(time.time() * 1000)
            for sym in symbols:
                # 获取最近100笔成交
                trades = exchange.fetch_trades(sym, limit=100)
                if not trades:
                    continue
                # 更新滑动窗口
                if sym not in trade_history:
                    trade_history[sym] = deque()
                for t in trades:
                    trade_time = t['timestamp']
                    amount = float(t['amount'])
                    is_buyer_maker = t['isBuyerMaker']
                    trade_history[sym].append((trade_time, amount, is_buyer_maker))
                # 清理超过1分钟的
                cutoff = now_ms - 60_000
                while trade_history[sym] and trade_history[sym][0][0] < cutoff:
                    trade_history[sym].popleft()
                if len(trade_history[sym]) == 0:
                    continue
                # 计算主动买入笔数、主动卖出笔数、大单统计
                active_buy_count = 0
                active_sell_count = 0
                large_buy_notional = 0
                total_notional = 0
                current_price = tickers[sym]['last']
                for (ts, amount, is_buyer_maker) in trade_history[sym]:
                    notional = amount * current_price
                    total_notional += notional
                    if not is_buyer_maker:  # 主动买入
                        active_buy_count += 1
                        if notional > 10000:  # 大单阈值 10,000 USDT
                            large_buy_notional += notional
                    else:
                        active_sell_count += 1
                total_trades = active_buy_count + active_sell_count
                if total_trades == 0:
                    continue
                active_buy_ratio = active_buy_count / total_trades
                large_buy_ratio = large_buy_notional / total_notional if total_notional > 0 else 0
                # 简单判断主动买入是否激增：当前1分钟的主动买入笔数是否超过过去5分钟均值的3倍？
                # 我们需要存储历史均值，这里简化：直接比较过去1分钟主动买入笔数是否大于过去5分钟的均值（需额外存储）
                # 为简化，我们只发送 active_buy_count 和 large_buy_ratio
                metrics = {
                    "symbol": sym,
                    "active_buy_count": active_buy_count,
                    "active_sell_count": active_sell_count,
                    "large_buy_ratio": large_buy_ratio,
                    "active_buy_ratio": active_buy_ratio,
                    "timestamp": now_ms
                }
                try:
                    os.write(pipe_fd, (json.dumps(metrics) + "\n").encode())
                except OSError:
                    pass
            time.sleep(1)  # 每秒轮询一次
        except Exception as e:
            print(f"错误: {e}", flush=True)
            time.sleep(5)

if __name__ == "__main__":
    main()