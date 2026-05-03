#!/usr/bin/env python3
import subprocess, json, time, requests
from datetime import datetime, timezone, timedelta

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

def place_order(symbol, side, price, amount="0.01"):
    print(f"[ORDER] {symbol} {side} @ {price:.6f} size {amount}")
    # TODO: 接入OKX API

def main():
    proc = subprocess.Popen(["./engine"], stdout=subprocess.PIPE, text=True)
    send_tg("🤖 多币种极端反转引擎已启动")
    last_signal = {}
    for line in proc.stdout:
        try:
            sig = json.loads(line.strip())
            sym = sig["symbol"]
            if sym in last_signal and time.time() - last_signal[sym] < 3600:
                continue
            if is_quiet_period():
                print("静默期")
                continue
            send_tg(f"⚡ {sig['side']} {sym} @ {sig['price']:.6f} 评分:{sig['score']:.1f}")
            place_order(sym, sig['side'].lower(), sig['price'])
            last_signal[sym] = time.time()
        except Exception as e:
            print("Error:", e)

if __name__ == "__main__":
    main()