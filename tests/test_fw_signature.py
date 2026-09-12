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


def test_ensure_signing_key_script() -> None:
    root = Path(__file__).resolve().parents[1]
    script = root / "scripts" / "ensure_signing_key.sh"
    with tempfile.TemporaryDirectory() as td:
        home = Path(td) / "home"
        cfg = home / ".config" / "searaboom"
        env = os.environ.copy()
        env["HOME"] = str(home)
        env["SEARABOOM_CONFIG_DIR"] = str(cfg)
        env["SEARABOOM_FIRMWARE_DIR"] = str(Path(td) / "firmware")
        env.pop("SEARABOOM_SIGNING_KEY", None)

        out = subprocess.check_output([str(script)], env=env, text=True).strip()
        key = Path(out)
        assert key.is_file() or key.is_symlink()
        assert (cfg / "secure_boot_signing_key.pem").is_file()
        assert (cfg / "ota_signing_pubkey.pem").is_file()
        first = (cfg / "secure_boot_signing_key.pem").read_bytes()
        subprocess.check_output([str(script)], env=env, text=True)
        assert (cfg / "secure_boot_signing_key.pem").read_bytes() == first


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
    test_upload_rejects_unsigned()
    print("ok")
