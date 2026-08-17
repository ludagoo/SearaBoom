#!/usr/bin/env bash
# Bump firmware patch version (0.0.X), rebuild, publish OTA, optionally force-update device via serial.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${SEARABOOM_PORT:-/dev/ttyACM0}"
TOKEN="${SEARABOOM_ADMIN_TOKEN:-searaboom-dev}"
URL="${SEARABOOM_PUBLIC_URL:-https://searaboom.goossen.dev}"
FLASH_FIRST=0
NO_TRIGGER=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --flash) FLASH_FIRST=1; shift ;;
    --no-trigger) NO_TRIGGER=1; shift ;;
    --port) PORT="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--flash] [--no-trigger] [--port /dev/ttyACM0]"
      echo "  Bumps patch in firmware/VERSION, builds, publishes, sends serial 'ota'."
      exit 0
      ;;
    *) echo "unknown arg: $1"; exit 1 ;;
  esac
done

VER_FILE="$ROOT/firmware/VERSION"
cur="$(tr -d '[:space:]' < "$VER_FILE")"
IFS=. read -r MA MI PA <<< "$cur"
PA=$((PA + 1))
NEW="${MA}.${MI}.${PA}"
echo "$NEW" > "$VER_FILE"
# Keep Kconfig default string in sync for log helpers
sed -i "s/^CONFIG_SEARABOOM_FW_VERSION=.*/CONFIG_SEARABOOM_FW_VERSION=\"$NEW\"/" \
  "$ROOT/firmware/sdkconfig.defaults"
if [[ -f "$ROOT/firmware/sdkconfig" ]]; then
  sed -i "s/^CONFIG_SEARABOOM_FW_VERSION=.*/CONFIG_SEARABOOM_FW_VERSION=\"$NEW\"/" \
    "$ROOT/firmware/sdkconfig"
fi
echo "Version -> $NEW"

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export ADF_PATH="${ADF_PATH:-$HOME/esp/esp-adf}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null
cd "$ROOT/firmware"
idf.py build

BIN="$ROOT/firmware/build/searaboom.bin"
test -f "$BIN"

curl -fsS -X POST "$URL/api/firmware/upload" \
  -H "X-Admin-Token: $TOKEN" \
  -F "version=$NEW" \
  -F "token=$TOKEN" \
  -F "firmware=@$BIN"
echo
echo "Published $NEW"

if [[ "$FLASH_FIRST" -eq 1 ]]; then
  idf.py -p "$PORT" flash
fi

if [[ "$NO_TRIGGER" -eq 0 ]]; then
  if [[ -e "$PORT" ]]; then
    echo "Triggering serial OTA on $PORT"
    python3 - <<PY
import serial, time
port = "$PORT"
s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.2)
s.write(b"ota\n")
s.flush()
# Show a bit of output while update starts
end = time.time() + 8
while time.time() < end:
    data = s.read(4096)
    if data:
        print(data.decode("utf-8", "replace"), end="")
s.close()
PY
  else
    echo "No serial $PORT — publish done; trigger later with: echo ota > $PORT"
  fi
fi

echo "Done. Watch: ./scripts/watch_serial.sh   or   ./scripts/watch_logs.sh"
