#!/usr/bin/env python3
"""PreToolUse: never create the backup marker; block signed flash/publish without it."""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parents[1]
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
from signing_key_backup import MARKER_NAME, marker_is_present, path_looks_like_marker  # noqa: E402

PUBLISH_SCRIPTS = ("dev_ota.sh", "publish_firmware.sh", "snapshot_factory.sh")
SIGNED_FLASH_NEEDLES = (
    "hw_flash.sh",
    "app-flash",
    "bootloader-flash",
    "encrypted-flash",
    "encrypted-app-flash",
    "partition-table-flash",
)

IDF_FLASH_RE = re.compile(
    r"\bidf\.py\b.*\b(flash|app-flash|bootloader-flash|encrypted-flash|"
    r"encrypted-app-flash|partition-table-flash)\b",
    re.I | re.S,
)
NINJA_FLASH_RE = re.compile(r"\b(ninja|cmake)\b.*\bflash\b", re.I)
ESPTOOL_SIGNED_RE = re.compile(
    r"\b(esptool|esptool\.py)\b.*\bwrite_flash\b.*\b(searaboom\.bin|firmware/build/)",
    re.I | re.S,
)


def _deny(reason: str) -> None:
    json.dump({"decision": "deny", "reason": reason}, sys.stdout)


def _allow() -> None:
    json.dump({"decision": "allow"}, sys.stdout)


def _cmd_and_paths(ev: dict) -> tuple[str, list[str]]:
    tool_in = ev.get("toolInput") or ev.get("tool_input") or {}
    if not isinstance(tool_in, dict):
        tool_in = {}
    # Do not treat file-body `contents` as a command: docs mention the marker
    # name and a Write/StrReplace of those files must still be allowed.
    cmd = str(tool_in.get("command") or "")
    paths = []
    for key in ("path", "target_file", "file_path", "target_notebook"):
        raw = tool_in.get(key)
        if raw:
            paths.append(str(raw))
    return cmd, paths


def _is_marker_create(cmd: str, paths: list[str]) -> bool:
    if any(path_looks_like_marker(p) for p in paths):
        return True
    if MARKER_NAME not in cmd:
        return False
    if "signing_key_backup.py" in cmd and ("--require" in cmd or "--status" in cmd):
        return False
    # Agents must not create, copy, or edit the marker. Existence checks go
    # through signing_key_backup.py --status.
    return True


def _is_signed_flash_or_publish(cmd: str) -> bool:
    if any(n in cmd for n in PUBLISH_SCRIPTS):
        return True
    if any(n in cmd for n in SIGNED_FLASH_NEEDLES):
        return True
    if IDF_FLASH_RE.search(cmd):
        return True
    if NINJA_FLASH_RE.search(cmd) and "firmware" in cmd:
        return True
    if ESPTOOL_SIGNED_RE.search(cmd):
        return True
    return False


def _is_publish_script(cmd: str) -> bool:
    return any(n in cmd for n in PUBLISH_SCRIPTS)


def main() -> int:
    try:
        ev = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    cmd, paths = _cmd_and_paths(ev)
    if _is_marker_create(cmd, paths):
        _deny(
            "Agents must never create ~/.config/searaboom/signing_key_backed_up. "
            "Only Lucas creates that marker after the signing key is stored offline."
        )
        return 0
    if _is_signed_flash_or_publish(cmd) and not marker_is_present():
        _deny(
            "Signed USB flash / OTA / factory publish is blocked until Lucas confirms "
            "the signing key is stored by creating ~/.config/searaboom/signing_key_backed_up "
            "(agents must never create that file). See docs/SIGNED_FIRMWARE.md"
        )
        return 0
    if _is_publish_script(cmd):
        cwd = ev.get("workspaceRoot") or ev.get("cwd") or "."
        try:
            branch = subprocess.check_output(
                ["git", "branch", "--show-current"], cwd=cwd, text=True, stderr=subprocess.DEVNULL
            ).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            branch = ""
        if branch != "main":
            _deny(
                f"Publish scripts are only allowed on main (this session is on {branch or 'an unknown branch'}). "
                "Open a PR, wait for USB box checks, merge, then publish when Lucas asks."
            )
            return 0
    _allow()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
