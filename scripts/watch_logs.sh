#!/usr/bin/env bash
# Tail remote device logs from the OTA server (SSE).
set -euo pipefail
URL="${SEARABOOM_PUBLIC_URL:-https://searaboom.goossen.dev}"
DEVICE="${1:-}"
STREAM="$URL/api/logs/stream"
if [[ -n "$DEVICE" ]]; then
  STREAM="$STREAM?device=$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$DEVICE")"
fi
echo "watching $STREAM (Ctrl-C to stop)"
curl -NsS "$STREAM" | python3 - <<'PY'
import sys, json
for raw in sys.stdin:
    line = raw.strip()
    if not line.startswith("data:"):
        continue
    try:
        ev = json.loads(line[5:].strip())
    except Exception:
        continue
    print(f"[{ev.get('device_id','?')}] {ev.get('line','')}", flush=True)
PY
