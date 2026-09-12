#!/usr/bin/env python3
"""Fail-closed gate: signed flash/publish only after Lucas confirms the key is stored.

Marker (Lucas creates this himself; agents must never create it):
  ~/.config/searaboom/signing_key_backed_up

No --force. Tests set HOME to a temp dir (Path.home() follows HOME).
Policy forbids using that as a bypass.
"""
from __future__ import annotations

import argparse
import os
import stat
import sys
from pathlib import Path

MARKER_NAME = "signing_key_backed_up"
EXIT_BLOCKED = 3

FLASH_IDF_ACTIONS = frozenset(
    {
        "flash",
        "app-flash",
        "bootloader-flash",
        "encrypted-flash",
        "encrypted-app-flash",
        "partition-table-flash",
    }
)

MESSAGE = """\
Refusing signed USB flash / OTA / factory publish: Lucas has not confirmed
the signing key is stored.

After the key is backed up offline, Lucas (not an agent) creates:
  ~/.config/searaboom/signing_key_backed_up

Build/compile may still generate or use the key. Agents must never create
that marker. See docs/SIGNED_FIRMWARE.md
"""


class SigningKeyBackupError(RuntimeError):
    pass


def marker_path() -> Path:
    # Do not honor SEARABOOM_CONFIG_DIR: agents could point it at /tmp.
    return Path.home() / ".config" / "searaboom" / MARKER_NAME


def marker_is_present() -> bool:
    path = marker_path()
    try:
        st = path.lstat()
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISREG(st.st_mode):
        return False
    return st.st_size >= 0


def require_backup() -> Path:
    path = marker_path()
    if not marker_is_present():
        raise SigningKeyBackupError(MESSAGE.strip())
    return path


def path_looks_like_marker(raw: str) -> bool:
    name = Path(str(raw)).name
    return name == MARKER_NAME


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Require Lucas's signing-key backup marker")
    p.add_argument("--require", action="store_true", help="exit 3 unless the marker exists")
    p.add_argument("--status", action="store_true", help="print present/absent")
    args = p.parse_args(argv)
    if args.status:
        print("present" if marker_is_present() else "absent")
        return 0 if marker_is_present() else 1
    if args.require:
        try:
            require_backup()
        except SigningKeyBackupError as exc:
            print(exc, file=sys.stderr)
            return EXIT_BLOCKED
        return 0
    p.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
