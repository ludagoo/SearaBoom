#!/usr/bin/env python3
"""Signed-app checks for Secure Boot V2 RSA-PSS (no IDF required)."""
from __future__ import annotations

import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from fw_signature import (  # noqa: E402
    SignedFirmwareError,
    image_is_signed,
    require_signed_app,
    sign_rsa3072,
)

from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization


def _rsa_pem() -> bytes:
    key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    return key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    )


def test_unsigned_rejected() -> None:
    try:
        require_signed_app(b"not a firmware" * 200)
    except SignedFirmwareError:
        return
    raise AssertionError("expected unsigned image to fail")


def test_sign_and_verify() -> None:
    pem = _rsa_pem()
    raw = os.urandom(5000)
    signed = sign_rsa3072(raw, pem)
    assert image_is_signed(signed)
    from fw_signature import verify_rsa_pss
    verify_rsa_pss(signed)
    info = require_signed_app(signed, pinned_pem=pem)
    assert info["ok"]
    tampered = signed[:100] + bytes((signed[100] ^ 1,)) + signed[101:]
    try:
        require_signed_app(tampered, pinned_pem=pem)
    except SignedFirmwareError:
        pass
    else:
        raise AssertionError("tampered image must fail")
    other = _rsa_pem()
    try:
        require_signed_app(signed, pinned_pem=other)
    except SignedFirmwareError as exc:
        assert "different key" in str(exc)
    else:
        raise AssertionError("wrong key must fail")


def _ensure_env(td: str) -> tuple[dict, Path, Path]:
    home = Path(td) / "home"
    cfg = home / ".config" / "searaboom"
    fw = Path(td) / "firmware"
    env = os.environ.copy()
    env["HOME"] = str(home)
    env["SEARABOOM_CONFIG_DIR"] = str(cfg)
    env["SEARABOOM_FIRMWARE_DIR"] = str(fw)
    env.pop("SEARABOOM_SIGNING_KEY", None)
    return env, cfg, fw


def test_ensure_signing_key_script() -> None:
    root = Path(__file__).resolve().parents[1]
    script = root / "scripts" / "ensure_signing_key.sh"
    with tempfile.TemporaryDirectory() as td:
        env, cfg, _fw = _ensure_env(td)
        missing = subprocess.run([str(script)], env=env, capture_output=True, text=True)
        assert missing.returncode == 1
        assert "--generate" in missing.stderr

        out = subprocess.check_output([str(script), "--generate"], env=env, text=True).strip()
        key = Path(out)
        assert key.is_file() or key.is_symlink()
        assert (cfg / "secure_boot_signing_key.pem").is_file()
        assert (cfg / "ota_signing_pubkey.pem").is_file()
        first = (cfg / "secure_boot_signing_key.pem").read_bytes()
        first_pub = (cfg / "ota_signing_pubkey.pem").read_bytes()
        subprocess.check_output([str(script)], env=env, text=True)
        subprocess.check_output([str(script), "--generate"], env=env, text=True)
        assert (cfg / "secure_boot_signing_key.pem").read_bytes() == first
        assert (cfg / "ota_signing_pubkey.pem").read_bytes() == first_pub
        assert not (cfg / "signing_key_backed_up").exists()
        assert not (Path(td) / "home" / ".config" / "searaboom" / "signing_key_backed_up").exists()


def test_ensure_signing_key_does_not_clobber_config_key() -> None:
    root = Path(__file__).resolve().parents[1]
    script = root / "scripts" / "ensure_signing_key.sh"
    with tempfile.TemporaryDirectory() as td:
        env, cfg, fw = _ensure_env(td)
        subprocess.check_output([str(script), "--generate"], env=env, text=True)
        original = (cfg / "secure_boot_signing_key.pem").read_bytes()
        original_pub = (cfg / "ota_signing_pubkey.pem").read_bytes()

        other = Path(td) / "other.pem"
        subprocess.check_call(
            ["openssl", "genrsa", "-out", str(other), "3072"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        env["SEARABOOM_SIGNING_KEY"] = str(other)
        rc = subprocess.run([str(script)], env=env, capture_output=True, text=True)
        assert rc.returncode == 1, rc.stderr
        assert "differs" in rc.stderr
        assert (cfg / "secure_boot_signing_key.pem").read_bytes() == original
        assert (cfg / "ota_signing_pubkey.pem").read_bytes() == original_pub

        env.pop("SEARABOOM_SIGNING_KEY", None)
        stray = fw / "secure_boot_signing_key.pem"
        if stray.exists() or stray.is_symlink():
            stray.unlink()
        other.replace(stray)
        rc = subprocess.run([str(script)], env=env, capture_output=True, text=True)
        assert rc.returncode == 1, rc.stderr
        assert (cfg / "secure_boot_signing_key.pem").read_bytes() == original
        assert (cfg / "ota_signing_pubkey.pem").read_bytes() == original_pub


def test_ensure_signing_key_stray_env_does_not_write_config() -> None:
    root = Path(__file__).resolve().parents[1]
    script = root / "scripts" / "ensure_signing_key.sh"
    with tempfile.TemporaryDirectory() as td:
        env, cfg, fw = _ensure_env(td)
        other = Path(td) / "other.pem"
        subprocess.check_call(
            ["openssl", "genrsa", "-out", str(other), "3072"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        env["SEARABOOM_SIGNING_KEY"] = str(other)
        out = subprocess.check_output([str(script)], env=env, text=True).strip()
        assert Path(out) == fw / "secure_boot_signing_key.pem"
        assert not (cfg / "secure_boot_signing_key.pem").exists()
        assert not (cfg / "ota_signing_pubkey.pem").exists()
        assert not (cfg / "signing_key_backed_up").exists()


def test_ensure_signing_key_firmware_pem_does_not_promote() -> None:
    root = Path(__file__).resolve().parents[1]
    script = root / "scripts" / "ensure_signing_key.sh"
    with tempfile.TemporaryDirectory() as td:
        env, cfg, fw = _ensure_env(td)
        fw.mkdir(parents=True)
        stray = fw / "secure_boot_signing_key.pem"
        subprocess.check_call(
            ["openssl", "genrsa", "-out", str(stray), "3072"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        out = subprocess.check_output([str(script)], env=env, text=True).strip()
        assert Path(out) == stray
        assert not (cfg / "secure_boot_signing_key.pem").exists()
        assert not (cfg / "ota_signing_pubkey.pem").exists()


def test_hw_flash_and_cmake_link_without_generate() -> None:
    root = Path(__file__).resolve().parents[1]
    flash = (root / "scripts" / "hw_flash.sh").read_text()
    cmake = (root / "firmware" / "CMakeLists.txt").read_text()
    invoked = False
    for line in flash.splitlines():
        if "ensure_signing_key.sh" in line and not line.lstrip().startswith("#"):
            assert "--generate" not in line
            invoked = True
    assert invoked
    invoke = cmake.split("execute_process", 1)[1].split("if(", 1)[0]
    assert "ensure_signing_key.sh" in invoke
    assert "--generate" not in invoke


def test_upload_rejects_unsigned() -> None:
    from app import app  # noqa: WPS433

    client = app.test_client()
    resp = client.post(
        "/api/firmware/upload",
        data={
            "version": "9.9.9",
            "token": "searaboom-dev",
            "firmware": (io.BytesIO(os.urandom(256)), "unsigned.bin"),
        },
    )
    assert resp.status_code == 400
    assert b"signed" in resp.data.lower()


if __name__ == "__main__":
    test_unsigned_rejected()
    test_sign_and_verify()
    test_ensure_signing_key_script()
    test_ensure_signing_key_does_not_clobber_config_key()
    test_ensure_signing_key_stray_env_does_not_write_config()
    test_ensure_signing_key_firmware_pem_does_not_promote()
    test_hw_flash_and_cmake_link_without_generate()
    test_upload_rejects_unsigned()
    print("ok")
