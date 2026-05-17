#!/usr/bin/env python3
import json
import time
import os
import errno
import threading
import websocket
import requests
import stat

# 配置
MIN_24H_VOLUME = 80000000
PIPE_PATH = "/tmp/price_pipe"

pipe_fd = None
pipe_lock = threading.Lock()

def init_pipe():
    global pipe_fd
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)
    else:
        # 如果存在但不是管道，删除重建
        if not os.path.islink(PIPE_PATH) and not stat.S_ISFIFO(os.stat(PIPE_PATH).st_mode):
            os.remove(PIPE_PATH)
            os.mkfifo(PIPE_PATH)
    pipe_fd = os.open(PIPE_PATH, os.O_WRONLY | os.O_NONBLOCK)

def write_price(symbol, price):
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
        if e.errno == errno.EAGAIN:
            # 管道满，丢弃本次数据（不阻塞）
            pass
        else:
            # 其他错误，尝试重新打开管道
            with pipe_lock:
                if pipe_fd is not None:
                    os.close(pipe_fd)
                pipe_fd = None
                init_pipe()

def fetch_symbols():
    url = "https://fapi.binance.com/fapi/v1/ticker/24hr"
    try:
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
    except Exception as e:
        print(f"获取币种列表失败: {e}", flush=True)
        return []

def on_message(ws, message):
    try:
        msg = json.loads(message)
        if 'data' in msg:
            trade = msg['data']
        else:
            trade = msg
        if 'p' in trade and 's' in trade:
            symbol = trade['s']
            price = float(trade['p'])
            write_price(symbol, price)
    except Exception as e:
        print(f"处理消息错误: {e}", flush=True)

def on_error(ws, error):
    print(f"WebSocket 错误: {error}", flush=True)

def on_close(ws, close_status_code, close_msg):
    print("WebSocket 关闭，5秒后重连...", flush=True)
    time.sleep(5)
    connect_and_run()

def on_open(ws, symbols):
    streams = [f"{s.lower()}@aggTrade" for s in symbols]
    if len(streams) > 200:
        streams = streams[:200]
    sub_msg = {"method": "SUBSCRIBE", "params": streams, "id": 1}
    ws.send(json.dumps(sub_msg))
    print(f"已订阅 {len(streams)} 个成交流", flush=True)

def connect_and_run():
    symbols = fetch_symbols()
    if not symbols:
        print("未获取到币种，10秒后重试", flush=True)
        time.sleep(10)
        connect_and_run()
        return
    print(f"监控 {len(symbols)} 个币种，开始桥接成交数据", flush=True)
    ws_url = "wss://fstream.binance.com/stream"
    ws = websocket.WebSocketApp(ws_url,
                                on_open=lambda ws: on_open(ws, symbols),
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    ws.run_forever(ping_interval=30, ping_timeout=10)

if __name__ == "__main__":
    init_pipe()
    connect_and_run()