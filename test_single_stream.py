cat > ~/extreme_reversal_engine/test_single_stream.py << 'EOF'
#!/usr/bin/env python3
import websocket
import json

def on_message(ws, message):
    print("!!! 收到原始数据 !!!")
    print(message[:500])
    try:
        data = json.loads(message)
        if 'p' in data and 's' in data:
            print(f"价格: {data['p']}, 币种: {data['s']}")
    except:
        pass

def on_error(ws, error):
    print(f"错误: {error}")

def on_close(ws, close_status_code, close_msg):
    print("连接关闭")

def on_open(ws):
    print("WebSocket 已打开，等待数据...")

if __name__ == "__main__":
    url = "wss://fstream.binance.com/ws/btcusdt@aggTrade"
    print(f"连接 {url}")
    ws = websocket.WebSocketApp(url,
                                on_open=on_open,
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    ws.run_forever(ping_interval=30, ping_timeout=10)
EOF