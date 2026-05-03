#!/usr/bin/env python3
import subprocess, json, time, os, sys
from datetime import datetime, timezone, timedelta
from dotenv import load_dotenv
import ccxt

# 加载 .env 中的 API 密钥
load_dotenv()
API_KEY = os.getenv("BINANCE_API_KEY")
SECRET_KEY = os.getenv("BINANCE_SECRET_KEY")

TG_TOKEN = "8722422674:AAGrKmRurQ2G__j-Vxbh5451v0e9_u97CQY"
TG_CHAT = "5372217316"

# 策略固定参数
LEVERAGE = 3            # 3倍杠杆
ORDER_USDT = 10.0       # 每次下单金额 (USDT)

# ---------- 交易所初始化 ----------
exchange = ccxt.binance({
    'apiKey': API_KEY,
    'secret': SECRET_KEY,
    'enableRateLimit': True,
    'options': {'defaultType': 'future'},
})

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

def place_order(symbol, side, price, usdt_amount=ORDER_USDT):
    """
    币安期货限价单（Post-Only），固定 USDT 金额，3倍杠杆逐仓
    """
    try:
        if not exchange.markets:
            exchange.load_markets()
        market = exchange.market(symbol)

        # ---- 设置杠杆与逐仓模式 (幂等操作，可重复调用) ----
        try:
            exchange.set_leverage(LEVERAGE, symbol)
        except Exception as e:
            print(f"⚠️ 设置杠杆失败 {symbol}: {e}")
        try:
            exchange.set_margin_mode('isolated', symbol)   # 逐仓模式
        except Exception:
            pass  # 可能已经为逐仓，忽略报错

        # ---- 计算数量与精度 ----
        amount = usdt_amount / price
        amount = exchange.amount_to_precision(symbol, amount)
        price_str = exchange.price_to_precision(symbol, price)

        # ---- 下单 ----
        params = {
            'timeInForce': 'GTX',   # Post-Only
            'postOnly': True,
        }
        order = exchange.create_order(
            symbol=symbol,
            type='limit',
            side=side.lower(),
            amount=amount,
            price=price_str,
            params=params
        )
        print(f"✅ 下单成功: {symbol} {side} @ {price_str} 数量:{amount} (约{usdt_amount}U, {LEVERAGE}x)")
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

    # 预加载市场信息
    print("加载币安合约市场信息...")
    try:
        exchange.load_markets()
        print("✅ 市场数据已加载")
    except Exception as e:
        print(f"⚠️ 加载市场数据失败: {e}，将尝试在线获取")

    proc = subprocess.Popen([engine_path], stdout=subprocess.PIPE, text=True)
    send_tg(f"🤖 币安极端反转引擎已启动（{ORDER_USDT}U/{LEVERAGE}x 自动交易）")
    last_signal = {}

    for line in proc.stdout:
        line = line.strip()
        if not line: continue
        try:
            sig = json.loads(line)
        except json.JSONDecodeError:
            print("非JSON输出:", line)
            continue

        symbol = sig.get("symbol", "")
        side = sig.get("side", "")
        price = sig.get("price", 0)
        score = sig.get("score", 0)

        now = time.time()
        if symbol in last_signal and now - last_signal[symbol] < 3600:
            print(f"⏰ {symbol} 冷却中，跳过")
            continue

        if is_quiet_period():
            print("🔇 静默期，跳过信号")
            continue

        # TG 推送
        msg = f"⚡ {side.upper()} {symbol} @ {price:.6f} 评分:{score:.1f}"
        send_tg(msg)

        # 自动下单
        place_order(symbol, side, price)

        last_signal[symbol] = now

if __name__ == "__main__":
    main()