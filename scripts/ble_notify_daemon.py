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
  GET /tool/<name>   — show tool icon in badge slot during thinking
  GET /tool          — clear tool icon

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
from datetime import date, datetime
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

# Per-session tracking. Sessions older than SESSION_TTL_S are evicted.
#   _session_seen   : sid → last-seen monotonic timestamp
#   _session_states : sid → 'T'/'W'/'D' (state char for the bar)
#   _session_slots  : sid → 0..3 (stable slot index, freed only on eviction)
_session_seen:   dict[str, float] = {}
_session_states: dict[str, str]   = {}
_session_slots:  dict[str, int]   = {}
_last_bar_payload:  str = ""
_last_face_state:   str = ""
SESSION_TTL_S = 5 * 60
MAX_SLOTS     = 4


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
            # Push today's theme + current hour on connect
            await _push_theme()
            await _push_time()
            # Force a re-broadcast of bar + face so reconnects refresh state.
            global _last_bar_payload, _last_face_state
            _last_bar_payload = ""
            _last_face_state = ""
            await _broadcast_session_view()
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


def _evict_old_sessions() -> None:
    now = time.monotonic()
    stale = [sid for sid, ts in _session_seen.items() if now - ts > SESSION_TTL_S]
    for sid in stale:
        _session_seen.pop(sid, None)
        _session_states.pop(sid, None)
        _session_slots.pop(sid, None)


def _alloc_slot(sid: str) -> int | None:
    """Assign a stable 0..MAX_SLOTS-1 slot to sid. Returns None if full."""
    if sid in _session_slots:
        return _session_slots[sid]
    used = set(_session_slots.values())
    for i in range(MAX_SLOTS):
        if i not in used:
            _session_slots[sid] = i
            return i
    return None  # no free slot — session is tracked but not shown on bar


def _build_bar_payload() -> str:
    """Build the bar codes string up to the highest occupied slot index + 1.
    Empty interior slots show as '.'. Returns '' if no slots are occupied."""
    if not _session_slots:
        return ""
    highest = max(_session_slots.values())
    slots = ["."] * (highest + 1)
    for sid, idx in _session_slots.items():
        slots[idx] = _session_states.get(sid, ".")
    return "".join(slots)


def _compute_face_state() -> str:
    """Pick the face command based on highest-urgency state across sessions.
    Priority: W (waiting) > D (done) > T (thinking) > standby."""
    states = set(_session_states.values())
    if "W" in states: return "waiting"
    if "D" in states: return "done"
    if "T" in states: return "thinking"
    return "standby"


async def _broadcast_session_view() -> None:
    """Send bar + face commands if either has changed since the last broadcast."""
    global _last_bar_payload, _last_face_state
    bar = _build_bar_payload()
    face = _compute_face_state()
    if bar != _last_bar_payload:
        _last_bar_payload = bar
        if bar:
            await _send(f"bar {bar}")
        # No-bar case: nothing to send; firmware will fall back on next render.
    if face != _last_face_state:
        _last_face_state = face
        await _send(face)


async def _set_session_state(session_id: str | None, state: str | None) -> None:
    """Mark a session as seen and (optionally) update its bar state.
    state in {'T','W','D', None}; None just refreshes the seen-time (e.g. /tool).
    Broadcasts updated bar + face if anything changed."""
    if session_id:
        _session_seen[session_id] = time.monotonic()
        if state is not None:
            _alloc_slot(session_id)
            _session_states[session_id] = state
    _evict_old_sessions()
    await _broadcast_session_view()


async def _session_evictor() -> None:
    """Periodically evict stale sessions and re-broadcast if anything changed."""
    while True:
        await asyncio.sleep(30)
        _evict_old_sessions()
        await _broadcast_session_view()


async def _push_time() -> None:
    await _send(f"time {datetime.now().hour}")


async def _hourly_time_pusher() -> None:
    """Re-push current hour shortly after each hour ticks over."""
    while True:
        now = datetime.now()
        # Sleep until 30s after the next hour boundary, so the firmware
        # crosses mood bands close to but slightly after the hour change.
        seconds_until_next_hour = 3600 - (now.minute * 60 + now.second) + 30
        await asyncio.sleep(seconds_until_next_hour)
        await _push_time()


# ─── HTTP handlers ────────────────────────────────────────────────────────────

def _sid(req: web.Request) -> str | None:
    """Pull session_id from query string. Returns None if absent."""
    sid = req.query.get("session_id")
    return sid if sid else None


async def handle_thinking(req: web.Request) -> web.Response:
    global _last_dizzy_ts
    sid = _sid(req)
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
        # Still update bar state so it's accurate after dizzy clears.
        await _set_session_state(sid, "T")
        await _send("dizzy")
        return web.Response(text="OK (dizzy)\n")

    await _set_session_state(sid, "T")
    return web.Response(text="OK\n")


async def handle_question(req: web.Request) -> web.Response:
    await _set_session_state(_sid(req), "W")
    return web.Response(text="OK\n")


async def handle_notify(req: web.Request) -> web.Response:
    await _set_session_state(_sid(req), "D")
    return web.Response(text="OK\n")


async def handle_clear(req: web.Request) -> web.Response:
    # Manual override — forces face to standby without touching per-session state.
    # (Per-session state is still refreshed via the seen-time path.)
    await _set_session_state(_sid(req), None)
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


# Map raw Claude Code tool names to the short tokens the firmware parses.
# Firmware does prefix match on: edit/multiedit/read/write/bash/web/fetch/grep/glob/task/agent.
# Anything else → TOOL_OTHER (three dots).
_TOOL_ALIAS = {
    "edit": "edit",
    "multiedit": "multiedit",
    "read": "read",
    "write": "write",
    "bash": "bash",
    "bashoutput": "bash",
    "killshell": "bash",
    "grep": "grep",
    "glob": "glob",
    "webfetch": "fetch",
    "websearch": "web",
    "task": "task",
    "agent": "agent",
    "notebookedit": "edit",
}


async def handle_tool(req: web.Request) -> web.Response:
    # Tool calls just refresh seen-time; they don't change the bar state.
    await _set_session_state(_sid(req), None)
    name = req.match_info.get("name", "").strip().lower()
    if not name:
        await _send("tool")  # clear
        return web.Response(text="OK tool=(clear)\n")
    token = _TOOL_ALIAS.get(name, "other")
    await _send(f"tool {token}")
    return web.Response(text=f"OK tool={token}\n")


async def handle_tool_clear(req: web.Request) -> web.Response:
    await _set_session_state(_sid(req), None)
    await _send("tool")
    return web.Response(text="OK tool=(clear)\n")


async def handle_sessions(req: web.Request) -> web.Response:
    """Manual debug override: GET /sessions/<n>. Sends legacy `sessions <n>`
    plus a synthetic `bar` with n idle slots — useful for layout testing.
    Does NOT touch the real session tracker."""
    try:
        n = int(req.match_info.get("n", ""))
    except ValueError:
        return web.Response(status=400, text="n must be 0-4\n")
    if n < 0 or n > 4:
        return web.Response(status=400, text="n must be 0-4\n")
    await _send(f"sessions {n}")
    if n > 0:
        await _send(f"bar {'.' * n}")
    return web.Response(text=f"OK sessions={n}\n")


async def handle_time(req: web.Request) -> web.Response:
    """Manual override: GET /time/<hh>. Firmware derives mood from hour."""
    try:
        hh = int(req.match_info.get("hh", ""))
    except ValueError:
        return web.Response(status=400, text="hh must be 0-23\n")
    if hh < 0 or hh > 23:
        return web.Response(status=400, text="hh must be 0-23\n")
    await _send(f"time {hh}")
    return web.Response(text=f"OK time={hh}\n")


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
    app.router.add_get("/tool/{name}",  handle_tool)
    app.router.add_get("/tool",         handle_tool_clear)
    app.router.add_get("/time/{hh}",    handle_time)
    app.router.add_get("/sessions/{n}", handle_sessions)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", PORT)
    await site.start()
    log.info("HTTP bridge ready on http://127.0.0.1:%d", PORT)
    log.info("today's theme: %s", _theme_for_date(date.today()))

    asyncio.create_task(_hourly_time_pusher())
    asyncio.create_task(_session_evictor())

    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("stopped")
        sys.exit(0)
