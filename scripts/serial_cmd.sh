#!/usr/bin/env bash
# Send a line to a device serial console.
# Usage: serial_cmd.sh [--box ID] [command] [wait_seconds]
# Default command is "help" (not ota). QA boxes: pass --box. Dev box: SEARABOOM_PORT.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOX=""
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --box) BOX="${2:?}"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--box <id>] [command] [wait_seconds]"
      exit 0
      ;;
    *) ARGS+=("$1"); shift ;;
  esac
done
CMD="${ARGS[0]:-help}"
WAIT="${ARGS[1]:-}"
if [[ -n "$BOX" ]]; then
  PORT="$(python3 "$ROOT/scripts/qa_boxes.py" path "$BOX")"
else
  PORT="${SEARABOOM_PORT:-/dev/ttyACM0}"
fi
PY="${SEARABOOM_ESPTOOL_PYTHON:-$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/python}"
export PORT CMD WAIT
"$PY" - <<'PY'
import os, serial, time
port = os.environ["PORT"]
cmd = os.environ["CMD"]
wait_s = os.environ.get("WAIT") or ""
wait = float(wait_s) if wait_s else (8.0 if cmd.startswith("audiotest") else 0.5)
s = serial.Serial(port, 115200, timeout=0.5)
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
