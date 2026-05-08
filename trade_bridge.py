#!/usr/bin/env python3
import json
import time
import os
import sys
import errno
import signal
import requests
import websocket

# ---------- 配置 ----------
MIN_24H_VOLUME = 80000000          # 24h成交额阈值（USDT）
PIPE_PATH = "/tmp/trade_pipe"

# ---------- 管道初始化（非阻塞写入）----------
pipe_fd = None

def init_pipe():
    global pipe_fd
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)
    # 以非阻塞只写方式打开，防止因管道满而阻塞
    pipe_fd = os.open(PIPE_PATH, os.O_WRONLY | os.O_NONBLOCK)
    print(f"管道已打开 (非阻塞模式): {PIPE_PATH}", flush=True)

def write_to_pipe(symbol, price):
    global pipe_fd
    if pipe_fd is None:
        init_pipe()
    if pipe_fd is None:
        return
    payload = json.dumps({"symbol": symbol, "price": price}) + "\n"
    try:
        os.write(pipe_fd, payload.encode())
    except OSError as e:
        if e.errno == errno.EAGAIN:
            # 管道满，丢弃当前消息（不阻塞）
            pass
        else:
            # 其他严重错误，尝试重新打开管道
            pipe_fd = None
            init_pipe()

# ---------- 获取高流动性币种列表 ----------
def fetch_high_volume_symbols():
    url = "https://fapi.binance.com/fapi/v1/ticker/24hr"
    try:
        resp = requests.get(url, timeout=10)
        resp.raise_for_status()
    except Exception as e:
        print(f"REST API 失败: {e}", file=sys.stderr)
        return []
    data = resp.json()
    symbols = []
    for item in data:
        sym = item["symbol"]
        if sym.endswith("USDT") and not any(x in sym for x in ("_", "USDC")):
            vol = float(item["quoteVolume"])
            if vol >= MIN_24H_VOLUME:
                symbols.append(sym)
    # 按成交额降序排序（无关紧要，为了美观）
    symbols.sort(key=lambda s: float(next(i["quoteVolume"] for i in data if i["symbol"]==s)), reverse=True)
    return symbols

# ---------- WebSocket 回调 ----------
def on_message(ws, message):
    # 关键：打印所有原始消息，确认是否收到数据
    print(f"RAW: {message[:200]}", flush=True)
    try:
        msg = json.loads(message)
        # 组合流数据包装在 "data" 字段中
        if "data" in msg:
            trade = msg["data"]
        else:
            trade = msg
        # 币安 aggTrade 流包含 s（symbol）和 p（price）
        if "s" in trade and "p" in trade:
            symbol = trade["s"]
            price = float(trade["p"])
            write_to_pipe(symbol, price)
    except Exception as e:
        print(f"解析错误: {e}", flush=True)

def on_error(ws, error):
    print(f"WebSocket 错误: {error}", file=sys.stderr, flush=True)

def on_close(ws, close_status_code, close_msg):
    print("WebSocket 关闭，10秒后重连...", flush=True)
    time.sleep(10)
    connect_and_run()

def on_open(ws):
    symbols = fetch_high_volume_symbols()
    if not symbols:
        print("未获取到任何币种，退出", file=sys.stderr)
        ws.close()
        return
    print(f"获取到 {len(symbols)} 个高流动性合约，开始订阅...", flush=True)
    streams = [f"{s.lower()}@aggTrade" for s in symbols]
    if len(streams) > 200:
        print(f"警告：合约数 {len(streams)} 超过 200，仅订阅前 200", file=sys.stderr)
        streams = streams[:200]
    sub_msg = {
        "method": "SUBSCRIBE",
        "params": streams,
        "id": 1
    }
    ws.send(json.dumps(sub_msg))
    print(f"订阅请求已发送，共 {len(streams)} 个流", flush=True)

def connect_and_run():
    ws_url = "wss://fstream.binance.com/stream"
    ws = websocket.WebSocketApp(ws_url,
                                on_open=on_open,
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    # 运行，设置 ping_interval 保持连接
    ws.run_forever(ping_interval=30, ping_timeout=10)

if __name__ == "__main__":
    # 忽略 SIGPIPE，防止写入管道时崩溃
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
    init_pipe()
    connect_and_run()