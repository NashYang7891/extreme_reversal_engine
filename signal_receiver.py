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
MAX_ACTIVE_ORDERS = 3                # 最多同时挂3个限价单

TRAILING_ACTIVATION_PCT = 3.0
TRAILING_CALLBACK_PCT = 2.0
TRAILING_CALLBACK_PCT_SHORT = 2.0

positions = {}

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
    if side == "long": side = "buy"
    elif side == "short": side = "sell"
    if side not in ('buy', 'sell'):
        print(f"❌ 无效方向: {side}")
        return None, None
    try:
        balance = exchange.fetch_balance()
        free = balance.get('USDT', {}).get('free', 0)
        if free < ORDER_USDT:
            print(f"❌ 余额不足 ({free:.2f}U)，无法下单")
            return None, f"余额不足({free:.2f}U)"

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
        return None, str(e)[:100]

def cancel_order(order_id, symbol):
    try:
        exchange.cancel_order(order_id, symbol)
        print(f"🗑 已撤单 {symbol} {order_id}")
    except Exception as e:
        print(f"⚠ 撤单失败 {symbol} {order_id}: {e}")

def update_positions_after_fill(symbol, side, entry_price, order):
    try:
        qty = float(order['info'].get('executedQty', 0))
        if qty == 0: qty = float(order.get('filled', 0))
    except:
        qty = 0
    pos_side = "LONG" if side == "buy" else "SHORT"
    positions[symbol] = {
        'side': pos_side,
        'entry_price': entry_price,
        'qty': qty,
        'highest_price': entry_price,
        'lowest_price': entry_price,
        'trailing_activated': False
    }
    print(f"📊 持仓记录: {symbol} {pos_side} @ {entry_price:.6f} 数量:{qty}")

def check_and_trail_positions():
    if not positions: return
    for sym in list(positions.keys()):
        pos = positions[sym]
        try:
            ticker = exchange.fetch_ticker(sym)
            current_price = ticker['last']
            if not current_price: continue
        except: continue
        side = pos['side']
        entry = pos['entry_price']
        qty = pos['qty']
        if side == 'LONG':
            if current_price > pos['highest_price']: pos['highest_price'] = current_price
            pnl_pct = (current_price - entry) / entry * 100
            if pnl_pct >= TRAILING_ACTIVATION_PCT: pos['trailing_activated'] = True
            if pos['trailing_activated']:
                stop_price = pos['highest_price'] * (1 - TRAILING_CALLBACK_PCT/100)
                if current_price <= stop_price:
                    send_tg(f"🛑 移动止损平仓 {sym} LONG @ {current_price:.6f}")
                    exchange.create_order(symbol=sym, type='market', side='sell',
                                          amount=exchange.amount_to_precision(sym, qty),
                                          params={'reduceOnly': True})
                    del positions[sym]
                    continue
        else:
            if current_price < pos['lowest_price']: pos['lowest_price'] = current_price
            pnl_pct = (entry - current_price) / entry * 100
            if pnl_pct >= TRAILING_ACTIVATION_PCT: pos['trailing_activated'] = True
            if pos['trailing_activated']:
                stop_price = pos['lowest_price'] * (1 + TRAILING_CALLBACK_PCT_SHORT/100)
                if current_price >= stop_price:
                    send_tg(f"🛑 移动止损平仓 {sym} SHORT @ {current_price:.6f}")
                    exchange.create_order(symbol=sym, type='market', side='buy',
                                          amount=exchange.amount_to_precision(sym, qty),
                                          params={'reduceOnly': True})
                    del positions[sym]

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    engine_path = os.path.join(script_dir, "build", "engine")
    if not os.path.exists(engine_path): sys.exit("engine 未编译")
    try:
        exchange.load_markets()
        print("✅ 市场数据已加载")
    except: pass

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True, bufsize=1)
    send_tg("🤖 引擎启动 (熔断保护/1s时效)")
    last_b_signal = {}
    last_a_push = {}
    active_a_orders = {}
    A_ORDER_TIMEOUT_SEC = 15 * 60
    last_trail_check = time.time()

    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try: msg = json.loads(line)
        except json.JSONDecodeError: continue

        now = time.time()
        if now - last_trail_check > 30:
            check_and_trail_positions()
            last_trail_check = now

        t = msg.get("type", "")
        sym = msg.get("symbol", "")

        current_market_price = None
        try:
            ticker = exchange.fetch_ticker(sym)
            current_market_price = ticker['last'] if ticker else None
        except:
            pass

        if t == "HEARTBEAT":
            syms = msg.get("symbols", 0)
            send_tg(f"💓 心跳 | 合约: {syms}")
            continue

        # 检查当前挂单数量是否已达上限
        active_count = len(active_a_orders)
        if active_count >= MAX_ACTIVE_ORDERS:
            print(f"🛑 挂单已满 ({active_count}/{MAX_ACTIVE_ORDERS})，跳过新信号")
            # 仍然可以推送A层异动，但不下单
            if t == "A_ACTIVE":
                derived_price = msg.get("price",0); change = msg.get("change_pct",0)
                vol_r = msg.get("vol_ratio",0); dev = msg.get("dev", None)
                d_str = f" | 偏离度:{dev:.1f}" if dev else ""
                send_tg(f"🔥 {sym} 异动 | 价:{derived_price:.4f} | 涨跌:{change:+.2f}% | 量比:{vol_r:.1f}x{d_str}")
            continue

        if t == "A_ACTIVE":
            now = time.time()
            if sym in last_a_push and now - last_a_push[sym] < 300: continue
            derived_price = msg.get("price", 0)
            change = msg.get("change_pct", 0)
            vol_r = msg.get("vol_ratio", 0)
            dev = msg.get("dev", None)

            d_str = f" | 偏离度:{dev:.1f}" if dev else ""
            send_tg(f"🔥 {sym} 异动 | 价:{derived_price:.4f} | 涨跌:{change:+.2f}% | 量比:{vol_r:.1f}x{d_str}")
            last_a_push[sym] = now

            if dev is not None and abs(dev) > 1.3:
                side = "buy" if dev > 0 else "sell"
                order_key = f"{sym}_{side}"
                if order_key in active_a_orders: continue
                actual_price, order = place_order(sym, side, derived_price)
                if actual_price and order:
                    active_a_orders[order_key] = {
                        'symbol': sym, 'side': side, 'order': order,
                        'time': now, 'entry_dev': dev
                    }
                    send_tg(f"⚡ A层埋单 {side.upper()} {sym} @ {actual_price:.6f}")
                    update_positions_after_fill(sym, side, actual_price, order)

        elif t == "SIGNAL":
            side = msg.get("side",""); derived_price = msg.get("price",0)
            score = msg.get("score",0)
            stop_loss = msg.get("stop_loss",0); take_profit = msg.get("take_profit",0)
            now = time.time()
            if sym in last_b_signal and now - last_b_signal[sym] < 600: continue
            if is_quiet_period(): continue

            if current_market_price and derived_price > 0:
                diff_pct = abs(current_market_price - derived_price) / derived_price
                if diff_pct > 0.01:
                    send_tg(f"⚠ {sym} 信号过期 (市场{current_market_price:.6f} 推导{derived_price:.6f})，放弃下单")
                    continue

            actual_price, order = place_order(sym, side, derived_price)
            tg_lines = [f"🎯 {side.upper()} {sym} 评分:{score:.1f}"]
            if current_market_price:
                tg_lines.append(f"💰 实时价: {current_market_price:.6f}")
            tg_lines.append(f"📍 推导入场: {derived_price:.6f}")
            if actual_price:
                tg_lines.append(f"✅ 实际下单: {actual_price:.6f}")
                order_key = f"{sym}_{side.lower()}"
                if order_key in active_a_orders:
                    cancel_order(active_a_orders[order_key]['order'].get('id',''), sym)
                    del active_a_orders[order_key]
                update_positions_after_fill(sym, side, actual_price, order)
                last_b_signal[sym] = now
            else:
                tg_lines.append(f"❌ 下单失败: {order[:80] if isinstance(order,str) else '未知'}")
            tg_lines.append(f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}")
            send_tg("\n".join(tg_lines))

        for key in list(active_a_orders.keys()):
            if time.time() - active_a_orders[key]['time'] > A_ORDER_TIMEOUT_SEC:
                cancel_order(active_a_orders[key]['order'].get('id',''), active_a_orders[key]['symbol'])
                del active_a_orders[key]

if __name__ == "__main__":
    main()