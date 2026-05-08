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
MIN_24H_VOLUME = 80000000          # 与 C++ 端一致
PIPE_PATH = "/tmp/trade_pipe"

# ---------- 确保管道存在 ----------
def ensure_pipe():
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)

# ---------- 动态获取高流动性币种 ----------
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
                symbols.append((sym, vol))
    symbols.sort(key=lambda x: x[1], reverse=True)
    return [sym for sym, _ in symbols]

# ---------- 非阻塞管道写入 ----------
pipe_fd = None

def init_pipe_writer():
    global pipe_fd
    if pipe_fd is None:
        try:
            pipe_fd = os.open(PIPE_PATH, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as e:
            print(f"打开管道失败: {e}", file=sys.stderr)
            pipe_fd = None

def write_to_pipe(symbol, price):
    global pipe_fd
    if pipe_fd is None:
        init_pipe_writer()
        if pipe_fd is None:
            return
    payload = json.dumps({"symbol": symbol, "price": price}) + "\n"
    try:
        os.write(pipe_fd, payload.encode())
    except OSError as e:
        if e.errno == errno.EAGAIN:
            # 管道满，丢弃这条数据（可打印调试）
            pass
        else:
            # 其他错误，尝试重新打开管道
            pipe_fd = None
            init_pipe_writer()

# ---------- WebSocket 回调 ----------
def on_message(ws, message):
    print(f"RAW: {message[:200]}")   # 关键调试：打印原始消息
    try:
        data = json.loads(message)
        # 处理组合流格式：{"stream":"btcusdt@aggTrade","data":{...}}
        if "data" in data:
            trade = data["data"]
        else:
            trade = data
        # 检查是否有价格字段
        if "p" in trade and "s" in trade:
            symbol = trade["s"]
            price = float(trade["p"])
            write_to_pipe(symbol, price)
    except Exception as e:
        print(f"解析消息错误: {e}", file=sys.stderr)

def on_error(ws, error):
    print(f"WebSocket 错误: {error}", file=sys.stderr)

def on_close(ws, close_status_code, close_msg):
    print("WebSocket 关闭，5秒后重连...", file=sys.stderr)
    time.sleep(5)
    connect_and_run()

def on_open(ws):
    symbols = fetch_high_volume_symbols()
    if not symbols:
        print("未获取到任何币种，退出", file=sys.stderr)
        ws.close()
        return
    print(f"获取到 {len(symbols)} 个高流动性合约，开始订阅...")
    streams = [f"{s.lower()}@aggTrade" for s in symbols]
    if len(streams) > 200:
        print(f"警告：合约数 {len(streams)} 超过 200，仅订阅前 200", file=sys.stderr)
        streams = streams[:200]
    sub_msg = {"method": "SUBSCRIBE", "params": streams, "id": 1}
    ws.send(json.dumps(sub_msg))
    print(f"已发送订阅请求，共 {len(streams)} 个流")

def connect_and_run():
    ws_url = "wss://fstream.binance.com/stream"
    ws = websocket.WebSocketApp(ws_url,
                                on_open=on_open,
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    # 允许 Ctrl+C 退出
    ws.run_forever(ping_interval=30, ping_timeout=10)

if __name__ == "__main__":
    ensure_pipe()
    # 忽略 SIGPIPE 防止写入管道时崩溃
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
    connect_and_run()