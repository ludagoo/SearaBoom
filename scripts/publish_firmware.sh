#!/usr/bin/env bash
# Publish an already-built firmware/build as OTA and USB factory (same version).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:?usage: publish_firmware.sh X.Y.Z}"
BIN="$ROOT/firmware/build/searaboom.bin"
TOKEN="${SEARABOOM_ADMIN_TOKEN:-searaboom-dev}"
URL="${SEARABOOM_PUBLIC_URL:-https://searaboom.goossen.dev}"
test -f "$BIN"

PY="$ROOT/server/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  PY="${PYTHON:-python3}"
fi
"$PY" "$ROOT/server/fw_signature.py" --check "$BIN"

curl -fsS -X POST "$URL/api/firmware/upload" \
  -H "X-Admin-Token: $TOKEN" \
  -F "version=$VER" \
  -F "token=$TOKEN" \
  -F "firmware=@$BIN"
echo
echo "Published OTA $VER"

BOOT="$ROOT/firmware/build/bootloader/bootloader.bin"
PART="$ROOT/firmware/build/partition_table/partition-table.bin"
OTAD="$ROOT/firmware/build/ota_data_initial.bin"
STOR="$ROOT/firmware/build/storage.bin"
if [[ -f "$BOOT" && -f "$PART" && -f "$OTAD" && -f "$STOR" ]]; then
  curl -fsS -X POST "$URL/api/factory/upload" \
    -H "X-Admin-Token: $TOKEN" \
    -F "token=$TOKEN" \
    -F "slot=live" \
    -F "version=$VER" \
    -F "bootloader=@$BOOT" \
    -F "partitions=@$PART" \
    -F "otadata=@$OTAD" \
    -F "app=@$BIN" \
    -F "storage=@$STOR"
  echo
  echo "Published USB factory $VER"
else
  echo "Skipping USB factory (missing bootloader/partitions/otadata/storage in firmware/build)" >&2
fi
