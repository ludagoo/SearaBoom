#!/usr/bin/env python3
"""PreToolUse: deny OTA/factory publish unless git branch is main."""
from __future__ import annotations

import json
import subprocess
import sys

NEEDLES = ("dev_ota.sh", "publish_firmware.sh", "snapshot_factory.sh")


def main() -> int:
    try:
        ev = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    tool = ev.get("toolName") or ""
    if tool not in ("run_terminal_command", "Bash", "bash"):
        json.dump({"decision": "allow"}, sys.stdout)
        return 0
    cmd = str((ev.get("toolInput") or {}).get("command") or "")
    if not any(n in cmd for n in NEEDLES):
        json.dump({"decision": "allow"}, sys.stdout)
        return 0
    cwd = ev.get("workspaceRoot") or ev.get("cwd") or "."
    try:
        branch = subprocess.check_output(
            ["git", "branch", "--show-current"], cwd=cwd, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        json.dump({"decision": "allow"}, sys.stdout)
        return 0
    if branch == "main":
        json.dump({"decision": "allow"}, sys.stdout)
        return 0
    json.dump(
        {
            "decision": "deny",
            "reason": (
                f"Publish scripts are only allowed on main (this session is on {branch or 'an unknown branch'}). "
                "Open a PR, wait for USB box checks, merge, then publish when Lucas asks."
            ),
        },
        sys.stdout,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
