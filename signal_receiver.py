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
MAX_ACTIVE_ORDERS = 5                # 最多同时挂单数

# 跟踪止损参数
TRAILING_ACTIVATION_PCT = 3.0        # 盈利 3% 激活
TRAILING_CALLBACK_PCT = 1.2          # 回撤 1.2% 平仓 (做多)
TRAILING_CALLBACK_PCT_SHORT = 1.2    # 做空反弹 1.2% 平仓

positions = {}           # 持仓记录
active_a_orders = {}     # 挂单记录（可选，此处用于熔断计数）
last_b_signal = {}        # B 层信号冷却 (symbol -> timestamp)
last_a_push = {}          # A 层去重冷却

# TG 频率控制
LAST_TG_SEND = 0
TG_RATE_LIMIT_SEC = 0.5

# API 熔断
api_fail_count = 0
API_FAIL_THRESHOLD = 5
api_pause_until = 0
PAUSE_MINUTES = 10

def send_tg(msg):
    """只在需要时发送 TG，提供频率限制"""
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

# ---------- 启动时同步持仓与挂单 ----------
def sync_positions_on_start():
    """从交易所获取当前持仓和挂单，恢复冷却状态"""
    try:
        # 同步持仓
        pos_list = exchange.fetch_positions()
        for p in pos_list:
            amount = float(p.get('contracts', 0))
            if amount > 0:
                symbol = p['symbol']
                side = 'LONG' if p.get('side') == 'long' else 'SHORT'
                entry_price = float(p.get('entryPrice', 0))
                if entry_price > 0:
                    positions[symbol] = {
                        'side': side,
                        'entry_price': entry_price,
                        'qty': amount,
                        'highest_price': entry_price,
                        'lowest_price': entry_price,
                        'trailing_activated': False
                    }
                    # 将对应币种加入 B 层冷却 (避免立即同币种信号)
                    last_b_signal[symbol] = time.time()
                    print(f"📥 恢复持仓: {symbol} {side} @ {entry_price:.6f} Qty:{amount}")

        # 同步未成交挂单（用于熔断计数）
        orders = exchange.fetch_open_orders()
        for order in orders:
            if order.get('type') == 'limit' and order.get('status') == 'open':
                active_a_orders[order['id']] = {
                    'symbol': order['symbol'],
                    'side': order.get('side', ''),
                    'order': order,
                    'time': order.get('timestamp', 0)/1000
                }
        print(f"📋 已同步 {len(positions)} 个持仓, {len(active_a_orders)} 个挂单")
    except Exception as e:
        print(f"⚠ 同步持仓失败: {e}")

# ---------- IOC 下单 ----------
def get_market_price(symbol, fallback):
    """获取盘口价，严重故障时返回 fallback"""
    global api_fail_count, api_pause_until
    if time.time() < api_pause_until:
        return fallback, fallback, True
    try:
        ticker = exchange.fetch_ticker(symbol)
        api_fail_count = 0
        ask = ticker.get('ask') or ticker.get('last') or fallback
        bid = ticker.get('bid') or ticker.get('last') or fallback
        return bid, ask, False
    except Exception as e:
        api_fail_count += 1
        print(f"⚠ fetch_ticker 失败 ({api_fail_count}/{API_FAIL_THRESHOLD}): {e}")
        if api_fail_count >= API_FAIL_THRESHOLD:
            api_pause_until = time.time() + PAUSE_MINUTES * 60
            send_tg(f"🚨 API 异常，暂停下单 {PAUSE_MINUTES} 分钟")
        return fallback, fallback, True

def place_order(symbol, side, price):
    """
    IOC 限价单 + 0.2% 滑点。
    返回 (实际成交均价, 订单对象) 或 (None, 错误信息字符串)。
    """
    side = side.lower()
    if side == "long": side = "buy"
    elif side == "short": side = "sell"
    if side not in ('buy', 'sell'):
        return None, "无效方向"

    # 已有持仓，不再开新仓
    if symbol in positions:
        return None, "已有持仓"

    # API 熔断
    if time.time() < api_pause_until:
        return None, "API熔断暂停"

    try:
        if not exchange.markets:
            exchange.load_markets()

        bid, ask, troubled = get_market_price(symbol, price)
        if troubled and api_fail_count >= API_FAIL_THRESHOLD:
            return None, "API严重异常"

        if side == 'buy':
            base_price = max(price, ask)
            order_price = base_price * (1.0 + SLIPPAGE_TOLERANCE)
        else:
            base_price = min(price, bid)
            order_price = base_price * (1.0 - SLIPPAGE_TOLERANCE)

        order_price_str = exchange.price_to_precision(symbol, order_price)
        amount = ORDER_USDT / order_price
        amount_str = exchange.amount_to_precision(symbol, amount)

        exchange.set_leverage(LEVERAGE, symbol)
        exchange.set_margin_mode('isolated', symbol)

        order = exchange.create_order(
            symbol=symbol, type='limit', side=side,
            amount=amount_str, price=order_price_str,
            params={'timeInForce': 'IOC'}
        )

        if not order:
            return None, "提交失败"

        filled = float(order.get('filled', 0))
        avg_price = float(order.get('average', 0))
        if filled <= 0 or avg_price <= 0:
            info = order.get('info', {})
            cum_qty = float(info.get('cumQty', 0))
            cum_quote = float(info.get('cumQuote', 0))
            if cum_qty > 0 and cum_quote > 0:
                avg_price = cum_quote / cum_qty
                filled = cum_qty

        if filled > 0 and avg_price > 0:
            print(f"✅ 成交: {symbol} {side} @ {avg_price:.6f} 量:{filled}")
            return avg_price, order
        else:
            print(f"⚠ IOC 未成交: {symbol} {side}")
            return None, "IOC未成交"

    except ccxt.InsufficientFunds:
        return None, "保证金不足"
    except ccxt.PermissionDenied as e:
        return None, f"无交易权限: {e}"
    except Exception as e:
        print(f"❌ 下单异常 {symbol}: {e}")
        return None, str(e)[:100]

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
    print(f"📊 持仓: {symbol} {pos_side} @ {entry_price:.6f} 量:{qty_val}")

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

    # ----- 启动时同步持仓与挂单 -----
    sync_positions_on_start()
    # 同步完成后发送启动消息
    send_tg("🤖 引擎已启动 (静默模式+状态同步)")

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True, bufsize=1)
    main.last_trail_check = 0

    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try: msg = json.loads(line)
        except: print("C++:", line); continue

        now = time.time()
        # 定期检查跟踪止盈
        if now - main.last_trail_check > 30:
            check_and_trail_positions()
            main.last_trail_check = now

        t = msg.get("type", "")
        sym = msg.get("symbol", "")

        # ---------- A 层异动 ----------
        if t == "A_ACTIVE":
            if sym in last_a_push and now - last_a_push[sym] < 300:
                continue
            price = msg.get("price", 0)
            change = msg.get("change_pct", 0)
            vol_r = msg.get("vol_ratio", 0)
            dev = msg.get("dev", None)
            d_str = f" | 偏离度:{dev:.1f}" if dev else ""
            send_tg(f"🔥 {sym} 异动 | 价:{price:.4f} | 涨跌:{change:+.2f}% | 量比:{vol_r:.1f}x{d_str}")
            last_a_push[sym] = now

        # ---------- B 层信号 ----------
        elif t == "SIGNAL":
            # 信号过期丢弃
            msg_ts = msg.get("timestamp", 0)
            if msg_ts > 0 and (time.time() - msg_ts) > 2.0:
                continue

            side = msg.get("side", "")
            price_derived = msg.get("price", 0)
            score = msg.get("score", 0)
            stop_loss = msg.get("stop_loss", 0)
            take_profit = msg.get("take_profit", 0)

            # B 层冷却：10 分钟内同币种不重复
            if sym in last_b_signal and now - last_b_signal[sym] < 600:
                continue

            # 静默期跳过
            if is_quiet_period():
                continue

            # 仓位已满拦截（不推送）
            if len(active_a_orders) >= MAX_ACTIVE_ORDERS:
                print(f"🛑 挂单已满，拦截 {sym}")
                continue

            # 尝试下单
            actual_price, order_info = place_order(sym, side, price_derived)
            if not actual_price:
                # 下单失败：仅控制台输出，不推送 TG
                reason = order_info or "未知"
                print(f"❌ 下单未成功 ({sym}): {reason}")
                continue

            # ---- 下单成功，仅此时推送 TG ----
            tg_lines = [f"🎯 {side.upper()} {sym} 评分:{score:.1f}",
                        f"✅ 成交: {actual_price:.6f}",
                        f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}"]
            send_tg("\n".join(tg_lines))
            update_positions_after_fill(sym, side, actual_price, order_info)
            last_b_signal[sym] = now

            # 清理对应方向的旧挂单（如果有）
            order_key = f"{sym}_{side.lower()}"
            if order_key in active_a_orders:
                try:
                    exchange.cancel_order(active_a_orders[order_key]['order']['id'], sym)
                    del active_a_orders[order_key]
                except:
                    pass

    proc.wait()

if __name__ == "__main__":
    main()