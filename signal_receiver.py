"""
signal_receiver.py - 策略信号发送端（含1H趋势防线、分段锁定、死亡开关）
功能：
- 每15分钟更新1小时级别趋势状态（强牛/强熊/正常）
- 信号发送前拦截逆势单
- 根据行情波动动态调整做多/做空门槛
- 连续止损后拉黑标的4小时
- 与C++引擎通信，自动附加对应止损单
"""

import socket
import json
import time
import threading
from collections import defaultdict, deque

# 模拟交易所接口，请替换为实际库 (ccxt 等)
class DummyExchange:
    def fetch_ohlcv(self, symbol, timeframe, limit):
        # 实际应从交易所获取数据，此处仅示例
        return []

exchange = DummyExchange()

# ---------- 趋势防线 ----------
trend_status = "NORMAL"      # STRONG_BULL, STRONG_BEAR, NORMAL
last_htf_check = 0

def update_htf_trend():
    """每15分钟更新一次1小时级别趋势"""
    global trend_status, last_htf_check
    if time.time() - last_htf_check < 900:
        return
    last_htf_check = time.time()
    try:
        ohlcv = exchange.fetch_ohlcv('BTC/USDT', '1h', 5)  # 领头羊
        if not ohlcv or len(ohlcv) < 3:
            return
        closes = [c[4] for c in ohlcv]
        ema = sum(closes) / len(closes)
        cur_price = closes[-1]
        # 简单斜率：最后两小时之差
        slope = closes[-1] - closes[-2]

        # 强趋势定义：价格高于均线5%且均线向上倾斜
        if cur_price > ema * 1.05 and slope > 0 and closes[-1] > closes[-3]:
            trend_status = "STRONG_BULL"
        elif cur_price < ema * 0.95 and slope < 0 and closes[-1] < closes[-3]:
            trend_status = "STRONG_BEAR"
        else:
            trend_status = "NORMAL"
        print(f"[{time.strftime('%H:%M:%S')}] 1H趋势更新 → {trend_status}")
    except Exception as e:
        print(f"更新HTF趋势失败: {e}")

# ---------- 死亡开关 ----------
# 记录每个币种的连续B层止损次数（均值回归失败），达到2次则拉黑4小时
stop_loss_counter = defaultdict(int)
blacklist = {}          # {symbol: 解禁时间戳}

def record_stop_loss(symbol):
    stop_loss_counter[symbol] += 1
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

# ---------- 分段参数 ----------
def get_sigma_thresholds(symbol):
    """
    根据领头羊涨幅调整门槛
    返回 (long_z, short_z) 做多和做空的z值阈值
    """
    # 获取1小时涨幅（这里简化为BTC的涨幅，实际可传入）
    try:
        ohlcv = exchange.fetch_ohlcv('BTC/USDT', '1h', 2)
        if len(ohlcv) >= 2:
            btc_change = (ohlcv[-1][4] / ohlcv[-2][4] - 1) * 100
        else:
            btc_change = 0
    except:
        btc_change = 0

    if btc_change >= 3.0:
        # 趋势保护模式：放宽做多、严格做空
        return 2.2, 5.5
    elif btc_change <= -3.0:
        return 5.5, 2.2
    else:
        return 3.0, 3.0   # 默认阈值

# ---------- 客户端通信 ----------
class TradingClient:
    def __init__(self, host='127.0.0.1', port=5555):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        self.buffer = b""

    def send_signal(self, signal_dict):
        msg = json.dumps(signal_dict) + "\n"
        self.sock.sendall(msg.encode())
        while b"\n" not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("Server closed connection")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode())

    def send_order_with_stop(self, symbol, side, quantity, 
                             order_type, trigger_price, limit_price):
        """开仓并自动挂止损单"""
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

    def cancel_order(self, order_id):
        return self.send_signal({"action": "cancel", "order_id": order_id})

    def close(self):
        self.sock.close()

# ---------- 主策略循环示例 ----------
def signal_handler(symbol, side, current_price, z_score, client):
    """处理外部产生的SIGNAL（例如来自C++模型或别的进程）"""
    update_htf_trend()

    # 死亡开关过滤
    if is_blacklisted(symbol):
        print(f"🚷 {symbol} 在黑名单中，跳过信号")
        return

    # 趋势拦截：强牛不做空，强熊不做多
    if side == "SHORT" and trend_status == "STRONG_BULL":
        print(f"🚨 拦截 {symbol} 空单：1小时强上升趋势")
        return
    if side == "LONG" and trend_status == "STRONG_BEAR":
        print(f"🚨 拦截 {symbol} 多单：1小时强下降趋势")
        return

    # 获取动态阈值
    long_z, short_z = get_sigma_thresholds(symbol)
    if side == "LONG" and abs(z_score) < long_z:
        print(f"⛔ {symbol} 做多z值 {z_score:.2f} < {long_z}，不满足阈值")
        return
    if side == "SHORT" and abs(z_score) < short_z:
        print(f"⛔ {symbol} 做空z值 {z_score:.2f} < {short_z}，不满足阈值")
        return

    # 计算止损价格（简化：基于当前价±一定百分比）
    stop_pct = 0.01  # 止损幅度1%
    if side == "LONG":
        trigger = current_price * (1 - stop_pct)
        limit = trigger * 0.999   # 稍微低一点确保成交
        order_side = "buy"
    else:
        trigger = current_price * (1 + stop_pct)
        limit = trigger * 1.001
        order_side = "sell"

    try:
        resp = client.send_order_with_stop(symbol, order_side, 1, "market", trigger, limit)
        print(f"✅ 下单成功: {resp}")
        # 此处模拟引擎返回订单ID，可存储用于后续止损监听
    except Exception as e:
        print(f"❌ 下单失败: {e}")

# 模拟运行（实际使用时，信号来自你的模型输出流）
if __name__ == "__main__":
    client = TradingClient()
    # 启动一个定时器线程，持续更新HTF趋势
    def trend_updater():
        while True:
            update_htf_trend()
            time.sleep(60)  # 每分钟检查一次，但内部15分钟才实际更新
    threading.Thread(target=trend_updater, daemon=True).start()

    # 模拟信号流（此处仅作测试）
    test_signals = [
        ("BTC/USDT", "SHORT", 65000, -3.2),
        ("ETH/USDT", "LONG", 3200, 2.8),
    ]
    for sym, side, price, z in test_signals:
        signal_handler(sym, side, price, z, client)
        time.sleep(1)

    # 模拟连续止损触发生死开关
    record_stop_loss("ADA/USDT")
    record_stop_loss("ADA/USDT")
    signal_handler("ADA/USDT", "SHORT", 0.45, -4.0, client)  # 会被拉黑

    client.close()