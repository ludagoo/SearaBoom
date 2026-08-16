#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/server"
if [[ ! -d .venv ]]; then
  python3 -m venv .venv
  .venv/bin/pip install -r requirements.txt
fi
export SEARABOOM_PUBLIC_URL="${SEARABOOM_PUBLIC_URL:-https://searaboom.goossen.dev}"
export SEARABOOM_ADMIN_TOKEN="${SEARABOOM_ADMIN_TOKEN:-searaboom-dev}"
exec .venv/bin/python app.py
