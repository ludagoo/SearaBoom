#!/usr/bin/env python3
"""SearaBoom OTA management server."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import threading
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, Response, jsonify, redirect, request, send_from_directory, stream_with_context, url_for
import urllib.request

ROOT = Path(__file__).resolve().parent
FW_DIR = ROOT / "firmware"
META_PATH = FW_DIR / "latest.json"
STATIC_DIR = ROOT / "static"
ADMIN_TOKEN = os.environ.get("SEARABOOM_ADMIN_TOKEN", "searaboom-dev")
HOST = os.environ.get("SEARABOOM_HOST", "0.0.0.0")
PORT = int(os.environ.get("SEARABOOM_PORT", "8080"))

app = Flask(__name__, static_folder=str(STATIC_DIR))
lock = threading.Lock()

FW_DIR.mkdir(parents=True, exist_ok=True)
STATIC_DIR.mkdir(parents=True, exist_ok=True)


def load_meta() -> dict:
    if META_PATH.exists():
        return json.loads(META_PATH.read_text())
    return {
        "version": "0.0.0",
        "filename": None,
        "sha256": None,
        "uploaded_at": None,
        "size": 0,
    }


def save_meta(meta: dict) -> None:
    META_PATH.write_text(json.dumps(meta, indent=2) + "\n")


def version_tuple(v: str) -> tuple[int, int, int]:
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", v or "0.0.0")
    if not m:
        return (0, 0, 0)
    return tuple(int(x) for x in m.groups())  # type: ignore[return-value]


@app.get("/")
def index():
    return send_from_directory(STATIC_DIR, "index.html")


@app.get("/api/status")
def status():
    meta = load_meta()
    return jsonify({"ok": True, "firmware": meta})


@app.get("/api/firmware/check")
def firmware_check():
    current = request.args.get("current", "0.0.0")
    meta = load_meta()
    latest = meta.get("version") or "0.0.0"
    update = version_tuple(latest) > version_tuple(current) and bool(meta.get("filename"))
    url = None
    if update:
        url = url_for("firmware_download", filename=meta["filename"], _external=True)
        # Prefer public host if behind tunnel
        public = os.environ.get("SEARABOOM_PUBLIC_URL", "https://searaboom.goossen.dev").rstrip("/")
        url = f"{public}/api/firmware/download/{meta['filename']}"
    return jsonify(
        {
            "update": update,
            "version": latest,
            "current": current,
            "url": url,
            "sha256": meta.get("sha256"),
            "size": meta.get("size"),
        }
    )


@app.get("/api/firmware/download/<path:filename>")
def firmware_download(filename: str):
    # Prevent path traversal
    safe = Path(filename).name
    return send_from_directory(FW_DIR, safe, as_attachment=True, mimetype="application/octet-stream")


@app.post("/api/firmware/upload")
def firmware_upload():
    token = request.headers.get("X-Admin-Token") or request.form.get("token")
    if token != ADMIN_TOKEN:
        return jsonify({"error": "unauthorized"}), 401
    version = (request.form.get("version") or "").strip()
    if not re.match(r"^\d+\.\d+\.\d+$", version):
        return jsonify({"error": "version must be X.Y.Z"}), 400
    f = request.files.get("firmware")
    if not f:
        return jsonify({"error": "missing firmware file"}), 400

    filename = f"searaboom-v{version}.bin"
    dest = FW_DIR / filename
    with lock:
        f.save(dest)
        digest = hashlib.sha256(dest.read_bytes()).hexdigest()
        meta = {
            "version": version,
            "filename": filename,
            "sha256": digest,
            "size": dest.stat().st_size,
            "uploaded_at": datetime.now(timezone.utc).isoformat(),
        }
        save_meta(meta)
        # Keep a stable latest.bin symlink/copy for convenience
        latest_bin = FW_DIR / "latest.bin"
        shutil.copyfile(dest, latest_bin)
    return jsonify({"ok": True, "firmware": meta})


STREAMS = {
    "102": "https://8396.brasilstream.com.br/stream",
    "104": "https://8404.brasilstream.com.br/stream",
}


@app.get("/stream/<station>")
def stream_proxy(station: str):
    """Proxy live AAC with a Content-Type ADF will not mis-classify.

    ESP-ADF maps:
      - audio/aac → RAW AAC (needs ASC; fails on ADTS live streams)
      - application/octet-stream → MP3 (wrong codec for this pipe)
    Use a neutral type so the AAC decoder ADTS-syncs on the payload.
    """
    upstream = STREAMS.get(station)
    if not upstream:
        return jsonify({"error": "unknown station"}), 404

    def generate():
        req = urllib.request.Request(
            upstream,
            headers={"Icy-MetaData": "0", "User-Agent": "SearaBoom-Proxy/1.0"},
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            while True:
                chunk = resp.read(4096)
                if not chunk:
                    break
                yield chunk

    return Response(
        stream_with_context(generate()),
        headers={
            "Content-Type": "audio/adts",
            "Cache-Control": "no-cache",
            "Connection": "close",
        },
    )


@app.get("/healthz")
def healthz():
    return jsonify({"ok": True})


if __name__ == "__main__":
    print(f"SearaBoom OTA server on http://{HOST}:{PORT}")
    print(f"Admin token: {ADMIN_TOKEN}")
    app.run(host=HOST, port=PORT, threaded=True)