#!/usr/bin/env python3
import json
import time
import os
import requests
import websocket

# 配置：只订阅24h成交额≥8000万 USDT 的合约（与C++端一致）
MIN_24H_VOLUME = 80000000
PIPE_PATH = "/tmp/trade_pipe"

def fetch_high_volume_symbols(min_volume=80000000):
    """获取成交额≥min_volume的USDT合约，按成交额降序返回"""
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
                symbols.append((sym, vol))
    # 按成交额降序排序
    symbols.sort(key=lambda x: x[1], reverse=True)
    return [sym for sym, _ in symbols]

def on_message(ws, message):
    try:
        data = json.loads(message)
        if 'data' in data:
            data = data['data']
        if 'p' in data and 's' in data:
            symbol = data['s']
            price = float(data['p'])
            # 非阻塞写入管道（如果管道满，忽略）
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
    symbols = fetch_high_volume_symbols(MIN_24H_VOLUME)
    print(f"获取到 {len(symbols)} 个高流动性合约 (24h成交额≥{MIN_24H_VOLUME}U)，开始订阅...")
    streams = [f"{sym.lower()}@aggTrade" for sym in symbols]
    if len(streams) > 200:
        print(f"警告：合约数 {len(streams)} 超过单连接200限制，请提高 MIN_24H_VOLUME")
        return
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
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)
    connect_and_run()