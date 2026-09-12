#!/usr/bin/env python3
"""Fail-closed signing-key backup gate (no --force, no env bypass)."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from signing_key_backup import (  # noqa: E402
    EXIT_BLOCKED,
    SigningKeyBackupError,
    marker_is_present,
    require_backup,
)


def _run_hook(ev: dict, env: dict) -> dict:
    proc = subprocess.run(
        [sys.executable, str(Path(__file__).resolve().parents[1] / "scripts" / "hooks" / "deny_publish.py")],
        input=json.dumps(ev),
        text=True,
        capture_output=True,
        env=env,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    return json.loads(proc.stdout)


def test_require_fails_without_marker() -> None:
    with tempfile.TemporaryDirectory() as td:
        env_home = Path(td)
        old = os.environ.get("HOME")
        os.environ["HOME"] = str(env_home)
        try:
            assert marker_is_present() is False
            try:
                require_backup()
            except SigningKeyBackupError as exc:
                assert "signing_key_backed_up" in str(exc)
            else:
                raise AssertionError("missing marker must fail")
            rc = subprocess.run(
                [sys.executable, str(Path(__file__).resolve().parents[1] / "scripts" / "signing_key_backup.py"), "--require"],
                env={**os.environ},
                capture_output=True,
                text=True,
            )
            assert rc.returncode == EXIT_BLOCKED
            assert "agents must never create" in rc.stderr.lower() or "Lucas" in rc.stderr
        finally:
            if old is None:
                os.environ.pop("HOME", None)
            else:
                os.environ["HOME"] = old


def test_require_ok_with_regular_file() -> None:
    with tempfile.TemporaryDirectory() as td:
        home = Path(td)
        marker = home / ".config" / "searaboom" / "signing_key_backed_up"
        marker.parent.mkdir(parents=True)
        marker.write_text("stored\n")
        old = os.environ.get("HOME")
        os.environ["HOME"] = str(home)
        try:
            assert marker_is_present()
            require_backup()
        finally:
            if old is None:
                os.environ.pop("HOME", None)
            else:
                os.environ["HOME"] = old


def test_symlink_marker_rejected() -> None:
    with tempfile.TemporaryDirectory() as td:
        home = Path(td)
        cfg = home / ".config" / "searaboom"
        cfg.mkdir(parents=True)
        target = cfg / "other"
        target.write_text("x")
        (cfg / "signing_key_backed_up").symlink_to(target)
        old = os.environ.get("HOME")
        os.environ["HOME"] = str(home)
        try:
            assert marker_is_present() is False
        finally:
            if old is None:
                os.environ.pop("HOME", None)
            else:
                os.environ["HOME"] = old


def test_publish_script_blocked_without_marker() -> None:
    root = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    with tempfile.TemporaryDirectory() as td:
        env["HOME"] = td
        rc = subprocess.run(
            [str(root / "scripts" / "publish_firmware.sh"), "9.9.9"],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
        )
        assert rc.returncode == EXIT_BLOCKED
        assert "signing_key_backed_up" in rc.stderr


def test_hook_denies_creating_marker() -> None:
    env = os.environ.copy()
    with tempfile.TemporaryDirectory() as td:
        env["HOME"] = td
        out = _run_hook(
            {
                "toolName": "Bash",
                "toolInput": {"command": "touch ~/.config/searaboom/signing_key_backed_up"},
            },
            env,
        )
        assert out["decision"] == "deny"
        assert "never create" in out["reason"]
        sneaky = (
            "python3 -c \"from pathlib import Path; "
            "Path.home().joinpath('.config/searaboom/signing_key_backed_up').write_text('x')\""
        )
        out = _run_hook({"toolName": "Bash", "toolInput": {"command": sneaky}}, env)
        assert out["decision"] == "deny"
        out = _run_hook(
            {
                "toolName": "Write",
                "toolInput": {"path": str(Path(td) / ".config" / "searaboom" / "signing_key_backed_up")},
            },
            env,
        )
        assert out["decision"] == "deny"


def test_hook_denies_flash_without_marker() -> None:
    env = os.environ.copy()
    with tempfile.TemporaryDirectory() as td:
        env["HOME"] = td
        out = _run_hook(
            {
                "toolName": "Bash",
                "toolInput": {"command": "idf.py -p /dev/ttyACM0 flash"},
            },
            env,
        )
        assert out["decision"] == "deny"
        assert "signing_key_backed_up" in out["reason"]
        out = _run_hook(
            {
                "toolName": "Bash",
                "toolInput": {"command": "./scripts/hw_flash.sh --box zero-fast"},
            },
            env,
        )
        assert out["decision"] == "deny"
        out = _run_hook(
            {
                "toolName": "Bash",
                "toolInput": {"command": "idf.py build"},
            },
            env,
        )
        assert out["decision"] == "allow"


def test_hook_still_denies_publish_off_main_with_marker() -> None:
    env = os.environ.copy()
    with tempfile.TemporaryDirectory() as td:
        home = Path(td)
        marker = home / ".config" / "searaboom" / "signing_key_backed_up"
        marker.parent.mkdir(parents=True)
        marker.write_text("ok\n")
        env["HOME"] = str(home)
        out = _run_hook(
            {
                "toolName": "Bash",
                "cwd": str(Path(__file__).resolve().parents[1]),
                "toolInput": {"command": "./scripts/publish_firmware.sh 1.2.3"},
            },
            env,
        )
        assert out["decision"] == "deny"
        assert "main" in out["reason"]


if __name__ == "__main__":
    test_require_fails_without_marker()
    test_require_ok_with_regular_file()
    test_symlink_marker_rejected()
    test_publish_script_blocked_without_marker()
    test_hook_denies_creating_marker()
    test_hook_denies_flash_without_marker()
    test_hook_still_denies_publish_off_main_with_marker()
    print("ok")
