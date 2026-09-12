#!/usr/bin/env bash
# Ensure firmware/secure_boot_signing_key.pem exists for IDF signed-app builds.
# Prefers ~/.config/searaboom/secure_boot_signing_key.pem (shared by QA worktrees).
# Generates a local RSA-3072 key only if nothing is present. Back that file up
# before the first signed OTA publish — field units will trust it thereafter.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG_DIR="${SEARABOOM_CONFIG_DIR:-$HOME/.config/searaboom}"
CFG_KEY="$CFG_DIR/secure_boot_signing_key.pem"
CFG_PUB="$CFG_DIR/ota_signing_pubkey.pem"
FW_DIR="${SEARABOOM_FIRMWARE_DIR:-$ROOT/firmware}"
FW_KEY="$FW_DIR/secure_boot_signing_key.pem"
ENV_KEY="${SEARABOOM_SIGNING_KEY:-}"

mkdir -p "$CFG_DIR" "$FW_DIR"

src=""
if [[ -n "$ENV_KEY" && -f "$ENV_KEY" ]]; then
  src="$ENV_KEY"
elif [[ -f "$CFG_KEY" ]]; then
  src="$CFG_KEY"
elif [[ -f "$FW_KEY" ]]; then
  src="$FW_KEY"
fi

if [[ -z "$src" ]]; then
  echo "Generating RSA-3072 signing key at $CFG_KEY" >&2
  echo "Back this file up before publishing signed OTA. See docs/SIGNED_FIRMWARE.md" >&2
  openssl genrsa -out "$CFG_KEY" 3072 >/dev/null 2>&1
  chmod 600 "$CFG_KEY"
  cat > "$CFG_DIR/secure_boot_signing_key.NOTICE" <<EOF
This is the SearaBoom Secure Boot V2 / signed-OTA private key for this machine.
Field units that OTA a firmware signed with it will only accept later images
signed with the same key. Keep a backup offline. Do not commit this file.
EOF
  src="$CFG_KEY"
fi

if [[ "$src" != "$CFG_KEY" ]]; then
  cp -f "$src" "$CFG_KEY"
  chmod 600 "$CFG_KEY"
fi
if [[ "$src" != "$FW_KEY" ]]; then
  ln -sfn "$CFG_KEY" "$FW_KEY" 2>/dev/null || cp -f "$CFG_KEY" "$FW_KEY"
fi
if [[ ! -e "$FW_KEY" ]]; then
  cp -f "$CFG_KEY" "$FW_KEY"
fi

openssl rsa -in "$CFG_KEY" -pubout -out "$CFG_PUB" 2>/dev/null
chmod 644 "$CFG_PUB" 2>/dev/null || true
# Never create signing_key_backed_up. Only Lucas does that after the key is stored.
echo "$FW_KEY"
