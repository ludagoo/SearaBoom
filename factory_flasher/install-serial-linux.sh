#!/usr/bin/env bash
# One-time factory-PC serial access (no sudo after this + a log out/in).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RULE="$HERE/99-searaboom-esp.rules"
if [[ ! -f "$RULE" ]]; then
  RULE="$(dirname "$HERE")/scripts/99-searaboom-esp.rules"
fi
if [[ ! -f "$RULE" ]]; then
  echo "missing 99-searaboom-esp.rules next to this script" >&2
  exit 1
fi
echo "Installing udev rule (needs sudo once)..."
sudo cp "$RULE" /etc/udev/rules.d/99-searaboom-esp.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty || true
if getent group uucp >/dev/null; then
  sudo usermod -aG uucp "$USER"
  echo "Added $USER to group uucp."
elif getent group dialout >/dev/null; then
  sudo usermod -aG dialout "$USER"
  echo "Added $USER to group dialout."
else
  echo "No uucp or dialout group; udev MODE=0666 still lets the flasher open the port."
fi
echo
echo "Unplug/replug the box (or reboot). Log out/in if the group change is new."
