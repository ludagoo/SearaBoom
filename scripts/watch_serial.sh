#!/usr/bin/env bash
# Tail USB serial from the connected SearaBoom.
set -euo pipefail
PORT="${1:-${SEARABOOM_PORT:-/dev/ttyACM0}}"
BAUD="${2:-115200}"
exec python3 - <<PY
import serial, sys
port, baud = "$PORT", int("$BAUD")
print(f"watching {port} @ {baud} (Ctrl-C to stop)", flush=True)
s = serial.Serial(port, baud, timeout=0.5)
try:
    while True:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    s.close()
PY
