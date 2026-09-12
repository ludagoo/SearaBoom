#!/usr/bin/env bash
# Launch the SearaBoom factory desktop flasher (esptool / DTR-RTS).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PARENT="$(dirname "$HERE")"
PYTHON="${SEARABOOM_ESPTOOL_PYTHON:-python3}"
if [[ ! -x "$HERE/.venv/bin/python" ]]; then
  "$PYTHON" -m venv "$HERE/.venv"
  "$HERE/.venv/bin/pip" install -r "$HERE/requirements.txt"
fi
export PYTHONPATH="$PARENT${PYTHONPATH:+:$PYTHONPATH}"
exec "$HERE/.venv/bin/python" -m factory_flasher "$@"
