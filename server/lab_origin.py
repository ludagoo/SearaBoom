"""Cursor Origin app JWT, installation tokens, webhook verify, check runs."""
from __future__ import annotations

import base64
import hashlib
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ORIGIN_API = "https://api.cursor.com/v1/origin"
JWKS_URL = "https://api.cursor.com/v1/origin/keys"
OWNER = os.environ.get("ORIGIN_OWNER", "goossen")
REPO = os.environ.get("ORIGIN_REPO", "searaboom")
APP_ID = os.environ.get("ORIGIN_APP_ID", "").strip()
INSTALLATION_ID = os.environ.get("ORIGIN_INSTALLATION_ID", "").strip()
KEY_PATH = Path(os.environ.get(
    "ORIGIN_APP_PRIVATE_KEY",
    str(Path.home() / ".config/searaboom/origin-app/private.pem"),
))

SUITE_KEY = "searaboom-lab"
SUITE_NAME = "SearaBoom lab"
CHECK_META = {
    "hw-test-zero": "USB box (s3-zero)",
    "hw-test-supermini": "USB box (s3-supermini)",
    "hw-soak-zero": "USB soak (s3-zero)",
    "hw-soak-supermini": "USB soak (s3-supermini)",
    "hw-listen": "USB listen",
    "hw-hands": "USB hands",
}

_jwks_cache: tuple[float, list[dict]] = (0.0, [])
_install_token: tuple[float, str] = (0.0, "")


def configured() -> bool:
    return bool(APP_ID and INSTALLATION_ID and KEY_PATH.is_file())


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _load_private_key():
    from cryptography.hazmat.primitives import serialization
    pem = KEY_PATH.read_bytes()
    return serialization.load_pem_private_key(pem, password=None)


def app_jwt() -> str:
    if not APP_ID:
        raise RuntimeError("ORIGIN_APP_ID is not set")
    now = int(time.time())
    header = {"alg": "EdDSA", "kid": APP_ID, "typ": "JWT"}
    claims = {"iss": APP_ID, "aud": "origin-apps", "iat": now, "exp": now + 240}
    signing = (
        _b64url(json.dumps(header, separators=(",", ":")).encode())
        + "."
        + _b64url(json.dumps(claims, separators=(",", ":")).encode())
    )
    sig = _load_private_key().sign(signing.encode())
    return f"{signing}.{_b64url(sig)}"


def _http_json(method: str, url: str, token: str, body: dict | None = None) -> dict:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/json")
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as e:
        err = e.read().decode("utf-8", "replace")
        raise RuntimeError(f"Origin {method} {url} -> {e.code}: {err}") from e
    if not raw:
        return {}
    return json.loads(raw)


def installation_token() -> str:
    global _install_token
    exp, tok = _install_token
    if tok and time.time() < exp - 30:
        return tok
    if not INSTALLATION_ID:
        raise RuntimeError("ORIGIN_INSTALLATION_ID is not set")
    jwt = app_jwt()
    url = f"{ORIGIN_API}/app/installations/{INSTALLATION_ID}/access_tokens"
    data = _http_json("POST", url, jwt, {})
    token = data.get("token") or data.get("accessToken") or ""
    if not token:
        raise RuntimeError(f"no installation token in {data!r}")
    # Origin tokens last <= 15 min
    _install_token = (time.time() + 8 * 60, token)
    return token


def origin_get(path: str) -> dict:
    url = path if path.startswith("http") else f"{ORIGIN_API}{path}"
    return _http_json("GET", url, installation_token())


def origin_post(path: str, body: dict) -> dict:
    url = path if path.startswith("http") else f"{ORIGIN_API}{path}"
    return _http_json("POST", url, installation_token(), body)


def _jwks() -> list[dict]:
    global _jwks_cache
    ts, keys = _jwks_cache
    if keys and time.time() - ts < 600:
        return keys
    req = urllib.request.Request(JWKS_URL)
    with urllib.request.urlopen(req, timeout=15) as resp:
        payload = json.loads(resp.read())
    keys = payload.get("keys") or []
    _jwks_cache = (time.time(), keys)
    return keys


def verify_webhook(headers: dict, body: bytes) -> bool:
    hid = headers.get("Webhook-Id") or headers.get("webhook-id") or ""
    ts_raw = headers.get("Webhook-Timestamp") or headers.get("webhook-timestamp") or ""
    sig_hdr = headers.get("Webhook-Signature") or headers.get("webhook-signature") or ""
    try:
        ts = int(ts_raw)
    except ValueError:
        return False
    if not hid or not sig_hdr:
        return False
    if abs(int(time.time()) - ts) > 300:
        return False
    token = None
    for part in sig_hdr.split():
        if part.startswith("v1ed,"):
            token = part[5:]
            break
    if not token:
        return False
    digest = hashlib.sha256(f"{hid}.{ts}.".encode() + body).hexdigest()
    sig = base64.b64decode(token)
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    for jwk in _jwks():
        x = jwk.get("x") or ""
        pad = "=" * (-len(x) % 4)
        try:
            raw = base64.urlsafe_b64decode(x + pad)
            pub = Ed25519PublicKey.from_public_bytes(raw)
            pub.verify(sig, digest.encode())
            return True
        except (InvalidSignature, ValueError, Exception):
            continue
    return False


def rfc3339(dt: datetime | None = None) -> str:
    """UTC RFC 3339 for google.protobuf.Timestamp JSON (Z, 6 fractional digits).

    Python's datetime.isoformat() emits ``+00:00``, which Origin rejects:
    ``cannot decode google.protobuf.Timestamp from JSON: invalid RFC 3339 string``.
    protojson wants a Z-normalized string with 0, 3, 6, or 9 fractional digits.
    """
    if dt is None:
        dt = datetime.now(timezone.utc)
    elif dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    else:
        dt = dt.astimezone(timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond:06d}Z"


def iso_now() -> str:
    return rfc3339()


def post_check(
    *,
    head_sha: str,
    check_key: str,
    status: str,
    conclusion: str | None = None,
    title: str = "",
    summary: str = "",
    text: str = "",
    external_id: str,
    details_url: str | None = None,
    deadline_at: str | None = None,
) -> dict:
    name = CHECK_META.get(check_key, check_key)
    run: dict = {
        "key": check_key,
        "name": name,
        "status": status,
        "externalUpdatedAt": iso_now(),
        "externalId": external_id,
        "output": {
            "title": (title or name)[:255],
            "summary": summary[:65000],
            "text": text[:65000],
        },
    }
    if conclusion:
        run["conclusion"] = conclusion
    if details_url:
        run["detailsUrl"] = details_url
    if deadline_at:
        run["deadlineAt"] = deadline_at
    if status == "in_progress":
        run["startedAt"] = iso_now()
    if status == "completed":
        run["completedAt"] = iso_now()
    body = {
        "headSha": head_sha,
        "checkSuite": {
            "key": SUITE_KEY,
            "name": SUITE_NAME,
            "externalId": f"{SUITE_KEY}-{head_sha[:12]}",
        },
        "checkRun": run,
    }
    return origin_post(f"/repos/{OWNER}/{REPO}/check-runs", body)


def get_pull(number: str | int) -> dict:
    return origin_get(f"/repos/{OWNER}/{REPO}/pulls/{number}")


def list_pull_files(number: str | int) -> list[str]:
    data = origin_get(f"/repos/{OWNER}/{REPO}/pulls/{number}/files")
    files = data.get("files") or data.get("changedFiles") or []
    paths = []
    for item in files:
        if isinstance(item, str):
            paths.append(item)
            continue
        path = item.get("path") or item.get("filename") or item.get("name")
        if path:
            paths.append(path)
    # pagination
    token = data.get("nextPageToken") or ""
    while token:
        more = origin_get(
            f"/repos/{OWNER}/{REPO}/pulls/{number}/files?pageToken={urllib.parse.quote(token)}"
        )
        for item in more.get("files") or more.get("changedFiles") or []:
            path = item if isinstance(item, str) else (
                item.get("path") or item.get("filename") or item.get("name")
            )
            if path:
                paths.append(path)
        token = more.get("nextPageToken") or ""
    return paths
