#!/usr/bin/env python3
import subprocess, json, time, os, sys, threading, requests
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

LEVERAGE = 5                      # 杠杆倍数（已改为5倍）
ORDER_USDT = 30.0                 # 开仓金额 (USDT) 保证金
SLIPPAGE_TOLERANCE = 0.002
MAX_ACTIVE_ORDERS = 5

# ---------- 跟踪止盈参数（固定U数） ----------
TRAIL_ACTIVE_PROFIT_U = 3.5       # 盈利超过3.5U激活跟踪
TRAIL_CALLBACK_U = 1.5            # 回撤1.5U平仓

positions = {}               # 持仓记录
active_a_orders = {}         # 限价单记录
last_b_signal = {}
last_fail_push = {}
last_a_push = {}

# 死亡开关
stop_loss_counter = {}
BLACKLIST = {}
BLACKLIST_DURATION = 4 * 3600
STOP_LOSS_THRESHOLD = 2
last_htf_check = {}
trend_status = {}

LAST_TG_SEND = 0
TG_RATE_LIMIT_SEC = 0.5

api_fail_count = 0
API_FAIL_THRESHOLD = 5
api_pause_until = 0
PAUSE_MINUTES = 10

def send_tg(msg):
    global LAST_TG_SEND
    now = time.time()
    if now - LAST_TG_SEND < TG_RATE_LIMIT_SEC:
        return
    LAST_TG_SEND = now
    try:
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

# ---------- 1h 趋势防线 ----------
def update_htf_trend_for_symbol(symbol):
    now = time.time()
    if symbol in last_htf_check and now - last_htf_check[symbol] < 900:
        return
    last_htf_check[symbol] = now
    try:
        ohlcv = exchange.fetch_ohlcv(symbol, '1h', 5)
        if not ohlcv or len(ohlcv) < 3:
            return
        closes = [c[4] for c in ohlcv]
        ema = sum(closes) / len(closes)
        cur = closes[-1]
        slope = closes[-1] - closes[-2]
        if cur > ema * 1.05 and slope > 0 and closes[-1] > closes[-3]:
            trend_status[symbol] = "STRONG_BULL"
        elif cur < ema * 0.95 and slope < 0 and closes[-1] < closes[-3]:
            trend_status[symbol] = "STRONG_BEAR"
        else:
            trend_status[symbol] = "NORMAL"
    except Exception as e:
        print(f"⚠ 趋势更新失败 {symbol}: {e}")

def is_against_trend(symbol, side):
    update_htf_trend_for_symbol(symbol)
    trend = trend_status.get(symbol, "NORMAL")
    if side == "SHORT" and trend == "STRONG_BULL":
        send_tg(f"🛑 拦截 {symbol} 做空\n原因: 1H 强牛趋势")
        return True
    if side == "LONG" and trend == "STRONG_BEAR":
        send_tg(f"🛑 拦截 {symbol} 做多\n原因: 1H 强熊趋势")
        return True
    return False

# ---------- 死亡开关 ----------
def record_stop_loss(symbol):
    stop_loss_counter[symbol] = stop_loss_counter.get(symbol, 0) + 1
    msg = f"⚠ {symbol} 止损计数: {stop_loss_counter[symbol]}/{STOP_LOSS_THRESHOLD}"
    print(msg); send_tg(msg)
    if stop_loss_counter[symbol] >= STOP_LOSS_THRESHOLD:
        BLACKLIST[symbol] = time.time() + BLACKLIST_DURATION
        msg = f"🚫 {symbol} 触发死亡开关，拉黑 {BLACKLIST_DURATION//3600} 小时"
        print(msg); send_tg(msg)

def is_blacklisted(symbol):
    if symbol in BLACKLIST:
        if time.time() < BLACKLIST[symbol]:
            return True
        else:
            del BLACKLIST[symbol]
            stop_loss_counter[symbol] = 0
    return False

# ---------- 启动同步 ----------
def sync_positions_on_start():
    try:
        pos_list = exchange.fetch_positions()
        live_positions = set()
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
                        'trailing_activated': False,
                        'stop_order_id': None
                    }
                    live_positions.add(symbol)
                    last_b_signal[symbol] = time.time()
                    print(f"📥 恢复持仓: {symbol} {side} @ {entry_price:.6f} Qty:{amount}")

        orders = exchange.fetch_open_orders()
        for order in orders:
            sym = order.get('symbol')
            if order.get('type') == 'STOP_MARKET' and order.get('reduceOnly') and sym in positions:
                positions[sym]['stop_order_id'] = order.get('id')
                print(f"🔒 保留止损单: {sym} ID:{order['id']}")
            else:
                try:
                    exchange.cancel_order(order['id'], sym)
                    print(f"🧹 启动清理多余挂单: {sym} ID:{order['id']}")
                except Exception as e:
                    print(f"⚠ 取消 {sym} 订单 {order['id']} 失败: {e}")

        for sym in list(positions.keys()):
            if sym not in live_positions:
                _force_clear_all(sym)
                del positions[sym]
                print(f"🧹 清理已消失持仓的挂单: {sym}")

        active_a_orders.clear()
        print(f"📋 当前持仓 {len(positions)} 个，挂单已同步")
    except Exception as e:
        print(f"⚠ 同步持仓失败: {e}")

# ---------- 彻底清空该币种所有挂单 ----------
def _force_clear_all(symbol):
    try:
        exchange.cancel_all_orders(symbol)
        time.sleep(0.5)
        remaining = exchange.fetch_open_orders(symbol)
        if remaining:
            exchange.cancel_all_orders(symbol)
            time.sleep(0.3)
        final = exchange.fetch_open_orders(symbol)
        if final:
            print(f"⚠ {symbol} 仍有 {len(final)} 个挂单无法取消")
            return False
        return True
    except Exception as e:
        print(f"⚠ 清空 {symbol} 挂单失败: {e}")
        return False

# ---------- IOC 下单（修正数量计算） ----------
def get_market_price(symbol, fallback):
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
        if api_fail_count >= API_FAIL_THRESHOLD:
            api_pause_until = time.time() + PAUSE_MINUTES * 60
            send_tg(f"🚨 API 异常，暂停下单 {PAUSE_MINUTES} 分钟")
        return fallback, fallback, True

def place_order_ioc(symbol, side, price):
    side = side.lower()
    if side == "long": side = "buy"
    elif side == "short": side = "sell"
    if side not in ('buy', 'sell'):
        return None, "无效方向"

    if symbol in positions:
        return None, "已有持仓"
    if is_blacklisted(symbol):
        return None, "死亡开关拉黑"
    if time.time() < api_pause_until:
        return None, "API熔断暂停"

    try:
        if not exchange.markets:
            exchange.load_markets()

        # 获取市场信息用于最小名义价值检查
        market = exchange.market(symbol)
        min_notional = 5.0  # 默认兜底值
        if market['limits'] and market['limits']['cost'] and market['limits']['cost'].get('min'):
            min_notional = market['limits']['cost']['min']

        # 获取步长精度
        step_size = 0.001
        if market['precision'] and market['precision']['amount']:
            step_size = market['precision']['amount']
        else:
            filters = market.get('info', {}).get('filters', [])
            lot_filter = next((f for f in filters if f.get('filterType') == 'LOT_SIZE'), {})
            step_size_str = lot_filter.get('stepSize')
            if step_size_str:
                step_size = float(step_size_str)

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

        # ========== 核心修正：计算数量 ==========
        desired_notional = ORDER_USDT * LEVERAGE  # 名义价值 = 保证金 × 杠杆
        if desired_notional < min_notional:
            desired_notional = min_notional
            print(f"⚠️ {symbol} 名义价值 {ORDER_USDT}*{LEVERAGE}={ORDER_USDT*LEVERAGE} USDT 低于最小限制 {min_notional}，已上调至 {min_notional} USDT")

        raw_amount = desired_notional / order_price
        # 根据步长取整
        raw_amount = (raw_amount // step_size) * step_size
        amount_str = exchange.amount_to_precision(symbol, raw_amount)

        # 最终名义价值校验
        final_notional = float(amount_str) * order_price
        if final_notional < min_notional - 1e-9:
            return None, f"调整后名义价值 {final_notional:.2f} USDT 仍低于最小限制 {min_notional}"

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
        err_msg = str(e)[:200]
        print(f"❌ 下单异常 {symbol}: {err_msg}")
        return None, err_msg

def place_order(symbol, side, price):
    _force_clear_all(symbol)
    avg, err = place_order_ioc(symbol, side, price)
    if avg is not None:
        return avg, err
    if isinstance(err, str) and "-4067" in err:
        print(f"🔄 再次强制清空 {symbol} 挂单")
        _force_clear_all(symbol)
        time.sleep(1)
        avg, err = place_order_ioc(symbol, side, price)
    return avg, err

def cancel_order(order_id, symbol):
    try:
        exchange.cancel_order(order_id, symbol)
        print(f"🗑 已撤单 {symbol} {order_id}")
    except Exception as e:
        print(f"⚠ 撤单失败 {symbol} {order_id}: {e}")

# ---------- 止损挂单 ----------
def place_stop_loss_order(symbol, side, qty, stop_price):
    try:
        if side.upper() == 'LONG':
            stop_side = 'sell'
        else:
            stop_side = 'buy'
        stop_price_str = exchange.price_to_precision(symbol, stop_price)
        qty_str = exchange.amount_to_precision(symbol, qty)
        params = {'stopPrice': stop_price_str, 'reduceOnly': True, 'workingType': 'MARK_PRICE'}
        order = exchange.create_order(symbol, 'STOP_MARKET', stop_side, qty_str, None, params)
        print(f"🛑 止损单已挂: {symbol} {stop_side} @ {stop_price_str}")
        return order
    except Exception as e:
        print(f"❌ 挂止损单失败 {symbol}: {e}")
        return None

def cancel_stop_order(symbol):
    if symbol in positions and positions[symbol].get('stop_order_id'):
        oid = positions[symbol]['stop_order_id']
        try:
            exchange.cancel_order(oid, symbol)
            positions[symbol]['stop_order_id'] = None
            print(f"🗑 已取消原止损单 {symbol} {oid}")
        except Exception as e:
            print(f"⚠ 取消止损单失败 {symbol} {oid}: {e}")

def update_positions_after_fill(symbol, side, entry_price, order, stop_loss):
    try:
        qty_val = float(order.get('filled') or order.get('amount') or 0)
    except:
        qty_val = 0.0
    if qty_val <= 0: return
    pos_side = "LONG" if side == "buy" else "SHORT"
    positions[symbol] = {
        'side': pos_side,
        'entry_price': entry_price,
        'qty': qty_val,
        'highest_price': entry_price,
        'lowest_price': entry_price,
        'trailing_activated': False,
        'stop_order_id': None
    }
    stop_order = place_stop_loss_order(symbol, pos_side, qty_val, stop_loss)
    if stop_order:
        positions[symbol]['stop_order_id'] = stop_order['id']
    print(f"📊 持仓记录: {symbol} {pos_side} @ {entry_price:.6f} 数量:{qty_val}")

# ---------- 安全平仓 ----------
def safe_close_position(symbol, side, reason=""):
    try:
        positions_info = exchange.fetch_positions(symbols=[symbol])
        if not positions_info or len(positions_info) == 0:
            print(f"⚠ 未找到 {symbol} 持仓")
            return
        pos_info = positions_info[0]
        actual_qty = abs(float(pos_info.get('contracts', 0)))
        if actual_qty <= 0:
            print(f"⚠ {symbol} 持仓为0")
            return
        qty_str = exchange.amount_to_precision(symbol, actual_qty)
        if float(qty_str) == 0:
            return
        cancel_stop_order(symbol)
        order_side = 'sell' if side.upper() == 'LONG' else 'buy'
        exchange.create_order(symbol=symbol, type='market', side=order_side,
                              amount=qty_str, params={'reduceOnly': True})
        entry = positions[symbol]['entry_price'] if symbol in positions else 0
        mark_price = pos_info.get('markPrice', 0)
        pnl_pct = ((mark_price - entry) / entry * 100) if entry > 0 else 0
        send_tg(f"🔻 {side} {symbol} 平仓\n价格: {mark_price:.6f}\n数量: {actual_qty}\n盈亏: {pnl_pct:+.2f}%\n原因: {reason}")
        print(f"✅ 平仓成功: {symbol} {side} Qty:{qty_str} ({reason})")
        # 死亡开关记录
        if pnl_pct < 0 and reason.find("跟踪止盈") >= 0:
            record_stop_loss(symbol)
        elif pnl_pct < 0 and reason.find("止损") >= 0:
            record_stop_loss(symbol)
        if symbol in positions:
            del positions[symbol]
    except Exception as e:
        print(f"❌ 平仓失败 {symbol}: {e}")

# ---------- 跟踪止盈（固定U数版） ----------
def check_and_trail_positions():
    if not positions:
        return

    # 1. 检测已消失的持仓（被止损单平仓）
    try:
        current_positions = exchange.fetch_positions()
        real_pos = {p['symbol']: p for p in current_positions if float(p.get('contracts', 0)) > 0}
        closed_syms = [sym for sym in positions if sym not in real_pos]
        for sym in closed_syms:
            entry = positions[sym]['entry_price']
            side = positions[sym]['side']
            send_tg(f"ℹ️ 检测到 {sym} {side} 已不在持仓中（可能已被止损）")
            _force_clear_all(sym)
            # 尝试记录止损次数
            try:
                mark_price = real_pos.get(sym, {}).get('markPrice', 0)
                if side == 'LONG' and mark_price and entry:
                    last_pnl = (mark_price - entry) / entry * 100
                elif side == 'SHORT' and mark_price and entry:
                    last_pnl = (entry - mark_price) / entry * 100
                else:
                    last_pnl = 0
                if last_pnl < 0:
                    record_stop_loss(sym)
            except:
                pass
            del positions[sym]
    except Exception as e:
        print(f"⚠ 持仓同步检查失败: {e}")

    # 2. 跟踪止盈逻辑（用U计算）
    for sym in list(positions.keys()):
        if sym not in positions:
            continue
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
            if current_price > pos['highest_price']:
                pos['highest_price'] = current_price

            profit_u = (current_price - entry) * qty
            if profit_u >= TRAIL_ACTIVE_PROFIT_U:
                pos['trailing_activated'] = True

            if pos['trailing_activated']:
                drawdown_u = (pos['highest_price'] - current_price) * qty
                if drawdown_u >= TRAIL_CALLBACK_U:
                    send_tg(f"🛑 做多跟踪止盈平仓 {sym} @ {current_price:.6f} (回撤 {drawdown_u:.2f}U)")
                    safe_close_position(sym, 'LONG', f"跟踪止盈回撤{drawdown_u:.2f}U")
        else:  # SHORT
            if current_price < pos['lowest_price']:
                pos['lowest_price'] = current_price

            profit_u = (entry - current_price) * qty
            if profit_u >= TRAIL_ACTIVE_PROFIT_U:
                pos['trailing_activated'] = True

            if pos['trailing_activated']:
                drawdown_u = (current_price - pos['lowest_price']) * qty
                if drawdown_u >= TRAIL_CALLBACK_U:
                    send_tg(f"🛑 做空跟踪止盈平仓 {sym} @ {current_price:.6f} (回撤 {drawdown_u:.2f}U)")
                    safe_close_position(sym, 'SHORT', f"跟踪止盈回撤{drawdown_u:.2f}U")

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

    sync_positions_on_start()
    send_tg("🤖 引擎已启动 (固定U跟踪止盈 | 杠杆5倍 | 名义价值150U)")

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True, bufsize=1)
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
            msg_ts = msg.get("timestamp", 0)
            if msg_ts > 0 and (time.time() - msg_ts) > 2.0:
                continue

            side = msg.get("side", "")
            price_derived = msg.get("price", 0)
            score = msg.get("score", 0)
            stop_loss = msg.get("stop_loss", 0)
            take_profit = msg.get("take_profit", 0)

            if sym in last_b_signal and now - last_b_signal[sym] < 600: continue
            if is_quiet_period(): continue
            if is_blacklisted(sym):
                print(f"🚫 {sym} 死亡开关生效，跳过")
                continue
            if is_against_trend(sym, side):
                continue
            if len(active_a_orders) >= MAX_ACTIVE_ORDERS:
                print(f"🛑 挂单已满，拦截 {sym}")
                continue

            actual_price, order_info = place_order(sym, side, price_derived)
            if actual_price and order_info:
                tg_msg = (f"🎯 {side.upper()} {sym} 评分:{score:.1f}\n"
                          f"✅ 成交: {actual_price:.6f}\n"
                          f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}")
                send_tg(tg_msg)
                update_positions_after_fill(sym, side, actual_price, order_info, stop_loss)
                last_b_signal[sym] = now
                order_key = f"{sym}_{side.lower()}"
                if order_key in active_a_orders:
                    try:
                        exchange.cancel_order(active_a_orders[order_key]['order']['id'], sym)
                        del active_a_orders[order_key]
                    except: pass
            else:
                fail_key = f"{sym}_{side.lower()}"
                if fail_key in last_fail_push and now - last_fail_push[fail_key] < 300:
                    continue
                reason = order_info or "未知"
                tg_msg = (f"⚠ {side.upper()} {sym} 评分:{score:.1f}\n"
                          f"📛 未成交: {reason}\n"
                          f"🛑 止损: {stop_loss:.6f} | 🎯 止盈: {take_profit:.6f}")
                send_tg(tg_msg)
                last_fail_push[fail_key] = now
                last_b_signal[sym] = now

    proc.wait()

if __name__ == "__main__":
    main()