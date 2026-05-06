#!/usr/bin/env python3
"""
signal_receiver.py - 独立标的趋势防线（方案二）
特征：
- 每个币种独立计算 1H 均线/斜率，判断自身强趋势
- 做多/做空阈值基于该币种自身的 1H 涨幅动态调整
- 死亡开关：同一币种连续两次止损后拉黑 4 小时
- 开仓信号附加对应的条件止损单，发送给 C++ 引擎
- 若 C++ 引擎未启动，自动降级为模拟客户端（打印信号）
"""

import socket
import json
import time
import sys
import threading
from collections import defaultdict

# ========== 行情获取（模拟/真实） ==========

def get_mock_ohlcv(symbol, timeframe, limit=5):
    """模拟 1H K线，不同币种生成不同走势，用于测试趋势防线"""
    if timeframe != '1h':
        return []
    # 根据币种名生成不同的走势特征
    if 'BTC' in symbol:
        # 强上升趋势
        closes = [64000, 65000, 66000, 67200, 68200]
    elif 'ETH' in symbol:
        # 温和上升
        closes = [3000, 3020, 3040, 3030, 3050]
    elif 'ADA' in symbol:
        # 震荡下跌（容易触发做空信号）
        closes = [0.50, 0.49, 0.48, 0.47, 0.46]
    elif 'DOGE' in symbol:
        # 强下降趋势
        closes = [0.20, 0.19, 0.18, 0.17, 0.16]
    else:
        # 默认小幅震荡
        closes = [100, 101, 100, 99, 100]
    # 返回固定长度的列表
    closes = closes[-limit:]
    return [[0, 0, 0, 0, c, 0] for c in closes]

# 尝试接入真实交易所（ccxt），失败则用模拟数据
try:
    import ccxt
    exchange = ccxt.binance()
    print("已连接 Binance 交易所 (ccxt)")
    def fetch_ohlcv(symbol, timeframe, limit):
        return exchange.fetch_ohlcv(symbol, timeframe, limit=limit)
except Exception as e:
    print(f"无法连接交易所 ({e})，使用模拟行情数据测试")
    fetch_ohlcv = get_mock_ohlcv

# ========== 每个标的的独立趋势状态 ==========
trend_status = {}          # {symbol: "STRONG_BULL"/"STRONG_BEAR"/"NORMAL"}
last_htf_check = {}        # {symbol: timestamp}

def update_htf_trend_for_symbol(symbol):
    """更新指定币种的1H趋势，每15分钟请求一次"""
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
blacklist = {}          # {symbol: 解禁时间戳}

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

# ========== 基于该币种自身涨幅的动态阈值 ==========
def get_sigma_thresholds(symbol):
    """
    根据该币种最近1小时涨跌幅，调整做多/做空的z值阈值。
    涨幅≥3% → 做多 2.2σ, 做空 5.5σ
    跌幅≥3% → 做多 5.5σ, 做空 2.2σ
    否则使用 3.0σ 作为默认阈值。
    """
    try:
        ohlcv = fetch_ohlcv(symbol, '1h', 2)
        if len(ohlcv) >= 2:
            change = (ohlcv[-1][4] / ohlcv[-2][4] - 1) * 100
        else:
            change = 0
    except:
        change = 0

    if change >= 3.0:
        return 2.2, 5.5  # (做多阈值, 做空阈值)
    elif change <= -3.0:
        return 5.5, 2.2
    else:
        return 3.0, 3.0

# ========== 引擎通信（自动回退模拟） ==========
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
                raise ConnectionError("引擎断开连接")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode())

    def send_order_with_stop(self, symbol, side, quantity,
                             order_type, trigger_price, limit_price):
        signal = {
            "action": "order_with_stop",
            "symbol": symbol,
            "side": side,
            "quantity": quantity,
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
    """模拟客户端，只打印信号内容，方便测试策略逻辑"""
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
            "symbol": symbol,
            "side": side,
            "quantity": quantity,
            "order_type": order_type,
            "stop_loss": {
                "trigger_price": trigger_price,
                "limit_price": limit_price,
                "quantity": quantity
            }
        })

    def close(self):
        pass

# ========== 信号处理（核心逻辑） ==========
def signal_handler(symbol, side, current_price, z_score, client):
    # 1. 更新该币种的独立趋势
    update_htf_trend_for_symbol(symbol)
    current_trend = trend_status.get(symbol, "NORMAL")

    # 2. 死亡开关
    if is_blacklisted(symbol):
        print(f"🚷 {symbol} 在黑名单中，跳过信号")
        return

    # 3. 趋势拦截
    if side == "SHORT" and current_trend == "STRONG_BULL":
        print(f"🚨 拦截 {symbol} 空单：该币1H处于强牛趋势")
        return
    if side == "LONG" and current_trend == "STRONG_BEAR":
        print(f"🚨 拦截 {symbol} 多单：该币1H处于强熊趋势")
        return

    # 4. 动态阈值过滤
    long_z, short_z = get_sigma_thresholds(symbol)
    if side == "LONG" and abs(z_score) < long_z:
        print(f"⛔ {symbol} 做多z值 {z_score:.2f} < {long_z}，不满足阈值")
        return
    if side == "SHORT" and abs(z_score) < short_z:
        print(f"⛔ {symbol} 做空z值 {z_score:.2f} < {short_z}，不满足阈值")
        return

    # 5. 构造止损参数
    stop_pct = 0.01
    if side == "LONG":
        trigger = current_price * (1 - stop_pct)
        limit = trigger * 0.999
        order_side = "buy"
    else:
        trigger = current_price * (1 + stop_pct)
        limit = trigger * 1.001
        order_side = "sell"

    # 6. 发送给引擎（真实或模拟）
    try:
        resp = client.send_order_with_stop(
            symbol, order_side, 1, "market", trigger, limit
        )
        print(f"✅ 下单成功: {json.dumps(resp, indent=2)}")
    except Exception as e:
        print(f"❌ 下单失败: {e}")

# ========== 主程序 ==========
def main():
    # 尝试连接真实引擎，失败则用模拟客户端
    try:
        client = RealTradingClient()
    except Exception as e:
        print(f"无法连接 C++ 引擎 ({e})，将使用模拟客户端，仅展示策略决策。")
        client = MockTradingClient()

    # 后台定时更新趋势（保留全局定时器，但趋势现在由信号按需更新，为了预热也可以主动刷新几个主要币种）
    def trend_preheater():
        # 启动时预先获取一些常见标的的趋势，避免第一次信号时卡顿
        common = ["BTC/USDT", "ETH/USDT", "ADA/USDT", "DOGE/USDT"]
        for sym in common:
            try:
                update_htf_trend_for_symbol(sym)
            except:
                pass
        while True:
            time.sleep(60)
    threading.Thread(target=trend_preheater, daemon=True).start()

    time.sleep(2)  # 等待预热完成

    # ---------- 测试信号 ----------
    print("\n>>> 开始发送测试信号（独立标的趋势）\n")

    # BTC 处于模拟的强上升趋势，做空应被拦截
    signal_handler("BTC/USDT", "SHORT", 68200, -4.2, client)
    # ETH 温和上升，趋势为 NORMAL，做多需要 z 值 >= 3.0
    signal_handler("ETH/USDT", "LONG", 3050, 2.8, client)   # 不够阈值
    # ADA 处于下降趋势，做多会被强熊拦截
    signal_handler("ADA/USDT", "LONG", 0.46, 3.5, client)
    # DOGE 强下降，做空应被允许（趋势 STRONG_BEAR 不做多，但做空允许）
    signal_handler("DOGE/USDT", "SHORT", 0.16, -4.0, client)

    # 模拟连续止损触发生死开关
    record_stop_loss("ADA/USDT")
    record_stop_loss("ADA/USDT")
    signal_handler("ADA/USDT", "SHORT", 0.45, -4.0, client)  # 应被拉黑

    print("\n按 Ctrl+C 退出...")
    try:
        while True:
            time.sleep(10)
    except KeyboardInterrupt:
        print("退出")

if __name__ == "__main__":
    main()