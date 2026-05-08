#!/usr/bin/env python3
import json
import time
import os
import sys
import errno
import signal
import threading
import requests
import websocket

MIN_24H_VOLUME = 80000000
PIPE_PATH = "/tmp/trade_pipe"

pipe_fd = None
pipe_lock = threading.Lock()

def init_pipe():
    global pipe_fd
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)
    pipe_fd = os.open(PIPE_PATH, os.O_WRONLY | os.O_NONBLOCK)
    print(f"管道已打开: {PIPE_PATH}", flush=True)

def write_to_pipe(symbol, price):
    global pipe_fd
    if pipe_fd is None:
        with pipe_lock:
            if pipe_fd is None:
                init_pipe()
    if pipe_fd is None:
        return
    payload = json.dumps({"symbol": symbol, "price": price}) + "\n"
    try:
        os.write(pipe_fd, payload.encode())
    except OSError as e:
        if e.errno != errno.EAGAIN:
            pass

def fetch_high_volume_symbols():
    url = "https://fapi.binance.com/fapi/v1/ticker/24hr"
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    data = resp.json()
    symbols = []
    for item in data:
        sym = item["symbol"]
        if sym.endswith("USDT") and not any(x in sym for x in ("_", "USDC")):
            vol = float(item["quoteVolume"])
            if vol >= MIN_24H_VOLUME:
                symbols.append(sym)
    return symbols

def on_message(ws, message):
    print("!!! 收到原始数据 !!!", flush=True)   # 关键调试
    try:
        msg = json.loads(message)
        if "p" in msg and "s" in msg:
            symbol = msg["s"]
            price = float(msg["p"])
            write_to_pipe(symbol, price)
    except Exception as e:
        print(f"解析错误: {e}", flush=True)

def on_error(ws, error):
    print(f"WebSocket 错误: {error}", file=sys.stderr, flush=True)

def on_close(ws, close_status_code, close_msg):
    print("WebSocket 关闭，5秒后重连...", flush=True)
    time.sleep(5)
    # 重连逻辑由外部处理

def run_single_stream(symbol):
    """为单个币种建立 WebSocket 连接"""
    url = f"wss://fstream.binance.com/ws/{symbol.lower()}@aggTrade"
    ws = websocket.WebSocketApp(url,
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    ws.run_forever(ping_interval=30, ping_timeout=10)

if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
    init_pipe()
    symbols = fetch_high_volume_symbols()
    print(f"获取到 {len(symbols)} 个合约，为每个币种启动独立 WebSocket 连接", flush=True)
    threads = []
    for sym in symbols:
        t = threading.Thread(target=run_single_stream, args=(sym,), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(0.05)  # 避免同时连接过多
    for t in threads:
        t.join()