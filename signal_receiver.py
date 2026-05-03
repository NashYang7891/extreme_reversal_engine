#!/usr/bin/env python3
import subprocess, json, time, requests, os, sys
from datetime import datetime, timezone, timedelta
from dotenv import load_dotenv
import ccxt.async_support as ccxt_async
import asyncio

# 加载 .env 文件中的 API 密钥
load_dotenv()

API_KEY = os.getenv("BINANCE_API_KEY")
API_SECRET = os.getenv("BINANCE_SECRET_KEY")
TG_TOKEN = "8722422674:AAGrKmRurQ2G__j-Vxbh5451v0e9_u97CQY"
TG_CHAT = "5372217316"

def send_tg(msg):
    try:
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

async def place_order(symbol, side, price, amount=0.01):
    """使用 ccxt 在币安下 post-only limit 单"""
    if not API_KEY or not API_SECRET:
        print("API密钥未设置，跳过下单")
        return
    exchange = ccxt_async.binance({
        'apiKey': API_KEY,
        'secret': API_SECRET,
        'enableRateLimit': True,
        'options': {'defaultType': 'future'},
    })
    try:
        await exchange.load_markets()
        # 币安需要 "BTC/USDT:USDT" 格式，symbol 从引擎输出的是 "BTCUSDT"，需转换
        ccxt_symbol = symbol.replace('-', '/').replace('USDT', '/USDT:USDT')  # 简单映射
        await exchange.create_order(
            symbol=ccxt_symbol,
            type='limit',
            side=side.lower(),
            amount=amount,
            price=price,
            params={'timeInForce': 'GTX', 'postOnly': True}
        )
        print(f"✅ 下单成功: {symbol} {side} @ {price}")
    except Exception as e:
        print(f"❌ 下单失败: {e}")
    finally:
        await exchange.close()

async def main_async():
    # 启动 C++ 引擎
    script_dir = os.path.dirname(os.path.abspath(__file__))
    engine_path = os.path.join(script_dir, "build", "engine")
    if not os.path.exists(engine_path):
        print("engine 未编译，请先执行 cmake && make")
        sys.exit(1)
    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True)
    send_tg("🤖 币安极端反转引擎已启动")
    last_signal = {}
    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try:
            sig = json.loads(line)
        except json.JSONDecodeError:
            print("非JSON输出:", line)
            continue
        sym = sig.get("symbol", "")
        now = time.time()
        if sym in last_signal and now - last_signal[sym] < 3600:
            continue
        if is_quiet_period():
            print("静默期")
            continue
        msg = f"⚡ {sig['side']} {sym} @ {sig['price']:.6f} 评分:{sig['score']:.1f}"
        send_tg(msg)
        asyncio.create_task(place_order(sym, sig['side'], sig['price']))
        last_signal[sym] = now

if __name__ == "__main__":
    asyncio.run(main_async())