#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:?usage: publish_firmware.sh X.Y.Z}"
BIN="$ROOT/firmware/build/searaboom.bin"
TOKEN="${SEARABOOM_ADMIN_TOKEN:-searaboom-dev}"
URL="${SEARABOOM_PUBLIC_URL:-https://searaboom.goossen.dev}"
test -f "$BIN"
curl -fsS -X POST "$URL/api/firmware/upload" \
  -H "X-Admin-Token: $TOKEN" \
  -F "version=$VER" \
  -F "token=$TOKEN" \
  -F "firmware=@$BIN"
echo
