#!/usr/bin/env python3
import subprocess, json, time, os, sys
from datetime import datetime, timezone, timedelta
from dotenv import load_dotenv
import ccxt

load_dotenv()
API_KEY = os.getenv("BINANCE_API_KEY")
SECRET_KEY = os.getenv("BINANCE_SECRET_KEY")

TG_TOKEN = "8722422674:AAGrKmRurQ2G__j-Vxbh5451v0e9_u97CQY"
TG_CHAT = "5372217316"

exchange = ccxt.binance({
    'apiKey': API_KEY,
    'secret': SECRET_KEY,
    'enableRateLimit': True,
    'options': {'defaultType': 'future'},
})

LEVERAGE = 3
ORDER_USDT = 10.0

def send_tg(msg):
    try:
        import requests
        requests.post(f"https://api.telegram.org/bot{TG_TOKEN}/sendMessage",
                      json={"chat_id": TG_CHAT, "text": msg, "parse_mode": "Markdown"}, timeout=8)
    except: pass

def is_quiet_period():
    now = datetime.now(timezone.utc)
    if now.hour == 0 and now.minute < 5: return True
    for h in [0, 8, 16]:
        start = now.replace(hour=h, minute=0, second=0) - timedelta(minutes=5)
        end = now.replace(hour=h, minute=0, second=0) + timedelta(minutes=5)
        if start <= now <= end: return True
    return False

def place_order(symbol, side, price):
    try:
        if not exchange.markets:
            exchange.load_markets()
        market = exchange.market(symbol)
        amount = ORDER_USDT / price
        amount = exchange.amount_to_precision(symbol, amount)
        price_str = exchange.price_to_precision(symbol, price)
        exchange.set_leverage(LEVERAGE, symbol)
        exchange.set_margin_mode('isolated', symbol)
        order = exchange.create_order(
            symbol=symbol,
            type='limit',
            side=side.lower(),
            amount=amount,
            price=price_str,
            params={'timeInForce': 'GTX', 'postOnly': True}
        )
        print(f"✅ 下单成功: {symbol} {side} @ {price_str} Qty:{amount}")
        return order
    except Exception as e:
        print(f"❌ 下单失败 {symbol}: {e}")
        return None

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    engine_path = os.path.join(script_dir, "build", "engine")
    if not os.path.exists(engine_path):
        print("engine 未编译，请先执行 cmake && make")
        sys.exit(1)

    try:
        exchange.load_markets()
        print("✅ 市场数据已加载")
    except Exception as e:
        print(f"⚠️ 加载市场数据失败: {e}")

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True)
    send_tg("🤖 诊断模式启动 (查看异动恢复)")
    last_b_signal = {}

    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            # 打印所有非 JSON 输出（包含心跳和调试信息）
            print("C++:", line)
            continue

        msg_type = msg.get("type", "")
        sym = msg.get("symbol", "")

        if msg_type == "A_ACTIVE":
            # 诊断期间不加去重，允许频繁推送
            price = msg.get("price", 0)
            change = msg.get("change_pct", 0)
            tg_text = f"🔥 {sym} 异动 | 价:{price:.4f} | 3m涨跌:{change:+.2f}%"
            send_tg(tg_text)
            print(f"A层推送: {sym} change={change:.2f}%")

        elif msg_type == "SIGNAL":
            side = msg.get("side", "")
            price = msg.get("price", 0)
            score = msg.get("score", 0)
            now = time.time()
            if sym in last_b_signal and now - last_b_signal[sym] < 3600:
                print(f"⏰ {sym} B信号冷却中")
                continue
            if is_quiet_period():
                print("🔇 静默期，跳过B信号")
                continue
            tg_text = f"🎯 {side.upper()} {sym} @ {price:.6f} 评分:{score:.1f}"
            send_tg(tg_text)
            place_order(sym, side, price)
            last_b_signal[sym] = now

if __name__ == "__main__":
    main()