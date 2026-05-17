#!/usr/bin/env python3
import errno
import json
import os
import stat
import threading
import time

import requests
import websocket

# 配置
MIN_24H_VOLUME = 80000000
PIPE_PATH = "/tmp/price_pipe"
SYMBOLS_PATH = os.getenv("SYMBOLS_PATH", "/tmp/extreme_reversal_symbols.json")

# FIFO 重连参数
PIPE_OPEN_RETRY_SEC = 1.0
PIPE_REOPEN_LOCK = threading.Lock()
PIPE_WAIT_LOG_EVERY = 10
WS_NO_MESSAGE_TIMEOUT_SEC = 20
STREAM_MODES = ("aggTrade", "trade", "markPrice")

pipe_fd = None
stats_lock = threading.Lock()
stats_started_at = time.time()
last_stats_log = stats_started_at
last_ws_message_at = stats_started_at
ws_messages = 0
pipe_writes = 0
non_trade_messages = 0


def ensure_fifo_exists():
    if not os.path.exists(PIPE_PATH):
        os.mkfifo(PIPE_PATH)
        print(f"[pipe] created fifo: {PIPE_PATH}", flush=True)
        return

    st_mode = os.stat(PIPE_PATH).st_mode
    if not stat.S_ISFIFO(st_mode):
        os.remove(PIPE_PATH)
        os.mkfifo(PIPE_PATH)
        print(f"[pipe] replaced non-fifo path: {PIPE_PATH}", flush=True)


def close_pipe():
    global pipe_fd
    if pipe_fd is not None:
        try:
            os.close(pipe_fd)
        except OSError:
            pass
        finally:
            pipe_fd = None


def init_pipe_blocking():
    """
    阻塞式打开 FIFO 写端：
    - 读端未启动时会等待，不会因首次打开失败退出。
    - 结合重试循环实现启动自恢复。
    """
    global pipe_fd
    wait_count = 0
    while True:
        try:
            ensure_fifo_exists()
            fd = os.open(PIPE_PATH, os.O_WRONLY)
            pipe_fd = fd
            print(f"[pipe] writer connected: {PIPE_PATH}", flush=True)
            return
        except Exception as e:
            wait_count += 1
            if wait_count == 1 or wait_count % PIPE_WAIT_LOG_EVERY == 0:
                print(
                    f"[pipe] waiting reader... retry {wait_count}, interval {PIPE_OPEN_RETRY_SEC:.1f}s, err={e}",
                    flush=True,
                )
            time.sleep(PIPE_OPEN_RETRY_SEC)


def ensure_pipe_connected():
    global pipe_fd
    if pipe_fd is not None:
        return
    with PIPE_REOPEN_LOCK:
        if pipe_fd is None:
            init_pipe_blocking()


def reopen_pipe():
    with PIPE_REOPEN_LOCK:
        close_pipe()
        init_pipe_blocking()


def write_price(symbol, price):
    global pipe_writes
    payload = json.dumps({"symbol": symbol, "price": price}) + "\n"

    while True:
        ensure_pipe_connected()
        try:
            os.write(pipe_fd, payload.encode())
            with stats_lock:
                pipe_writes += 1
            return
        except BrokenPipeError:
            print("[pipe] broken pipe detected, reconnecting...", flush=True)
            reopen_pipe()
        except OSError as e:
            # 关键恢复场景：EPIPE / ENXIO
            if e.errno in (errno.EPIPE, errno.ENXIO):
                print(f"[pipe] write errno={e.errno}, reconnecting...", flush=True)
                reopen_pipe()
            elif e.errno == errno.EINTR:
                continue
            elif e.errno == errno.EAGAIN:
                time.sleep(0.05)
            else:
                print(f"[pipe] write unexpected error, reconnecting: {e}", flush=True)
                reopen_pipe()


def fetch_symbols():
    try:
        with open(SYMBOLS_PATH, "r", encoding="utf-8") as f:
            symbols = json.load(f)
        symbols = [str(s) for s in symbols if str(s).endswith("USDT")]
        if symbols:
            print(f"[ws] loaded symbols from {SYMBOLS_PATH}: {len(symbols)}", flush=True)
            return symbols
    except Exception as e:
        print(f"[ws] load symbols file failed: {e}", flush=True)

    symbols = fetch_symbols_from_binance()
    if symbols:
        tmp_path = SYMBOLS_PATH + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(symbols, f)
        os.replace(tmp_path, SYMBOLS_PATH)
        print(f"[ws] wrote symbols file {SYMBOLS_PATH}: {len(symbols)}", flush=True)
    return symbols

def fetch_symbols_from_binance():
    url = "https://fapi.binance.com/fapi/v1/ticker/24hr"
    try:
        resp = requests.get(url, timeout=10)
        resp.raise_for_status()
        data = resp.json()

        symbols = []
        for item in data:
            sym = item.get("symbol", "")
            if sym.endswith("USDT") and not any(x in sym for x in ("_", "USDC")):
                vol = float(item.get("quoteVolume", 0))
                if vol >= MIN_24H_VOLUME:
                    symbols.append(sym)
        return symbols
    except Exception as e:
        print(f"[ws] fetch symbols failed: {e}", flush=True)
        return []


def log_stats(force=False):
    global last_stats_log
    now = time.time()
    if not force and now - last_stats_log < 30:
        return
    with stats_lock:
        elapsed = max(now - stats_started_at, 1.0)
        print(
            f"[stats] ws_messages={ws_messages} pipe_writes={pipe_writes} "
            f"non_trade_messages={non_trade_messages} "
            f"idle_sec={now - last_ws_message_at:.0f} uptime_sec={elapsed:.0f}",
            flush=True,
        )
        last_stats_log = now


def on_message(ws, message):
    global ws_messages, non_trade_messages, last_ws_message_at
    try:
        last_ws_message_at = time.time()
        msg = json.loads(message)
        trade = msg.get("data", msg)
        if "p" in trade and "s" in trade:
            symbol = trade["s"]
            price = float(trade["p"])
            with stats_lock:
                ws_messages += 1
            write_price(symbol, price)
            log_stats()
        else:
            with stats_lock:
                non_trade_messages += 1
            if non_trade_messages <= 5:
                print(f"[ws] non-trade message: {msg}", flush=True)
            log_stats()
    except Exception as e:
        print(f"[ws] handle message error: {e}", flush=True)


def on_error(ws, error):
    print(f"[ws] error: {error}", flush=True)


def on_close(ws, close_status_code, close_msg):
    print(f"[ws] closed code={close_status_code} msg={close_msg}, reconnect in 5s...", flush=True)


def on_open(ws, stream_count, mode):
    global last_ws_message_at
    last_ws_message_at = time.time()
    print(f"[ws] connected combined streams: {stream_count} mode={mode}", flush=True)


def stream_name(symbol, mode):
    if mode == "markPrice":
        return f"{symbol.lower()}@markPrice@1s"
    return f"{symbol.lower()}@{mode}"


def no_message_watchdog(ws, mode):
    while True:
        time.sleep(5)
        idle = time.time() - last_ws_message_at
        if idle >= WS_NO_MESSAGE_TIMEOUT_SEC:
            print(f"[ws] no messages for {idle:.0f}s on mode={mode}, switching stream mode", flush=True)
            try:
                ws.close()
            except Exception as e:
                print(f"[ws] watchdog close failed: {e}", flush=True)
            return


def connect_and_run():
    symbols = fetch_symbols()
    if not symbols:
        print("[ws] no symbols fetched, retry in 10s", flush=True)
        time.sleep(10)
        return False

    for mode in STREAM_MODES:
        streams = [stream_name(symbol, mode) for symbol in symbols[:200]]
        print(f"[ws] monitoring {len(streams)} symbols, start bridging trades mode={mode}", flush=True)
        ws_url = "wss://fstream.binance.com/stream?streams=" + "/".join(streams)
        ws = websocket.WebSocketApp(
            ws_url,
            on_open=lambda ws, mode=mode: on_open(ws, len(streams), mode),
            on_message=on_message,
            on_error=on_error,
            on_close=on_close,
        )
        threading.Thread(target=no_message_watchdog, args=(ws, mode), daemon=True).start()
        ws.run_forever(ping_interval=30, ping_timeout=10)
        time.sleep(5)

    return True


def stats_heartbeat():
    while True:
        time.sleep(30)
        log_stats(force=True)


if __name__ == "__main__":
    if os.getenv("WS_TRACE", "").lower() in ("1", "true", "yes"):
        websocket.enableTrace(True)
    ensure_pipe_connected()
    threading.Thread(target=stats_heartbeat, daemon=True).start()
    while True:
        connect_and_run()
        time.sleep(5)
