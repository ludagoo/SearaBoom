#!/usr/bin/env bash
# Send a line to the device serial console (default: ota).
set -euo pipefail
PORT="${SEARABOOM_PORT:-/dev/ttyACM0}"
CMD="${1:-ota}"
python3 - <<PY
import serial, time
s = serial.Serial("$PORT", 115200, timeout=1)
time.sleep(0.15)
s.write(b"$CMD\n")
s.flush()
time.sleep(0.5)
print(s.read(8192).decode("utf-8", "replace"), end="")
s.close()
PY
