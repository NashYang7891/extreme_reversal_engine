#!/usr/bin/env python3
import subprocess, json, time, os, sys
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
SLIPPAGE_TOLERANCE = 0.002          # 0.2% 滑点

# 跟踪止损参数
TRAILING_ACTIVATION_PCT = 3.0       # 盈利 3% 激活
TRAILING_CALLBACK_PCT = 1.2         # 回撤 1.2% 平仓 (做多)
TRAILING_CALLBACK_PCT_SHORT = 1.2   # 做空反弹 1.2% 平仓

positions = {}                       # 持仓记录

# TG 消息频率控制
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
    IOC 限价单 + 0.2% 滑点。
    返回 (实际成交均价, 订单对象) 或 (None, 详细错误描述)。
    """
    side = side.lower()
    if side == "long": side = "buy"
    elif side == "short": side = "sell"
    if side not in ('buy', 'sell'):
        return None, "无效方向"

    try:
        if not exchange.markets:
            exchange.load_markets()

        # 获取最新盘口
        ticker = exchange.fetch_ticker(symbol)
        ask = ticker.get('ask')
        bid = ticker.get('bid')
        if not ask or not bid:
            return None, "盘口数据缺失"

        # 计算下单价格 (含 0.2% 滑点)
        if side == 'buy':
            base_price = max(price, ask)
            order_price = base_price * 1.002
        else:
            base_price = min(price, bid)
            order_price = base_price * 0.998

        order_price_str = exchange.price_to_precision(symbol, order_price)
        amount = ORDER_USDT / order_price
        amount_str = exchange.amount_to_precision(symbol, amount)

        # 设置杠杆和逐仓
        exchange.set_leverage(LEVERAGE, symbol)
        exchange.set_margin_mode('isolated', symbol)

        # 发送 IOC 订单
        order = exchange.create_order(
            symbol=symbol,
            type='limit',
            side=side,
            amount=amount_str,
            price=order_price_str,
            params={'timeInForce': 'IOC'}
        )

        if not order:
            return None, "提交失败"

        # ----- 解析成交结果 -----
        filled = 0.0
        avg_price = 0.0

        # 从基础字段读取
        if order.get('filled'):
            filled = float(order['filled'])
        if order.get('average'):
            avg_price = float(order['average'])

        # 备选：从 info 中提取
        if filled <= 0 or avg_price <= 0:
            info = order.get('info', {})
            cum_qty = float(info.get('cumQty', 0))
            cum_quote = float(info.get('cumQuote', 0))
            if cum_qty > 0 and cum_quote > 0:
                avg_price = cum_quote / cum_qty
                filled = cum_qty

        if filled > 0 and avg_price > 0:
            print(f"✅ IOC 成交: {symbol} {side} @ {avg_price:.6f} 量:{filled} (本:{price:.6f})")
            return avg_price, order
        else:
            # 未成交，记录价格对比
            print(f"⚠ IOC 未成交: {symbol} {side} 本:{price:.6f} 买一:{bid:.6f} 卖一:{ask:.6f} 下单:{order_price_str}")
            return None, "IOC未成交"

    except ccxt.InsufficientFunds:
        return None, "保证金不足"
    except ccxt.PermissionDenied as e:
        return None, f"无交易权限: {e}"
    except ccxt.BadSymbol as e:
        return None, f"无效币种: {e}"
    except Exception as e:
        print(f"❌ 下单异常 {symbol}: {e}")
        return None, f"异常:{str(e)[:60]}"

def update_positions_after_fill(symbol, side, entry_price, order):
    try:
        qty_val = float(order.get('filled') or order.get('amount') or 0)
    except:
        qty_val = 0.0
    if qty_val <= 0:
        return
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
    send_tg("🤖 引擎启动 (IOC 0.2%滑点·成交解析优化)")

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
                tg_lines.append(f"✅ 成交: {actual_price:.6f}")
                update_positions_after_fill(sym, side, actual_price, order_info)
                last_b_signal[sym] = now
            else:
                err_msg = str(order_info) if order_info else "未成交"
                tg_lines.append(f"❌ 未成交: {err_msg[:80]}")

            tg_lines.append(f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}")
            send_tg("\n".join(tg_lines))

    proc.wait()

if __name__ == "__main__":
    main()