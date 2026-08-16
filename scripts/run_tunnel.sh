#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$ROOT/tunnel/config.yml"
exec /usr/bin/cloudflared tunnel --config "$CFG" --protocol http2 run searaboom
