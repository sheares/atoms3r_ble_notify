#!/usr/bin/env python3
"""
BLE Notify Daemon
Bridges localhost HTTP → AtomS3R BLE NUS (Nordic UART Service).

Endpoints:
  GET /thinking      — Claude is processing
  GET /question      — Claude is waiting for input
  GET /notify        — Claude finished (done)
  GET /clear         — reset to standby
  GET /theme/<name>  — manually set theme (testing)

Side effects:
  • /thinking firing >10 times in 30s → sends "dizzy" instead.
  • On BLE connect → picks a theme from today's date (SG holidays + weekends)
    and sends "theme <name>".
"""

import asyncio
import logging
import sys
import time
from collections import deque
from datetime import date
from aiohttp import web
from bleak import BleakClient, BleakScanner

DEVICE_NAME = "AtomS3R-Notify"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
PORT        = 8765

# Rapid-fire glitch
RAPID_WINDOW_S   = 30
RAPID_THRESHOLD  = 10
RAPID_COOLDOWN_S = 60

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [ble-notify] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

_client: BleakClient | None = None
_loop:   asyncio.AbstractEventLoop | None = None
_thinking_history: deque = deque()
_last_dizzy_ts: float = 0.0
_current_theme: str = ""


# ─── Singapore holiday calendar ──────────────────────────────────────────────
# Variable-date holidays are listed per year. Update yearly.
_FIXED_HOLIDAYS = {
    (1, 1):  "new_year",
    (5, 1):  "default",       # Labour Day — no specific theme
    (8, 9):  "national_day",
    (12, 25): "christmas",
    (12, 31): "new_year",
}

_VARIABLE_HOLIDAYS = {
    2026: {
        (2, 17): "cny",        # Chinese New Year day 1
        (2, 18): "cny",        # Chinese New Year day 2
        (3, 21): "default",    # Hari Raya Puasa
        (4, 3):  "default",    # Good Friday
        (5, 27): "default",    # Hari Raya Haji
        (5, 31): "default",    # Vesak Day
        (11, 8): "deepavali",
    },
    2027: {
        (2, 6):  "cny",
        (2, 7):  "cny",
        (3, 10): "default",
        (3, 26): "default",
        (5, 16): "default",
        (5, 21): "default",
        (10, 28): "deepavali",
    },
}


def _theme_for_date(d: date) -> str:
    key = (d.month, d.day)
    if key in _FIXED_HOLIDAYS:
        return _FIXED_HOLIDAYS[key]
    yr = _VARIABLE_HOLIDAYS.get(d.year, {})
    if key in yr:
        return yr[key]
    if d.weekday() >= 5:  # 5=Sat, 6=Sun
        return "weekend"
    return "default"


# ─── BLE connection management ────────────────────────────────────────────────

def _on_disconnect(client: BleakClient) -> None:
    global _client
    log.warning("disconnected — scanning for device...")
    _client = None
    if _loop:
        asyncio.run_coroutine_threadsafe(_connect_loop(), _loop)


async def _connect_loop() -> None:
    global _client
    while True:
        try:
            log.info("scanning for '%s'...", DEVICE_NAME)
            device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
            if device is None:
                log.warning("not found, retrying in 5 s")
                await asyncio.sleep(5)
                continue
            client = BleakClient(device, disconnected_callback=_on_disconnect)
            await client.connect()
            _client = client
            log.info("connected to %s (%s)", DEVICE_NAME, device.address)
            # Push today's theme on connect
            await _push_theme()
            return
        except Exception as exc:
            log.error("connection failed: %s — retrying in 5 s", exc)
            await asyncio.sleep(5)


async def _send(cmd: str) -> None:
    if _client is None or not _client.is_connected:
        log.warning("not connected — command dropped: %s", cmd)
        return
    try:
        await _client.write_gatt_char(NUS_RX_UUID, cmd.encode(), response=False)
        log.info("→ %s", cmd)
    except Exception as exc:
        log.error("send failed: %s", exc)


async def _push_theme() -> None:
    global _current_theme
    theme = _theme_for_date(date.today())
    _current_theme = theme
    await _send(f"theme {theme}")


# ─── HTTP handlers ────────────────────────────────────────────────────────────

async def handle_thinking(req: web.Request) -> web.Response:
    global _last_dizzy_ts
    now = time.monotonic()
    _thinking_history.append(now)
    # Drop entries older than the window
    while _thinking_history and _thinking_history[0] < now - RAPID_WINDOW_S:
        _thinking_history.popleft()

    if (len(_thinking_history) > RAPID_THRESHOLD
            and (now - _last_dizzy_ts) > RAPID_COOLDOWN_S):
        _last_dizzy_ts = now
        _thinking_history.clear()
        log.info("rapid-fire detected → dizzy")
        await _send("dizzy")
        return web.Response(text="OK (dizzy)\n")

    await _send("thinking")
    return web.Response(text="OK\n")


async def handle_question(req: web.Request) -> web.Response:
    await _send("waiting")
    return web.Response(text="OK\n")


async def handle_notify(req: web.Request) -> web.Response:
    await _send("done")
    return web.Response(text="OK\n")


async def handle_clear(req: web.Request) -> web.Response:
    await _send("standby")
    return web.Response(text="OK\n")


async def handle_theme(req: web.Request) -> web.Response:
    name = req.match_info.get("name", "default")
    # Whitelist to avoid junk reaching firmware parser
    allowed = {"default", "weekend", "cny", "christmas", "national_day", "deepavali", "new_year"}
    if name not in allowed:
        return web.Response(status=400, text=f"unknown theme: {name}\n")
    await _send(f"theme {name}")
    return web.Response(text=f"OK theme={name}\n")


# ─── Main ─────────────────────────────────────────────────────────────────────

async def main() -> None:
    global _loop
    _loop = asyncio.get_running_loop()

    await _connect_loop()

    app = web.Application()
    app.router.add_get("/thinking",     handle_thinking)
    app.router.add_get("/question",     handle_question)
    app.router.add_get("/notify",       handle_notify)
    app.router.add_get("/clear",        handle_clear)
    app.router.add_get("/theme/{name}", handle_theme)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", PORT)
    await site.start()
    log.info("HTTP bridge ready on http://127.0.0.1:%d", PORT)
    log.info("today's theme: %s", _theme_for_date(date.today()))

    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("stopped")
        sys.exit(0)
