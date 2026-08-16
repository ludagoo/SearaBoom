#!/usr/bin/env bash
# One-time host setup: passwordless access to ESP USB serial (/dev/ttyACM*).
set -euo pipefail

RULE_SRC="$(cd "$(dirname "$0")" && pwd)/99-searaboom-esp.rules"
RULE_DST=/etc/udev/rules.d/99-searaboom-esp.rules

if [[ ! -f "$RULE_SRC" ]]; then
  echo "missing $RULE_SRC" >&2
  exit 1
fi

echo "Installing udev rule (needs sudo once)..."
sudo cp "$RULE_SRC" "$RULE_DST"
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty || true
sudo usermod -aG uucp "$USER"

echo
echo "Done. Log out/in (or reboot) so group 'uucp' applies."
echo "After that, idf.py flash/monitor should work without sudo/password."
