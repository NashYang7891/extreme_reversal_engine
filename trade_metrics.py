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

def fetch_symbols():
    """获取24h成交额≥8000万U的USDT合约，与C++端一致"""
    try:
        tickers = exchange.fetch_tickers()
        symbols = []
        for sym, t in tickers.items():
            if sym.endswith('USDT') and t.get('quoteVolume') and t['quoteVolume'] >= 80_000_000:
                symbols.append(sym)
        return symbols
    except Exception as e:
        print(f"获取币种列表失败: {e}", flush=True)
        return []

# 存储每个币种的最近成交记录（滑动窗口）
trade_history = {}

def update_trade_history(symbol, trades):
    now_ms = int(time.time() * 1000)
    if symbol not in trade_history:
        trade_history[symbol] = deque()
    for t in trades:
        trade_time = t['timestamp']
        amount = float(t['amount'])
        is_buyer_maker = t['isBuyerMaker']
        trade_history[symbol].append((trade_time, amount, is_buyer_maker))
    # 清理超过60秒的数据
    cutoff = now_ms - 60_000
    while trade_history[symbol] and trade_history[symbol][0][0] < cutoff:
        trade_history[symbol].popleft()

def main():
    symbols = fetch_symbols()
    if not symbols:
        print("未获取到任何币种，退出", flush=True)
        return
    print(f"监控 {len(symbols)} 个币种，开始计算成交指标", flush=True)

    pipe_path = "/tmp/trade_metrics_pipe"
    if not os.path.exists(pipe_path):
        os.mkfifo(pipe_path)
    # 使用普通 open（阻塞模式），C++引擎必须已经打开读端，否则会阻塞
    pipe_fd = open(pipe_path, 'w')

    while True:
        try:
            # 批量获取当前价格（用于计算名义价值）
            tickers = exchange.fetch_tickers([f"{s}/USDT" for s in symbols]) if symbols else {}
            now_ms = int(time.time() * 1000)

            for sym in symbols:
                try:
                    # 获取最近100笔成交
                    trades = exchange.fetch_trades(sym, limit=100)
                    if not trades:
                        continue
                    update_trade_history(sym, trades)

                    history = trade_history.get(sym, [])
                    if len(history) < 5:
                        continue

                    # 计算指标
                    active_buy_count = 0
                    active_sell_count = 0
                    large_buy_notional = 0
                    total_notional = 0
                    current_price = tickers.get(sym, {}).get('last', 0)
                    if current_price == 0:
                        continue

                    for (ts, amount, is_buyer_maker) in history:
                        notional = amount * current_price
                        total_notional += notional
                        if not is_buyer_maker:   # 主动买入
                            active_buy_count += 1
                            if notional > 10000:  # 大单阈值 1万 USDT
                                large_buy_notional += notional
                        else:
                            active_sell_count += 1

                    total_trades = active_buy_count + active_sell_count
                    if total_trades == 0:
                        continue

                    active_buy_ratio = active_buy_count / total_trades
                    large_buy_ratio = large_buy_notional / total_notional if total_notional > 0 else 0

                    metrics = {
                        "symbol": sym,
                        "active_buy_count": active_buy_count,
                        "active_sell_count": active_sell_count,
                        "large_buy_ratio": large_buy_ratio,
                        "active_buy_ratio": active_buy_ratio,
                        "timestamp": now_ms
                    }
                    # 写入管道
                    try:
                        pipe_fd.write(json.dumps(metrics) + "\n")
                        pipe_fd.flush()
                    except (BrokenPipeError, OSError):
                        # 管道读端可能关闭，尝试重新打开
                        pipe_fd.close()
                        pipe_fd = open(pipe_path, 'w')
                        pipe_fd.write(json.dumps(metrics) + "\n")
                        pipe_fd.flush()
                except Exception as e:
                    print(f"处理 {sym} 失败: {e}", flush=True)
            time.sleep(1)  # 每秒轮询一次
        except Exception as e:
            print(f"主循环异常: {e}", flush=True)
            time.sleep(5)

if __name__ == "__main__":
    main()