#!/usr/bin/env bash
# Build firmware in the current tree and USB-flash one QA box. Never publishes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOX=""
QUIET_VOL="${SEARABOOM_QA_QUIET_VOLUME:-1}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --box) BOX="${2:?}"; shift 2 ;;
    -h|--help) echo "usage: $0 --box <zero-fast|supermini-fast|zero-soak|supermini-soak>"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$BOX" ]]; then
  echo "usage: $0 --box <id>" >&2
  exit 2
fi
PORT="$(python3 "$ROOT/scripts/qa_boxes.py" path "$BOX")"
LOCK="$(python3 "$ROOT/scripts/qa_boxes.py" lock "$BOX")"
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "box $BOX busy ($LOCK)" >&2
  exit 4
fi

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export ADF_PATH="${ADF_PATH:-$HOME/esp/esp-adf}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null
# Link-only: symlink the existing config key into this worktree. Does not mint.
"$ROOT/scripts/ensure_signing_key.sh"
cd "$ROOT/firmware"
idf.py build
python3 "$ROOT/scripts/signing_key_backup.py" --require
idf.py -p "$PORT" flash

for _ in $(seq 1 40); do
  if [[ -e "$PORT" ]]; then
    break
  fi
  sleep 0.25
done
sleep 2
"$ROOT/scripts/serial_cmd.sh" --box "$BOX" "vol $QUIET_VOL" 1.5
echo "flashed $BOX on $PORT (vol $QUIET_VOL)"
