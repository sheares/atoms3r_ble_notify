#!/bin/bash
# BLE-notify hook wrapper. Reads the Claude Code hook JSON from stdin,
# extracts session_id (and tool_name for the "tool" endpoint), and forwards
# to the daemon. Used by all hooks so session-count tracking sees every event.
#
# Usage:
#   ble_notify_hook.sh thinking      → GET /thinking?session_id=...
#   ble_notify_hook.sh question      → GET /question?session_id=...
#   ble_notify_hook.sh notify        → GET /notify?session_id=...
#   ble_notify_hook.sh tool          → GET /tool/<tool_name>?session_id=...
#   ble_notify_hook.sh tool-clear    → GET /tool?session_id=...

ENDPOINT="$1"
J=$(cat 2>/dev/null || true)
S=$(printf '%s' "$J" | jq -r '.session_id // empty' 2>/dev/null || true)
# Pull cwd from the hook payload (falls back to current working dir), take
# the basename, then pick the longest token (split by _ - space) that fits
# in LABEL_MAX chars. If nothing fits, truncate the full basename.
CWD=$(printf '%s' "$J" | jq -r '.cwd // empty' 2>/dev/null || true)
[ -z "$CWD" ] && CWD="$PWD"
LABEL_MAX=5
BASE=$(basename "$CWD")
LABEL=$(printf '%s' "$BASE" | awk -v M=$LABEL_MAX '
{
    gsub(/[ _-]+/, "\n")
    n = split($0, tok, "\n")
    best = ""
    for (i = 1; i <= n; i++) {
        t = tok[i]
        gsub(/[^A-Za-z0-9]/, "", t)
        if (length(t) <= M && length(t) > length(best)) best = t
    }
    if (best == "") best = substr($0, 1, M)
    gsub(/[^A-Za-z0-9]/, "", best)
    print best
}')

case "$ENDPOINT" in
    tool)
        N=$(printf '%s' "$J" | jq -r '.tool_name // empty' 2>/dev/null || true)
        if [ -n "$N" ]; then
            URL="http://127.0.0.1:8765/tool/$N"
        else
            URL="http://127.0.0.1:8765/tool"
        fi
        ;;
    tool-clear)
        URL="http://127.0.0.1:8765/tool"
        ;;
    *)
        URL="http://127.0.0.1:8765/$ENDPOINT"
        ;;
esac

if [ -n "$S" ]; then
    curl -s -G --max-time 2 \
        --data-urlencode "session_id=$S" \
        --data-urlencode "label=$LABEL" \
        "$URL" > /dev/null 2>&1 || true
else
    curl -s --max-time 2 "$URL" > /dev/null 2>&1 || true
fi
