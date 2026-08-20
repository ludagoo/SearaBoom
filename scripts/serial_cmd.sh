#!/usr/bin/env bash
# Send a line to the device serial console (default: ota).
set -euo pipefail
PORT="${SEARABOOM_PORT:-/dev/ttyACM0}"
CMD="${1:-ota}"
WAIT="${2:-}"
python3 - <<PY
import serial, time
cmd = """$CMD"""
wait = float("${WAIT:-0}") if "${WAIT:-}" else (8.0 if cmd.startswith("audiotest") else 0.5)
s = serial.Serial("$PORT", 115200, timeout=0.5)
time.sleep(0.15)
s.write((cmd + "\n").encode())
s.flush()
end = time.time() + wait
while time.time() < end:
    data = s.read(4096)
    if data:
        print(data.decode("utf-8", "replace"), end="")
s.close()
PY
