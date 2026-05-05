#!/usr/bin/env python3
import subprocess, json, time, os, sys, threading
from datetime import datetime, timezone, timedelta
from pathlib import Path
from dotenv import load_dotenv
import ccxt

basedir = Path(__file__).resolve().parent
load_dotenv(basedir / '.env')

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

TRAILING_ACTIVATION_PCT = 3.0
TRAILING_CALLBACK_PCT = 1.2
TRAILING_CALLBACK_PCT_SHORT = 1.2

positions = {}           # symbol -> 持仓信息
pending_orders = {}      # order_id -> dict(symbol, side, price, time)  待成交订单

# TG 频率控制
LAST_TG_SEND = 0
TG_RATE_LIMIT_SEC = 0.5

def send_tg(msg):
    global LAST_TG_SEND
    now = time.time()
    if now - LAST_TG_SEND < TG_RATE_LIMIT_SEC:
        return
    LAST_TG_SEND = now
    try:
        import requests
        requests.post(f"https://api.telegram.org/bot{TG_TOKEN}/sendMessage",
                      json={"chat_id": TG_CHAT, "text": msg, "parse_mode": "Markdown"}, timeout=8)
    except Exception as e:
        print(f"TG推送失败: {e}")

def is_quiet_period():
    now = datetime.now(timezone.utc)
    if now.hour == 0 and now.minute < 5: return True
    for h in [0, 8, 16]:
        start = now.replace(hour=h, minute=0, second=0) - timedelta(minutes=5)
        end = now.replace(hour=h, minute=0, second=0) + timedelta(minutes=5)
        if start <= now <= end: return True
    return False

def place_order(symbol, side, price):
    """
    IOC 模式下单，直接使用 C++ 传入的 current_price 作为价格基准，
    做多时以 max(推导价, 卖一价) 下单，做空时以 min(推导价, 买一价) 下单，
    并且不获取 ticker，从而消除延迟。
    """
    side = side.lower()
    if side == "long": side = "buy"
    elif side == "short": side = "sell"
    if side not in ('buy', 'sell'):
        return None, None

    try:
        if not exchange.markets:
            exchange.load_markets()

        # 获取实时盘口价（必须，用于设置 IOC 保护）
        ticker = exchange.fetch_ticker(symbol)
        ask = ticker['ask'] if ticker['ask'] else price
        bid = ticker['bid'] if ticker['bid'] else price

        # 激进下单：做多卖出价，做空买入价，较大可能立刻成交
        if side == 'buy':
            order_price = max(price, ask)   # 不低于卖一，可能作为taker
        else:
            order_price = min(price, bid)   # 不高于买一

        order_price_str = exchange.price_to_precision(symbol, order_price)
        amount = ORDER_USDT / order_price
        amount_str = exchange.amount_to_precision(symbol, amount)

        exchange.set_leverage(LEVERAGE, symbol)
        exchange.set_margin_mode('isolated', symbol)

        order = exchange.create_order(
            symbol=symbol,
            type='limit',
            side=side,
            amount=amount_str,
            price=order_price_str,
            params={'timeInForce': 'IOC'}   # 立即成交或取消
        )

        if order and order.get('status') == 'rejected':
            msg = order.get('info', {}).get('msg', '未知原因')
            print(f"❌ 订单被拒绝: {msg}")
            return None, f"被拒: {msg}"

        # IOC 可能部分成交，此处用 average 作为实际成交均价
        actual_price = float(order.get('average') or order_price)
        filled = float(order.get('filled') or 0)
        if filled > 0:
            print(f"✅ 成交: {symbol} {side} @ {actual_price:.6f} (推导:{price:.6f}) 量:{filled}")
            return actual_price, order
        else:
            print(f"⚠ IOC 未成交: {symbol} {side} @ {order_price_str} (推导:{price:.6f})，已自动取消")
            return None, None
    except Exception as e:
        print(f"❌ 下单异常 {symbol}: {e}")
        return None, str(e)[:100]
    
def cancel_order(order_id, symbol):
    try:
        exchange.cancel_order(order_id, symbol)
        pending_orders.pop(order_id, None)   # 从待成交列表中移除
        print(f"🗑 已撤单 {symbol} {order_id}")
    except Exception as e:
        print(f"⚠ 撤单失败 {symbol} {order_id}: {e}")

def check_pending_orders():
    """检查超过1分钟未成交的订单，自动撤单"""
    now = time.time()
    to_cancel = []
    for oid, info in pending_orders.items():
        if now - info['time'] > 60:   # 1 分钟超时
            to_cancel.append((oid, info['symbol']))
    for oid, sym in to_cancel:
        print(f"⏰ 订单 {oid} 超时未成交，自动撤单")
        cancel_order(oid, sym)

def update_positions_after_fill(symbol, side, entry_price, order):
    try:
        qty_val = float(order.get('filled') or order.get('amount') or 0)
    except:
        qty_val = 0.0
    if qty_val <= 0:
        return   # 未实际成交，不记录持仓
    pos_side = "LONG" if side == "buy" else "SHORT"
    positions[symbol] = {
        'side': pos_side,
        'entry_price': entry_price,
        'qty': qty_val,
        'highest_price': entry_price,
        'lowest_price': entry_price,
        'trailing_activated': False
    }
    print(f"📊 持仓记录: {symbol} {pos_side} @ {entry_price:.6f} 数量:{qty_val}")

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
                    send_tg(f"🛑 跟踪止盈平仓 {sym} LONG @ {current_price:.6f}")
                    exchange.create_order(symbol=sym, type='market', side='sell',
                                          amount=exchange.amount_to_precision(sym, qty),
                                          params={'reduceOnly': True})
                    del positions[sym]
        else:
            if current_price < pos['lowest_price']: pos['lowest_price'] = current_price
            pnl_pct = (entry - current_price) / entry * 100
            if pnl_pct >= TRAILING_ACTIVATION_PCT: pos['trailing_activated'] = True
            if pos['trailing_activated']:
                stop_price = pos['lowest_price'] * (1 + TRAILING_CALLBACK_PCT_SHORT/100)
                if current_price >= stop_price:
                    send_tg(f"🛑 跟踪止盈平仓 {sym} SHORT @ {current_price:.6f}")
                    exchange.create_order(symbol=sym, type='market', side='buy',
                                          amount=exchange.amount_to_precision(sym, qty),
                                          params={'reduceOnly': True})
                    del positions[sym]

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    engine_path = os.path.join(script_dir, "build", "engine")
    if not os.path.exists(engine_path):
        print("engine 未编译")
        sys.exit(1)

    try:
        exchange.load_markets()
        print("✅ 市场数据已加载")
    except Exception as e:
        print(f"⚠ 加载市场失败: {e}")

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True, bufsize=1)
    send_tg("🤖 引擎已启动 (1分钟未成交撤单)")

    last_b_signal = {}
    last_a_push = {}
    main.last_trail_check = 0

    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try: msg = json.loads(line)
        except: print("C++:", line); continue

        now = time.time()
        if now - main.last_trail_check > 30:
            check_and_trail_positions()
            check_pending_orders()   # 检查超时未成交订单
            main.last_trail_check = now

        t = msg.get("type", "")
        sym = msg.get("symbol", "")

        if t == "A_ACTIVE":
            if sym in last_a_push and now - last_a_push[sym] < 300: continue
            price = msg.get("price", 0)
            change = msg.get("change_pct", 0)
            vol_r = msg.get("vol_ratio", 0)
            dev = msg.get("dev", None)
            d_str = f" | 偏离度:{dev:.1f}" if dev else ""
            send_tg(f"🔥 {sym} 异动 | 价:{price:.4f} | 涨跌:{change:+.2f}% | 量比:{vol_r:.1f}x{d_str}")
            last_a_push[sym] = now

        elif t == "SIGNAL":
            side = msg.get("side", "")
            price_derived = msg.get("price", 0)
            score = msg.get("score", 0)
            stop_loss = msg.get("stop_loss", 0)
            take_profit = msg.get("take_profit", 0)

            if sym in last_b_signal and now - last_b_signal[sym] < 600: continue
            if is_quiet_period(): continue

            actual_price, order_info = place_order(sym, side, price_derived)

            tg_lines = [f"🎯 {side.upper()} {sym} 评分:{score:.1f}"]
            if actual_price and order_info:
                tg_lines.append(f"✅ 下单成功: {actual_price:.6f}")
                # 撤单逻辑将在下次定时检查中处理，这里不立即标记持仓，只有成交后才更新
                last_b_signal[sym] = now
                # 注意：持仓信息会在订单成交后由跟踪止盈模块检查，但我们没有主动轮询成交状态
                # 可在后续版本中增加成交监听。目前若未立即成交，不会记录持仓。
            else:
                err_msg = str(order_info) if order_info else "未知错误"
                tg_lines.append(f"❌ 下单失败: {err_msg[:80]}")

            tg_lines.append(f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}")
            send_tg("\n".join(tg_lines))

    proc.wait()

if __name__ == "__main__":
    main()