#!/usr/bin/env bash
# Flash the live USB factory image onto one QA box. Does not publish OTA.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOX=""
QUIET_VOL="${SEARABOOM_QA_QUIET_VOLUME:-1}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --box) BOX="${2:?}"; shift 2 ;;
    -h|--help) echo "usage: $0 --box <id>"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$BOX" ]]; then
  echo "usage: $0 --box <id>" >&2
  exit 2
fi
PORT="$(python3 "$ROOT/scripts/qa_boxes.py" path "$BOX")"
LOCK="$(python3 "$ROOT/scripts/qa_boxes.py" lock "$BOX")"
DEST="$ROOT/server/firmware/factory"
PY="${SEARABOOM_ESPTOOL_PYTHON:-$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/python}"
for f in bootloader.bin partition-table.bin ota_data_initial.bin app.bin storage.bin; do
  if [[ ! -f "$DEST/$f" ]]; then
    echo "missing $DEST/$f — cannot restore factory" >&2
    exit 3
  fi
done
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "box $BOX busy ($LOCK)" >&2
  exit 4
fi
"$PY" -u -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 "$DEST/bootloader.bin" \
  0x8000 "$DEST/partition-table.bin" \
  0xF000 "$DEST/ota_data_initial.bin" \
  0x20000 "$DEST/app.bin" \
  0x3A0000 "$DEST/storage.bin"
for _ in $(seq 1 40); do
  if [[ -e "$PORT" ]]; then
    break
  fi
  sleep 0.25
done
sleep 2
"$ROOT/scripts/serial_cmd.sh" --box "$BOX" "vol $QUIET_VOL" 1.5 || true
echo "restored factory on $BOX ($PORT)"
