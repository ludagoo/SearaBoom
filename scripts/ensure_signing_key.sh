#!/usr/bin/env bash
# Link firmware/secure_boot_signing_key.pem for IDF signed-app builds.
#
# Never overwrite ~/.config/searaboom/secure_boot_signing_key.pem. A stray
# SEARABOOM_SIGNING_KEY or firmware/*.pem must not replace that file or rewrite
# the server pin (ota_signing_pubkey.pem) to match a different key.
#
# Does not mint a trust anchor unless invoked with --generate. cmake / idf.py
# build only consume the firmware path; they must not call this script.
set -euo pipefail

GENERATE=0
if [[ "${1:-}" == "--generate" ]]; then
  GENERATE=1
elif [[ -n "${1:-}" ]]; then
  echo "usage: $0 [--generate]" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG_DIR="${SEARABOOM_CONFIG_DIR:-$HOME/.config/searaboom}"
CFG_KEY="$CFG_DIR/secure_boot_signing_key.pem"
CFG_PUB="$CFG_DIR/ota_signing_pubkey.pem"
FW_DIR="${SEARABOOM_FIRMWARE_DIR:-$ROOT/firmware}"
FW_KEY="$FW_DIR/secure_boot_signing_key.pem"
ENV_KEY="${SEARABOOM_SIGNING_KEY:-}"

mkdir -p "$CFG_DIR" "$FW_DIR"

same_key() {
  local a="$1" b="$2"
  [[ -e "$a" && -e "$b" ]] || return 1
  local ra rb
  ra="$(readlink -f "$a")"
  rb="$(readlink -f "$b")"
  [[ "$ra" == "$rb" ]] && return 0
  cmp -s "$a" "$b"
}

fail_conflict() {
  echo "Refusing to touch $CFG_KEY" >&2
  echo "It already exists and differs from $1" >&2
  echo "Unset SEARABOOM_SIGNING_KEY and remove a stray firmware key, or replace" >&2
  echo "the config private key yourself after backing it up. See docs/SIGNED_FIRMWARE.md" >&2
  exit 1
}

if [[ -n "$ENV_KEY" && ! -f "$ENV_KEY" ]]; then
  echo "SEARABOOM_SIGNING_KEY=$ENV_KEY is not a file" >&2
  exit 1
fi

src=""
if [[ -f "$CFG_KEY" ]]; then
  if [[ -n "$ENV_KEY" ]] && ! same_key "$ENV_KEY" "$CFG_KEY"; then
    fail_conflict "$ENV_KEY"
  fi
  src="$CFG_KEY"
elif [[ -n "$ENV_KEY" ]]; then
  # Use the env key for the firmware symlink only. Do not copy it into ~/.config
  # and do not write ota_signing_pubkey.pem from it (that would move the server pin).
  src="$ENV_KEY"
elif [[ -f "$FW_KEY" ]]; then
  # Firmware copy/symlink may exist from an older tree. Use it for the build
  # without promoting it into ~/.config.
  src="$FW_KEY"
fi

if [[ -z "$src" ]]; then
  if [[ "$GENERATE" -ne 1 ]]; then
    echo "No signing key at $CFG_KEY" >&2
    echo "Minting a trust anchor is not a side effect of cmake/idf.py build." >&2
    echo "Once, after you intend to keep this key: $0 --generate" >&2
    echo "See docs/SIGNED_FIRMWARE.md" >&2
    exit 1
  fi
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

# Never copy into CFG_KEY. If it exists, it is the machine's trust anchor.
if [[ -f "$CFG_KEY" && "$src" != "$CFG_KEY" ]] && ! same_key "$src" "$CFG_KEY"; then
  fail_conflict "$src"
fi

if [[ "$(readlink -f "$src")" != "$(readlink -f "$FW_KEY" 2>/dev/null || true)" ]]; then
  if [[ -e "$FW_KEY" || -L "$FW_KEY" ]]; then
    if same_key "$FW_KEY" "$src"; then
      :
    elif [[ -L "$FW_KEY" ]]; then
      ln -sfn "$src" "$FW_KEY"
    else
      echo "Refusing to overwrite $FW_KEY (exists and differs from $src)" >&2
      echo "Remove that file or replace it with a symlink. See docs/SIGNED_FIRMWARE.md" >&2
      exit 1
    fi
  else
    ln -sfn "$src" "$FW_KEY" 2>/dev/null || cp -f "$src" "$FW_KEY"
  fi
fi

# Server pin: only derived from the config private key, never from a stray env
# or firmware PEM. Writing this file is what makes the host pin non-optional
# after the next server restart.
if [[ -f "$CFG_KEY" ]]; then
  openssl rsa -in "$CFG_KEY" -pubout -out "$CFG_PUB" 2>/dev/null
  chmod 644 "$CFG_PUB" 2>/dev/null || true
fi

# Never create signing_key_backed_up. Only Lucas does that after the key is stored.
echo "$FW_KEY"
