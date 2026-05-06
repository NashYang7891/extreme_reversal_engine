#!/usr/bin/env python3
"""
signal_receiver.py - 独立标的趋势防线（方案二，最终版）
用法：
  USE_MOCK_DATA=True  → 用模拟行情，测试拦截/死亡开关
  USE_MOCK_DATA=False → 连接真实交易所 (ccxt)
"""

import socket
import json
import time
import sys
import threading
from collections import defaultdict

# ========== 调试开关：True 用模拟极端行情测试，False 用真实行情 ==========
USE_MOCK_DATA = False

# ========== 行情获取 ==========
def get_mock_ohlcv(symbol, timeframe, limit=5):
    """根据币种模拟不同的行情走势"""
    if timeframe != '1h':
        return []
    # 设定不同币种的极端走势
    if 'BTC' in symbol:
        # 强上升：价格大幅高于均线
        closes = [78000, 80000, 82000, 84000, 87000]  # 当前 87000，5日均线约 82200，价差5.8%
    elif 'ETH' in symbol:
        # 温和震荡
        closes = [2400, 2420, 2410, 2430, 2425]
    elif 'ADA' in symbol:
        # 强下降趋势
        closes = [0.35, 0.34, 0.33, 0.32, 0.30]
    elif 'DOGE' in symbol:
        # 强下降
        closes = [0.18, 0.17, 0.16, 0.15, 0.14]
    else:
        closes = [100, 101, 102, 101, 100]
    closes = closes[-limit:]
    return [[0, 0, 0, 0, c, 0] for c in closes]

if USE_MOCK_DATA:
    print("⚙️  使用模拟行情（极端走势测试模式）")
    def fetch_ohlcv(symbol, timeframe, limit):
        return get_mock_ohlcv(symbol, timeframe, limit)
else:
    try:
        import ccxt
        exchange = ccxt.binance()
        print("已连接 Binance 交易所")
        def fetch_ohlcv(symbol, timeframe, limit):
            return exchange.fetch_ohlcv(symbol, timeframe, limit=limit)
    except Exception as e:
        print(f"无法连接交易所 ({e})，回退到模拟数据")
        fetch_ohlcv = get_mock_ohlcv

# ========== 每个标的的独立趋势 ==========
trend_status = {}
last_htf_check = {}

def update_htf_trend_for_symbol(symbol):
    now = time.time()
    if symbol in last_htf_check and (now - last_htf_check[symbol]) < 900:
        return
    last_htf_check[symbol] = now
    try:
        ohlcv = fetch_ohlcv(symbol, '1h', 5)
        if not ohlcv or len(ohlcv) < 3:
            return
        closes = [c[4] for c in ohlcv]
        ema = sum(closes) / len(closes)
        cur_price = closes[-1]
        slope = closes[-1] - closes[-2]
        if cur_price > ema * 1.05 and slope > 0 and closes[-1] > closes[-3]:
            trend_status[symbol] = "STRONG_BULL"
        elif cur_price < ema * 0.95 and slope < 0 and closes[-1] < closes[-3]:
            trend_status[symbol] = "STRONG_BEAR"
        else:
            trend_status[symbol] = "NORMAL"
        print(f"[{time.strftime('%H:%M:%S')}] {symbol} 1H趋势 → {trend_status[symbol]} （价格={cur_price}, 均线={ema:.2f}）")
    except Exception as e:
        print(f"更新 {symbol} 趋势失败: {e}")

# ========== 死亡开关 ==========
stop_loss_counter = defaultdict(int)
blacklist = {}

def record_stop_loss(symbol):
    stop_loss_counter[symbol] += 1
    print(f"⚠️  {symbol} 止损计数: {stop_loss_counter[symbol]}")
    if stop_loss_counter[symbol] >= 2:
        blacklist[symbol] = time.time() + 4 * 3600
        print(f"🚫 {symbol} 触发死亡开关，拉黑至 {time.ctime(blacklist[symbol])}")

def is_blacklisted(symbol):
    if symbol in blacklist:
        if time.time() < blacklist[symbol]:
            return True
        else:
            del blacklist[symbol]
            stop_loss_counter[symbol] = 0
    return False

# ========== 动态阈值 ==========
def get_sigma_thresholds(symbol):
    try:
        ohlcv = fetch_ohlcv(symbol, '1h', 2)
        if len(ohlcv) >= 2:
            change = (ohlcv[-1][4] / ohlcv[-2][4] - 1) * 100
        else:
            change = 0
    except:
        change = 0
    if change >= 3.0:
        return 2.2, 5.5
    elif change <= -3.0:
        return 5.5, 2.2
    else:
        return 3.0, 3.0

# ========== 引擎客户端 ==========
class RealTradingClient:
    def __init__(self, host='127.0.0.1', port=5555):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect((host, port))
        self.buffer = b""
        print(f"已连接 C++ 引擎 {host}:{port}")

    def send_signal(self, signal_dict):
        msg = json.dumps(signal_dict) + "\n"
        self.sock.sendall(msg.encode())
        while b"\n" not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("引擎断开")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode())

    def send_order_with_stop(self, symbol, side, quantity,
                             order_type, trigger_price, limit_price):
        signal = {
            "action": "order_with_stop",
            "symbol": symbol, "side": side, "quantity": quantity,
            "order_type": order_type,
            "stop_loss": {
                "trigger_price": trigger_price,
                "limit_price": limit_price,
                "quantity": quantity
            }
        }
        return self.send_signal(signal)

    def close(self):
        self.sock.close()

class MockTradingClient:
    def send_signal(self, signal_dict):
        print(f"[模拟引擎] 收到信号: {json.dumps(signal_dict, indent=2)}")
        return {
            "status": "ok",
            "main_order": {"order_id": 99999, "status": "filled"},
            "stop_order": {"order_id": 88888, "status": "pending_stop", "parent_order_id": 99999}
        }

    def send_order_with_stop(self, symbol, side, quantity,
                             order_type, trigger_price, limit_price):
        return self.send_signal({
            "action": "order_with_stop",
            "symbol": symbol, "side": side, "quantity": quantity,
            "order_type": order_type,
            "stop_loss": {
                "trigger_price": trigger_price,
                "limit_price": limit_price,
                "quantity": quantity
            }
        })

    def close(self):
        pass

# ========== 信号处理 ==========
def signal_handler(symbol, side, current_price, z_score, client):
    update_htf_trend_for_symbol(symbol)
    current_trend = trend_status.get(symbol, "NORMAL")

    if is_blacklisted(symbol):
        print(f"🚷 {symbol} 在黑名单中，跳过信号")
        return

    if side == "SHORT" and current_trend == "STRONG_BULL":
        print(f"🚨 拦截 {symbol} 空单：该币1H处于强牛趋势")
        return
    if side == "LONG" and current_trend == "STRONG_BEAR":
        print(f"🚨 拦截 {symbol} 多单：该币1H处于强熊趋势")
        return

    long_z, short_z = get_sigma_thresholds(symbol)
    if side == "LONG" and abs(z_score) < long_z:
        print(f"⛔ {symbol} 做多z值 {z_score:.2f} < {long_z}，不满足阈值")
        return
    if side == "SHORT" and abs(z_score) < short_z:
        print(f"⛔ {symbol} 做空z值 {z_score:.2f} < {short_z}，不满足阈值")
        return

    stop_pct = 0.01
    if side == "LONG":
        trigger = current_price * (1 - stop_pct)
        limit = trigger * 0.999
        order_side = "buy"
    else:
        trigger = current_price * (1 + stop_pct)
        limit = trigger * 1.001
        order_side = "sell"

    try:
        resp = client.send_order_with_stop(
            symbol, order_side, 1, "market", trigger, limit
        )
        print(f"✅ 下单成功: {json.dumps(resp, indent=2)}")
    except Exception as e:
        print(f"❌ 下单失败: {e}")

# ========== 主程序 ==========
def main():
    try:
        client = RealTradingClient()
    except:
        print("无法连接 C++ 引擎，使用模拟客户端")
        client = MockTradingClient()

    # 预热几个常见标的
    def preheat():
        common = ["BTC/USDT", "ETH/USDT", "ADA/USDT", "DOGE/USDT"]
        for sym in common:
            update_htf_trend_for_symbol(sym)
        while True:
            time.sleep(60)
    threading.Thread(target=preheat, daemon=True).start()
    time.sleep(2)

    print("\n>>> 开始发送测试信号\n")

    # ---- 测试用例 ----
    # 1. BTC 强牛，做空应被拦截
    signal_handler("BTC/USDT", "SHORT", 87000, -4.2, client)

    # 2. ETH 震荡市，做多 z 不够
    signal_handler("ETH/USDT", "LONG", 2425, 2.8, client)

    # 3. ADA 强熊，做多应被拦截
    signal_handler("ADA/USDT", "LONG", 0.30, 3.5, client)

    # 4. DOGE 强熊，做空应允许 (强熊不做多，做空放行)
    signal_handler("DOGE/USDT", "SHORT", 0.14, -4.0, client)

    # 5. 死亡开关测试
    record_stop_loss("ADA/USDT")
    record_stop_loss("ADA/USDT")
    signal_handler("ADA/USDT", "SHORT", 0.29, -4.0, client)  # 被拉黑

    print("\n按 Ctrl+C 退出...")
    try:
        while True:
            time.sleep(10)
    except KeyboardInterrupt:
        print("退出")

if __name__ == "__main__":
    main()