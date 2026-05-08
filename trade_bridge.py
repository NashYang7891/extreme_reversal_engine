#!/usr/bin/env python3
"""
动态获取币安所有 USDT 本位永续合约，订阅 @aggTrade 流，将实时成交价格写入管道。
可配置最低24h成交额（USDT），例如 min_volume=80000000 则只订阅流动性高的合约。
若 min_volume=0 则订阅全部合约（注意：总合约数可能超过WebSocket单连接200个流的限制）。
"""
import json
import time
import os
import requests
import websocket
import threading

# 配置
MIN_24H_VOLUME = 0          # 最小24h成交额（USDT），0表示不筛选
PIPE_PATH = "/tmp/trade_pipe"

def fetch_all_usdt_symbols(min_volume=0):
    """获取币安所有 USDT 本位永续合约，可选按24h成交额过滤"""
    url = "https://fapi.binance.com/fapi/v1/ticker/24hr"
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    data = resp.json()
    symbols = []
    for item in data:
        sym = item["symbol"]
        if sym.endswith("USDT") and not any(x in sym for x in ["_", "USDC"]):
            vol = float(item["quoteVolume"])
            if vol >= min_volume:
                symbols.append(sym)
    return symbols

def on_message(ws, message):
    try:
        data = json.loads(message)
        if 'data' in data:
            data = data['data']
        if 'p' in data and 's' in data:
            symbol = data['s']
            price = float(data['p'])
            # 写入管道（非阻塞，需处理管道满的情况）
            try:
                with open(PIPE_PATH, 'w') as f:
                    f.write(json.dumps({"symbol": symbol, "price": price}) + "\n")
            except BlockingIOError:
                pass
    except Exception as e:
        print("处理消息错误:", e)

def on_error(ws, error):
    print("WebSocket错误:", error)

def on_close(ws, close_status_code, close_msg):
    print("WebSocket关闭，5秒后重连...")
    time.sleep(5)
    connect_and_run()

def on_open(ws):
    # 获取需要订阅的币种列表
    symbols = fetch_all_usdt_symbols(MIN_24H_VOLUME)
    print(f"获取到 {len(symbols)} 个合约，开始订阅...")
    streams = [f"{sym.lower()}@aggTrade" for sym in symbols]
    # 分批次订阅（如果超过200个，币安会拒绝，需分批建立多个连接，这里简化起见，只订阅前200个）
    if len(streams) > 200:
        print(f"警告：合约数 {len(streams)} 超过单连接200限制，仅订阅前200个。建议提高 MIN_24H_VOLUME 过滤。")
        streams = streams[:200]
    sub_msg = {"method": "SUBSCRIBE", "params": streams, "id": 1}
    ws.send(json.dumps(sub_msg))
    print(f"已订阅 {len(streams)} 个成交流")

def connect_and_run():
    ws_url = "wss://fstream.binance.com/stream"
    ws = websocket.WebSocketApp(ws_url,
                                on_open=on_open,
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    ws.run_forever()

if __name__ == "__main__":
    # 确保管道存在
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)
    connect_and_run()