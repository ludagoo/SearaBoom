#!/usr/bin/env bash
# Freeze the current firmware/build as the USB factory image (local files only).
# Does not publish OTA. Prefer ./scripts/publish_firmware.sh so OTA and USB match.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$ROOT/scripts/signing_key_backup.py" --require
BUILD="${SEARABOOM_FIRMWARE_BUILD:-$ROOT/firmware/build}"
DEST="${SEARABOOM_FACTORY_DIR:-$ROOT/server/firmware/factory}"
VER="${1:-}"
if [[ -z "$VER" ]]; then
  VER="$(tr -d '[:space:]' < "$ROOT/firmware/VERSION")"
fi

BOOT="$BUILD/bootloader/bootloader.bin"
PART="$BUILD/partition_table/partition-table.bin"
OTAD="$BUILD/ota_data_initial.bin"
APP="$BUILD/searaboom.bin"
STOR="$BUILD/storage.bin"
for f in "$BOOT" "$PART" "$OTAD" "$APP" "$STOR"; do
  if [[ ! -f "$f" ]]; then
    echo "missing $f — build firmware first" >&2
    exit 1
  fi
done

PY="$ROOT/server/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  PY="${PYTHON:-python3}"
fi
"$PY" "$ROOT/server/fw_signature.py" --check "$APP"

mkdir -p "$DEST"
cp "$BOOT" "$DEST/bootloader.bin"
cp "$PART" "$DEST/partition-table.bin"
cp "$OTAD" "$DEST/ota_data_initial.bin"
cp "$APP" "$DEST/app.bin"
cp "$STOR" "$DEST/storage.bin"

python3 - "$DEST" "$VER" <<'PY'
import json, sys
from datetime import datetime, timezone
from pathlib import Path
dest = Path(sys.argv[1])
version = sys.argv[2]
plan = [
    ("bootloader", "bootloader.bin", 0x0),
    ("partitions", "partition-table.bin", 0x8000),
    ("otadata", "ota_data_initial.bin", 0xF000),
    ("app", "app.bin", 0x20000),
    ("storage", "storage.bin", 0x3A0000),
]
meta = {
    "version": version,
    "uploaded_at": datetime.now(timezone.utc).isoformat(),
    "files": [
        {
            "key": key,
            "filename": name,
            "offset": offset,
            "size": (dest / name).stat().st_size,
        }
        for key, name, offset in plan
    ],
}
(dest / "factory.json").write_text(json.dumps(meta, indent=2) + "\n")
print(f"Factory USB image frozen at {version}")
for item in meta["files"]:
    print(f"  {item['filename']}  {item['size']} bytes")
PY
