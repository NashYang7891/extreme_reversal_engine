#!/usr/bin/env python3
import subprocess, json, time, os, sys
from datetime import datetime, timezone, timedelta
from dotenv import load_dotenv
import ccxt

load_dotenv()
API_KEY = os.getenv("BINANCE_API_KEY")
SECRET_KEY = os.getenv("BINANCE_SECRET_KEY")
TG_TOKEN = os.getenv("TG_BOT_TOKEN")
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
    if not TG_TOKEN: return
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
    side = side.lower()
    if side not in ('buy', 'sell'):
        print(f"❌ 无效方向: {side}")
        return None, None
    try:
        if not exchange.markets: exchange.load_markets()
        ticker = exchange.fetch_ticker(symbol)
        bid = ticker['bid'] if ticker['bid'] else price
        ask = ticker['ask'] if ticker['ask'] else price
        if side == 'buy':
            order_price = min(price, ask * 1.0005)
        else:
            order_price = max(price, bid * 0.9995)
        order_price = exchange.price_to_precision(symbol, order_price)
        amount = ORDER_USDT / float(order_price)
        amount = exchange.amount_to_precision(symbol, amount)
        exchange.set_leverage(LEVERAGE, symbol)
        exchange.set_margin_mode('isolated', symbol)
        order = exchange.create_order(
            symbol=symbol, type='limit', side=side, amount=amount, price=order_price,
            params={'timeInForce': 'GTX', 'postOnly': True}
        )
        print(f"✅ 下单成功: {symbol} {side} @ {order_price} (推导:{price}) Qty:{amount}")
        return order_price, order
    except Exception as e:
        print(f"❌ 下单失败 {symbol}: {e}")
        return None, None

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    engine_path = os.path.join(script_dir, "build", "engine")
    if not os.path.exists(engine_path): sys.exit("engine 未编译")
    try:
        exchange.load_markets()
        print("✅ 市场数据已加载")
    except: pass

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True)
    send_tg("🤖 实时极端反转引擎启动 (埋单+止盈止损)")
    last_b_signal = {}      # B层 10 分钟冷却
    last_a_push = {}        # A层 5 分钟去重
    last_a_order = {}       # A层 30 分钟不重复下单

    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try: msg = json.loads(line)
        except: print("C++:", line); continue

        t = msg.get("type", "")
        sym = msg.get("symbol", "")

        if t == "A_ACTIVE":
            now = time.time()
            if sym in last_a_push and now - last_a_push[sym] < 300: continue
            price = msg.get("price",0); change = msg.get("change_pct",0)
            vol_r = msg.get("vol_ratio",0); dev = msg.get("dev", None)
            d_str = f" | 偏离度:{dev:.1f}" if dev else ""
            send_tg(f"🔥 {sym} 异动 | 价:{price:.4f} | 涨跌:{change:+.2f}% | 量比:{vol_r:.1f}x{d_str}")
            last_a_push[sym] = now

            # ---- A 层实时埋单（抢跑） ----
            # 当偏离度满足一定条件时，提前挂限价单，不等 B 层确认
            if dev is not None and abs(dev) > 1.3:
                # 根据偏离度方向确定做多/做空
                side = "buy" if dev > 0 else "sell"
                # 30 分钟内同一方向不重复下单
                order_key = f"{sym}_{side}"
                if order_key in last_a_order and now - last_a_order[order_key] < 1800:
                    continue
                # 下单（盘口修正自动处理）
                actual_price, _ = place_order(sym, side, price)
                if actual_price:
                    last_a_order[order_key] = now
                    send_tg(f"⚡ A层埋单 {side.upper()} {sym} @ {actual_price:.6f} (偏离度:{dev:.1f})")

        elif t == "SIGNAL":
            side = msg.get("side",""); price_derived = msg.get("price",0)
            score = msg.get("score",0)
            stop_loss = msg.get("stop_loss",0); take_profit = msg.get("take_profit",0)
            now = time.time()
            if sym in last_b_signal and now - last_b_signal[sym] < 600: continue
            if is_quiet_period(): continue

            # B 层下单
            actual_price, _ = place_order(sym, side, price_derived)

            tg_lines = [f"🎯 {side.upper()} {sym} 评分:{score:.1f}"]
            if actual_price:
                tg_lines.append(f"✅ 下单成功: {actual_price:.6f}")
            else:
                tg_lines.append(f"❌ 下单失败")
            tg_lines.append(f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}")
            send_tg("\n".join(tg_lines))

            if actual_price:
                last_b_signal[sym] = now

if __name__ == "__main__":
    main()