#!/usr/bin/env python3
"""SearaBoom OTA management server + multi-device log hub."""

from __future__ import annotations

import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import threading
import time
import sys
from collections import defaultdict, deque
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, Response, jsonify, request, send_from_directory, stream_with_context
import urllib.request

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
FW_DIR = ROOT / "firmware"
META_PATH = FW_DIR / "latest.json"
FACTORY_DIR = FW_DIR / "factory"
FACTORY_META = FACTORY_DIR / "factory.json"
FACTORY_NEXT_DIR = FW_DIR / "factory-next"
FACTORY_NEXT_META = FACTORY_NEXT_DIR / "factory.json"
BUILD_DIR = REPO / "firmware" / "build"
VERSION_FILE = REPO / "firmware" / "VERSION"
STATIC_DIR = ROOT / "static"
ADMIN_TOKEN = os.environ.get("SEARABOOM_ADMIN_TOKEN", "searaboom-dev")
HOST = os.environ.get("SEARABOOM_HOST", "0.0.0.0")
PORT = int(os.environ.get("SEARABOOM_PORT", "8080"))
PUBLIC_URL = os.environ.get("SEARABOOM_PUBLIC_URL", "https://searaboom.goossen.dev").rstrip("/")

LOG_LINES_PER_DEVICE = 2000

app = Flask(__name__, static_folder=str(STATIC_DIR))
lock = threading.Lock()
log_lock = threading.RLock()
flash_lock = threading.Lock()
device_logs: dict[str, deque[dict]] = defaultdict(lambda: deque(maxlen=LOG_LINES_PER_DEVICE))
device_seen: dict[str, float] = {}
subscribers: list[queue.Queue] = []

FW_DIR.mkdir(parents=True, exist_ok=True)
FACTORY_DIR.mkdir(parents=True, exist_ok=True)
FACTORY_NEXT_DIR.mkdir(parents=True, exist_ok=True)
STATIC_DIR.mkdir(parents=True, exist_ok=True)

# USB factory flash layout (must match firmware/build/flasher_args.json).
FACTORY_PLAN = [
    {"key": "bootloader", "offset": 0x0, "filename": "bootloader.bin",
     "build": BUILD_DIR / "bootloader" / "bootloader.bin"},
    {"key": "partitions", "offset": 0x8000, "filename": "partition-table.bin",
     "build": BUILD_DIR / "partition_table" / "partition-table.bin"},
    {"key": "otadata", "offset": 0xF000, "filename": "ota_data_initial.bin",
     "build": BUILD_DIR / "ota_data_initial.bin"},
    {"key": "app", "offset": 0x20000, "filename": "app.bin",
     "build": BUILD_DIR / "searaboom.bin"},
    {"key": "storage", "offset": 0x3A0000, "filename": "storage.bin",
     "build": BUILD_DIR / "storage.bin"},
]


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


def read_repo_version() -> str:
    if VERSION_FILE.exists():
        return VERSION_FILE.read_text().strip()
    return load_meta().get("version") or "0.0.0"


def read_factory_meta(path: Path) -> dict:
    if path.is_file():
        try:
            data = json.loads(path.read_text())
            if isinstance(data, dict):
                return data
        except json.JSONDecodeError:
            pass
    return {}


def factory_slot_ready(directory: Path) -> bool:
    return all((directory / item["filename"]).is_file() for item in FACTORY_PLAN)


def factory_file_path(item: dict) -> Path | None:
    stored = FACTORY_DIR / item["filename"]
    if stored.is_file():
        return stored
    return None


def write_factory_meta(directory: Path, version: str) -> dict:
    meta = {
        "version": version,
        "uploaded_at": datetime.now(timezone.utc).isoformat(),
        "files": [
            {
                "key": item["key"],
                "filename": item["filename"],
                "offset": item["offset"],
                "size": (directory / item["filename"]).stat().st_size,
            }
            for item in FACTORY_PLAN
        ],
    }
    (directory / "factory.json").write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def copy_factory_dir(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for item in FACTORY_PLAN:
        shutil.copyfile(src / item["filename"], dst / item["filename"])
    src_meta = src / "factory.json"
    if src_meta.is_file():
        shutil.copyfile(src_meta, dst / "factory.json")


def promote_factory_next() -> dict:
    if not factory_slot_ready(FACTORY_NEXT_DIR):
        return {"ok": True, "promoted": False, "reason": "no staged factory-next"}
    with lock:
        copy_factory_dir(FACTORY_NEXT_DIR, FACTORY_DIR)
    return {"ok": True, "promoted": True, "factory": factory_manifest()}


def factory_manifest() -> dict:
    files = []
    ready = True
    for item in FACTORY_PLAN:
        path = factory_file_path(item)
        if not path:
            ready = False
            files.append({
                "key": item["key"],
                "filename": item["filename"],
                "offset": item["offset"],
                "url": f"/api/factory/{item['filename']}",
                "size": 0,
                "ready": False,
            })
            continue
        files.append({
            "key": item["key"],
            "filename": item["filename"],
            "offset": item["offset"],
            "url": f"/api/factory/{item['filename']}",
            "size": path.stat().st_size,
            "ready": True,
        })
    meta = read_factory_meta(FACTORY_META)
    version = meta.get("version") or "unknown"
    next_meta = read_factory_meta(FACTORY_NEXT_META)
    ota = load_meta()
    port = serial_port()
    return {
        "ok": True,
        "ready": ready,
        "version": version,
        "ota_version": ota.get("version") or "0.0.0",
        "next_version": next_meta.get("version"),
        "chip": "esp32s3",
        "flash_mode": "dio",
        "flash_freq": "80m",
        "flash_size": "4MB",
        "files": files,
        "serial_port": port,
        "serial_present": Path(port).exists(),
    }


def serial_port() -> str:
    return os.environ.get("SEARABOOM_SERIAL_PORT", "/dev/ttyACM0")


def esptool_python() -> Path:
    override = os.environ.get("SEARABOOM_ESPTOOL_PYTHON")
    if override:
        return Path(override)
    env = Path.home() / ".espressif" / "python_env"
    cands = sorted(env.glob("idf*_py*/bin/python"), reverse=True)
    if cands:
        return cands[0]
    return Path(sys.executable)


def factory_flash_cmd() -> list[str]:
    py = esptool_python()
    port = serial_port()
    cmd = [
        str(py), "-u", "-m", "esptool",
        "--chip", "esp32s3",
        "-p", port,
        "-b", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "4MB",
    ]
    for item in FACTORY_PLAN:
        path = factory_file_path(item)
        if not path:
            raise FileNotFoundError(item["filename"])
        cmd.extend([hex(item["offset"]), str(path)])
    return cmd


def version_tuple(v: str) -> tuple[int, int, int]:
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", v or "0.0.0")
    if not m:
        return (0, 0, 0)
    return tuple(int(x) for x in m.groups())  # type: ignore[return-value]


def publish_log_event(event: dict) -> None:
    dead: list[queue.Queue] = []
    with log_lock:
        for q in subscribers:
            try:
                q.put_nowait(event)
            except queue.Full:
                dead.append(q)
        for q in dead:
            if q in subscribers:
                subscribers.remove(q)


@app.get("/")
def index():
    return send_from_directory(STATIC_DIR, "index.html")


@app.get("/admin")
def admin():
    return send_from_directory(STATIC_DIR, "admin.html")


@app.get("/api/factory")
def factory_status():
    return jsonify(factory_manifest())


@app.get("/api/factory/web-tools.json")
def factory_web_tools():
    m = factory_manifest()
    parts = [
        {"path": item["filename"], "offset": item["offset"]}
        for item in m["files"]
        if item.get("ready")
    ]
    return jsonify({
        "name": "SearaBoom",
        "version": m["version"],
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {"chipFamily": "ESP32-S3", "serialType": "cdc", "parts": parts},
            {"chipFamily": "ESP32-S3", "serialType": "uart", "parts": parts},
        ],
    })


@app.get("/api/factory/<path:filename>")
def factory_download(filename: str):
    safe = Path(filename).name
    item = next((x for x in FACTORY_PLAN if x["filename"] == safe), None)
    if not item:
        return jsonify({"error": "unknown factory file"}), 404
    path = factory_file_path(item)
    if not path:
        return jsonify({"error": "factory image missing"}), 404
    return send_from_directory(path.parent, path.name, mimetype="application/octet-stream")


@app.post("/api/factory/flash")
def factory_flash():
    token = request.headers.get("X-Admin-Token") or request.form.get("token")
    if token != ADMIN_TOKEN:
        return jsonify({"error": "unauthorized"}), 401
    port = serial_port()
    if not Path(port).exists():
        return jsonify({"error": f"no USB box on {port}"}), 400
    try:
        cmd = factory_flash_cmd()
    except FileNotFoundError as e:
        return jsonify({"error": f"missing factory file {e}"}), 400
    if not flash_lock.acquire(blocking=False):
        return jsonify({"error": "flash already running"}), 409

    def gen():
        try:
            yield json.dumps({"line": " ".join(cmd)}) + "\n"
            try:
                proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    env={**os.environ, "PYTHONUNBUFFERED": "1"},
                )
            except Exception as e:
                yield json.dumps({"line": str(e), "done": True, "ok": False}) + "\n"
                return
            assert proc.stdout is not None
            for line in proc.stdout:
                yield json.dumps({"line": line.rstrip("\n")}) + "\n"
            rc = proc.wait()
            yield json.dumps({"done": True, "ok": rc == 0, "code": rc}) + "\n"
        finally:
            flash_lock.release()

    return Response(
        stream_with_context(gen()),
        mimetype="application/x-ndjson",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.post("/api/factory/upload")
def factory_upload():
    token = request.headers.get("X-Admin-Token") or request.form.get("token")
    if token != ADMIN_TOKEN:
        return jsonify({"error": "unauthorized"}), 401
    slot = (request.form.get("slot") or "live").strip().lower()
    if slot not in ("live", "next"):
        return jsonify({"error": "slot must be live or next"}), 400
    version = (request.form.get("version") or read_repo_version()).strip()
    missing = [item["key"] for item in FACTORY_PLAN if item["key"] not in request.files]
    if missing:
        return jsonify({"error": "missing files", "missing": missing}), 400
    dest_dir = FACTORY_DIR if slot == "live" else FACTORY_NEXT_DIR
    dest_dir.mkdir(parents=True, exist_ok=True)
    with lock:
        for item in FACTORY_PLAN:
            request.files[item["key"]].save(dest_dir / item["filename"])
        write_factory_meta(dest_dir, version)
    return jsonify({
        "ok": True,
        "slot": slot,
        "factory": factory_manifest(),
    })


@app.post("/api/factory/promote")
def factory_promote():
    token = request.headers.get("X-Admin-Token") or request.form.get("token")
    if token != ADMIN_TOKEN:
        return jsonify({"error": "unauthorized"}), 401
    return jsonify(promote_factory_next())


@app.get("/api/status")
def status():
    meta = load_meta()
    factory = factory_manifest()
    with log_lock:
        devices = [
            {"device_id": did, "last_seen": device_seen.get(did), "lines": len(device_logs[did])}
            for did in sorted(device_logs.keys())
        ]
    return jsonify({
        "ok": True,
        "firmware": meta,
        "factory": {
            "version": factory.get("version"),
            "ready": factory.get("ready"),
            "next_version": factory.get("next_version"),
        },
        "devices": devices,
    })


@app.get("/api/firmware/check")
def firmware_check():
    current = request.args.get("current", "0.0.0")
    meta = load_meta()
    latest = meta.get("version") or "0.0.0"
    cur_t = version_tuple(current)
    lat_t = version_tuple(latest)
    newer_full = lat_t > cur_t and bool(meta.get("filename"))
    newer_stable = (lat_t[0], lat_t[1]) > (cur_t[0], cur_t[1]) and bool(meta.get("filename"))
    url = None
    if newer_full:
        url = f"{PUBLIC_URL}/api/firmware/download/{meta['filename']}"
    return jsonify(
        {
            "update": newer_full,
            "update_stable": newer_stable,
            "update_dev": newer_full,
            "version": latest,
            "current": current,
            "url": url,
            "sha256": meta.get("sha256"),
            "size": meta.get("size"),
        }
    )


@app.get("/api/firmware/download/<path:filename>")
def firmware_download(filename: str):
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
        latest_bin = FW_DIR / "latest.bin"
        shutil.copyfile(dest, latest_bin)
    return jsonify({"ok": True, "firmware": meta})


@app.post("/api/logs")
def logs_ingest():
    data = request.get_json(silent=True) or {}
    device_id = (data.get("device_id") or "unknown").strip().lower()
    text = data.get("text") or ""
    if not text:
        return jsonify({"ok": True, "accepted": 0})
    now = time.time()
    entries = []
    for line in text.splitlines():
        if not line:
            continue
        entry = {
            "device_id": device_id,
            "line": line,
            "ts": now,
            "server_ts": datetime.now(timezone.utc).isoformat(),
        }
        entries.append(entry)
    with log_lock:
        device_seen[device_id] = now
        for entry in entries:
            device_logs[device_id].append(entry)
            publish_log_event(entry)
    return jsonify({"ok": True, "accepted": len(entries)})


@app.get("/api/logs")
def logs_get():
    device = (request.args.get("device") or "").strip().lower()
    limit = min(int(request.args.get("limit", "200")), 1000)
    with log_lock:
        if device:
            lines = list(device_logs.get(device, []))[-limit:]
        else:
            merged: list[dict] = []
            for dq in device_logs.values():
                merged.extend(dq)
            merged.sort(key=lambda e: e.get("ts", 0))
            lines = merged[-limit:]
    return jsonify({"ok": True, "lines": lines})


@app.get("/api/logs/stream")
def logs_stream():
    device = (request.args.get("device") or "").strip().lower()
    q: queue.Queue = queue.Queue(maxsize=500)
    with log_lock:
        subscribers.append(q)

    @stream_with_context
    def gen():
        try:
            yield ": connected\n\n"
            while True:
                try:
                    event = q.get(timeout=15)
                except queue.Empty:
                    yield ": ping\n\n"
                    continue
                if device and event.get("device_id") != device:
                    continue
                payload = json.dumps(event, separators=(",", ":"))
                yield f"data: {payload}\n\n"
        finally:
            with log_lock:
                if q in subscribers:
                    subscribers.remove(q)

    return Response(gen(), mimetype="text/event-stream", headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


STREAMS = {
    "102": "https://8396.brasilstream.com.br/stream",
    "104": "https://8404.brasilstream.com.br/stream",
}


@app.get("/stream/<station>")
def stream_proxy(station: str):
    """Proxy live AAC with a Content-Type ADF will not mis-classify."""
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


@app.errorhandler(404)
def not_found(_e):
    if request.path.startswith("/api/"):
        return jsonify({"error": "not found", "path": request.path}), 404
    return (
        "<!doctype html><title>404 Not Found</title><h1>Not Found</h1>",
        404,
        {"Content-Type": "text/html; charset=utf-8"},
    )


if __name__ == "__main__":
    print(f"SearaBoom OTA server on http://{HOST}:{PORT}")
    print(f"Admin token: {ADMIN_TOKEN}")
    app.run(host=HOST, port=PORT, threaded=True)
