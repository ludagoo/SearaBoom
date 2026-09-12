#!/usr/bin/env python3
"""ESP32-S3 Secure Boot V2 (RSA-PSS) signature helpers.

Matches espsecure.py / IDF 5.3.2: 4 KB signature sector, first 1216-byte
RSA block, little-endian bignums, PSS salt length 32, SHA-256.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

SIG_MAGIC = 0xE7
SIG_VERSION_RSA = 0x02
SECTOR_SIZE = 4096
SIG_BLOCK_SIZE = 1216
CRC_LEN = 1196
RSA_N_BYTES = 384

PUBKEY_ENV = "SEARABOOM_OTA_PUBKEY"
PUBKEY_DEFAULT = Path.home() / ".config" / "searaboom" / "ota_signing_pubkey.pem"


class SignedFirmwareError(ValueError):
    """Image is missing, corrupt, or not signed with the expected key."""


def _int_to_le(value: int, length: int) -> bytes:
    return int(value).to_bytes(length, "big")[::-1]


def _le_to_int(data: bytes) -> int:
    return int.from_bytes(data, "little")


def _rsa_primitives(n: int, e: int, key_bits: int) -> tuple[int, int]:
    mdash = (-pow(n, -1, 1 << 32)) & 0xFFFFFFFF
    rinv = (1 << (key_bits * 2)) % n
    return mdash, rinv


def parse_rsa_block(image: bytes, block_index: int = 0) -> dict | None:
    if len(image) < SECTOR_SIZE or len(image) % SECTOR_SIZE != 0:
        return None
    offset = len(image) - SECTOR_SIZE + block_index * SIG_BLOCK_SIZE
    if offset + SIG_BLOCK_SIZE > len(image):
        return None
    blk = image[offset : offset + SIG_BLOCK_SIZE]
    magic, version = blk[0], blk[1]
    if magic != SIG_MAGIC or version != SIG_VERSION_RSA:
        return None
    crc_stored = struct.unpack_from("<I", blk, CRC_LEN)[0]
    crc_calc = zlib.crc32(blk[:CRC_LEN]) & 0xFFFFFFFF
    if crc_stored != crc_calc:
        return None
    digest = blk[4:36]
    n_le = blk[36:420]
    e = struct.unpack_from("<I", blk, 420)[0]
    sig_le = blk[812:1196]
    padded = image[: len(image) - SECTOR_SIZE]
    return {
        "digest": digest,
        "n": _le_to_int(n_le),
        "e": e,
        "signature": sig_le[::-1],
        "padded": padded,
        "block": blk,
    }


def image_is_signed(image: bytes) -> bool:
    parsed = parse_rsa_block(image)
    if not parsed:
        return False
    return hashlib.sha256(parsed["padded"]).digest() == parsed["digest"]


def load_pubkey_pem(pem: bytes | str):
    from cryptography.hazmat.primitives.serialization import load_pem_public_key, load_pem_private_key

    data = pem.encode() if isinstance(pem, str) else pem
    try:
        key = load_pem_public_key(data)
    except ValueError:
        key = load_pem_private_key(data, password=None).public_key()
    return key


def pubkey_modulus(pem: bytes | str) -> int:
    return load_pubkey_pem(pem).public_numbers().n


def resolve_pinned_pubkey() -> bytes | None:
    override = os.environ.get(PUBKEY_ENV, "").strip()
    path = Path(override) if override else PUBKEY_DEFAULT
    if path.is_file():
        return path.read_bytes()
    return None


def verify_rsa_pss(image: bytes, pinned_pem: bytes | str | None = None) -> dict:
    """Return info dict or raise SignedFirmwareError."""
    parsed = parse_rsa_block(image)
    if not parsed:
        raise SignedFirmwareError("missing ESP-IDF Secure Boot V2 RSA signature")
    if hashlib.sha256(parsed["padded"]).digest() != parsed["digest"]:
        raise SignedFirmwareError("signature digest does not match image")

    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils

    n, e = parsed["n"], parsed["e"]
    if pinned_pem is not None:
        want = pubkey_modulus(pinned_pem)
        if n != want:
            raise SignedFirmwareError("image signed with a different key")

    pub = rsa.RSAPublicNumbers(e, n).public_key()
    try:
        pub.verify(
            parsed["signature"],
            parsed["digest"],
            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
            utils.Prehashed(hashes.SHA256()),
        )
    except InvalidSignature as exc:
        raise SignedFirmwareError("RSA-PSS signature invalid") from exc
    return {
        "ok": True,
        "scheme": "rsa3072-sbv2",
        "n_hex": f"{n:0768x}",
        "size": len(image),
        "padded_len": len(parsed["padded"]),
    }


def require_signed_app(image: bytes, pinned_pem: bytes | str | None = None) -> dict:
    pinned = pinned_pem if pinned_pem is not None else resolve_pinned_pubkey()
    return verify_rsa_pss(image, pinned)


def sign_rsa3072(image: bytes, private_pem: bytes | str) -> bytes:
    """Append a Secure Boot V2 RSA signature sector (test / host helper)."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, utils
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    data = private_pem.encode() if isinstance(private_pem, str) else private_pem
    key = load_pem_private_key(data, password=None)
    padded = image
    if len(padded) % SECTOR_SIZE:
        padded += b"\xff" * (SECTOR_SIZE - (len(padded) % SECTOR_SIZE))
    digest = hashlib.sha256(padded).digest()
    signature = key.sign(
        digest,
        padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
        utils.Prehashed(hashes.SHA256()),
    )
    numbers = key.public_key().public_numbers()
    mdash, rinv = _rsa_primitives(numbers.n, numbers.e, key.key_size)
    block = struct.pack(
        "<BBxx32s384sI384sI384s",
        SIG_MAGIC,
        SIG_VERSION_RSA,
        digest,
        _int_to_le(numbers.n, RSA_N_BYTES),
        numbers.e,
        _int_to_le(rinv, RSA_N_BYTES),
        mdash,
        signature[::-1],
    )
    block += struct.pack("<I", zlib.crc32(block) & 0xFFFFFFFF)
    block += b"\x00" * 16
    if len(block) != SIG_BLOCK_SIZE:
        raise RuntimeError(f"bad signature block length {len(block)}")
    sector = block + b"\xff" * (SECTOR_SIZE - len(block))
    return padded + sector


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Check or sign SearaBoom app images")
    p.add_argument("--check", metavar="BIN", help="require a valid SB V2 signature")
    p.add_argument("--sign", metavar="BIN", help="append an SB V2 signature (writes --output)")
    p.add_argument("--key", metavar="PEM", help="private key for --sign, or pin for --check")
    p.add_argument("--output", metavar="BIN", help="signed output path")
    args = p.parse_args(argv)

    if args.check:
        data = Path(args.check).read_bytes()
        pem = Path(args.key).read_bytes() if args.key else None
        try:
            info = require_signed_app(data, pinned_pem=pem)
        except SignedFirmwareError as exc:
            print(f"unsigned or invalid: {exc}", file=sys.stderr)
            return 2
        print(f"signed {args.check} ({info['scheme']}, {info['size']} bytes)")
        return 0

    if args.sign:
        if not args.key or not args.output:
            p.error("--sign requires --key and --output")
        signed = sign_rsa3072(Path(args.sign).read_bytes(), Path(args.key).read_bytes())
        Path(args.output).write_bytes(signed)
        print(f"wrote {args.output} ({len(signed)} bytes)")
        return 0

    p.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
