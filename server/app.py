#!/usr/bin/env python3
"""SearaBoom OTA management server + multi-device log hub."""

from __future__ import annotations

import atexit
import hashlib
import ipaddress
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from collections import defaultdict, deque
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from flask import Flask, Response, jsonify, request, send_from_directory, stream_with_context

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
LOGS_DIR = ROOT / "logs"
DEVICES_PATH = LOGS_DIR / "devices.json"
GEOCODE_PATH = LOGS_DIR / "geocode.json"
GEO_TTL_S = 24 * 3600
ONLINE_WINDOW_S = 120
PLAYING_WINDOW_S = 180
DEVICES_SAVE_DEBOUNCE_S = 1.0
GEO_LOOKUP_TIMEOUT_S = 3
NOMINATIM_MIN_INTERVAL_S = 1.0
OUVINTES_ID_SALT = os.environ.get("SEARABOOM_OUVINTES_SALT", "searaboom-ouvintes-v1")
ESP_LOG_RE = re.compile(r"^([IWE DV]) \((\d+)\) ([^:]+): ?(.*)$")
USER_FLAG_RE = re.compile(r"USER_FLAG seq=(\d+)")
STATION_LABELS = {
    "URL1": "102.7",
    "URL2": "104.7",
    "102": "102.7",
    "104": "104.7",
    "102.7": "102.7",
    "104.7": "104.7",
}
STATUS_RANK = {"playing": 0, "silent": 1, "offline": 2}
KNOWN_STATIONS = ("102.7", "104.7")
try:
    FORTALEZA_TZ = ZoneInfo("America/Fortaleza")
except ZoneInfoNotFoundError:
    FORTALEZA_TZ = timezone(timedelta(hours=-3))

app = Flask(__name__, static_folder=str(STATIC_DIR))
lock = threading.Lock()
log_lock = threading.RLock()
flash_lock = threading.Lock()
device_logs: dict[str, deque[dict]] = defaultdict(lambda: deque(maxlen=LOG_LINES_PER_DEVICE))
device_seen: dict[str, float] = {}
devices_registry: dict[str, dict] = {}
geo_cache: dict[str, dict] = {}
geo_inflight: set[str] = set()
geocode_cache: dict[str, dict] = {}
geocode_inflight: set[str] = set()
_nominatim_lock = threading.Lock()
_nominatim_last_ts = 0.0
_devices_save_timer: threading.Timer | None = None
subscribers: list[queue.Queue] = []

FW_DIR.mkdir(parents=True, exist_ok=True)
FACTORY_DIR.mkdir(parents=True, exist_ok=True)
FACTORY_NEXT_DIR.mkdir(parents=True, exist_ok=True)
STATIC_DIR.mkdir(parents=True, exist_ok=True)
LOGS_DIR.mkdir(parents=True, exist_ok=True)

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


def utc_iso(ts: float | None = None) -> str:
    return datetime.fromtimestamp(ts if ts is not None else time.time(), tz=timezone.utc).isoformat()


def parse_iso_ts(value) -> float | None:
    if value is None or value == "":
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if not isinstance(value, str):
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def parse_esp_log_line(line: str) -> dict:
    m = ESP_LOG_RE.match(line)
    if not m:
        return {"level": None, "tag": None, "device_ms": None, "msg": line}
    return {
        "level": m.group(1),
        "tag": m.group(3),
        "device_ms": int(m.group(2)),
        "msg": m.group(4),
    }


def strip_ip_port(raw: str) -> str:
    host = (raw or "").strip()
    if not host:
        return ""
    if host.startswith("["):
        end = host.find("]")
        if end != -1:
            return host[1:end]
    if host.count(":") == 1:
        return host.rsplit(":", 1)[0]
    return host


def parse_ip(raw: str | None) -> str:
    if not raw:
        return ""
    host = strip_ip_port(raw)
    if not host:
        return ""
    try:
        ipaddress.ip_address(host)
    except ValueError:
        return ""
    return host


def ip_ok_for_geo(ip: str) -> bool:
    try:
        return ipaddress.ip_address(ip).is_global
    except ValueError:
        return False


def client_ip() -> str:
    headers = request.headers
    candidates = []
    cf = headers.get("CF-Connecting-IP")
    if cf:
        candidates.append(cf)
    xff = headers.get("X-Forwarded-For")
    if xff:
        candidates.append(xff.split(",")[0])
    xri = headers.get("X-Real-IP")
    if xri:
        candidates.append(xri)
    if request.remote_addr:
        candidates.append(request.remote_addr)
    for raw in candidates:
        ip = parse_ip(raw)
        if ip:
            return ip
    return ""


def device_log_path(device_id: str) -> Path:
    safe = re.sub(r"[^a-z0-9:-]", "_", (device_id or "unknown").lower())
    name = safe.replace(":", "-") or "unknown"
    return LOGS_DIR / f"{name}.jsonl"


def make_log_entry(device_id: str, line: str, now: float, seq) -> dict:
    parsed = parse_esp_log_line(line)
    return {
        "device_id": device_id,
        "line": line,
        "level": parsed["level"],
        "tag": parsed["tag"],
        "device_ms": parsed["device_ms"],
        "msg": parsed["msg"],
        "seq": seq,
        "ts": now,
        "server_ts": utc_iso(now),
    }


def devices_for_disk() -> dict:
    out = {}
    for did, rec in devices_registry.items():
        out[did] = rec
    return out


def flush_devices_now() -> None:
    global _devices_save_timer
    with log_lock:
        timer = _devices_save_timer
        _devices_save_timer = None
        snapshot = json.dumps(devices_for_disk(), indent=2) + "\n"
    if timer is not None:
        timer.cancel()
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    tmp = DEVICES_PATH.with_name("devices.json.tmp")
    tmp.write_text(snapshot)
    tmp.replace(DEVICES_PATH)


def schedule_save_devices() -> None:
    global _devices_save_timer

    def fire() -> None:
        global _devices_save_timer
        with log_lock:
            _devices_save_timer = None
        flush_devices_now()

    with log_lock:
        if _devices_save_timer is not None:
            _devices_save_timer.cancel()
        _devices_save_timer = threading.Timer(DEVICES_SAVE_DEBOUNCE_S, fire)
        _devices_save_timer.daemon = True
        _devices_save_timer.start()


def coerce_nonneg_int(val, default=None) -> int | None:
    if isinstance(val, bool) or val is None or val == "":
        return default
    try:
        n = int(val)
    except (TypeError, ValueError):
        try:
            n = int(float(val))
        except (TypeError, ValueError):
            return default
    return max(0, n)


def coerce_bool(val) -> bool:
    if isinstance(val, bool):
        return val
    if isinstance(val, (int, float)):
        return val != 0
    if isinstance(val, str):
        return val.strip().lower() in ("1", "true", "yes", "on")
    return False


def listener_loyalty_s(rec: dict) -> int:
    loyalty = coerce_nonneg_int(rec.get("listen_s_server"), None)
    if loyalty is None:
        loyalty = coerce_nonneg_int(rec.get("listen_s"), 0) or 0
    return loyalty


def is_currently_listening(did: str, rec: dict, now: float) -> bool:
    if not coerce_bool(rec.get("playing")):
        return False
    last_unix = last_seen_unix(did, rec)
    if last_unix is None:
        return False
    return (now - last_unix) <= PLAYING_WINDOW_S


def last_seen_unix(did: str, rec: dict) -> float | None:
    last_unix = device_seen.get(did)
    if last_unix is None:
        last_unix = parse_iso_ts(rec.get("last_seen"))
    return last_unix


def fortaleza_dt(now: float) -> datetime:
    return datetime.fromtimestamp(now, tz=FORTALEZA_TZ)


def fortaleza_date_str(now: float) -> str:
    return fortaleza_dt(now).strftime("%Y-%m-%d")


def ensure_session_state(rec: dict) -> dict:
    rec.setdefault("session_open", False)
    rec.setdefault("session_start", None)
    rec.setdefault("session_listen_at_start", 0)
    rec.setdefault("last_session_s", 0)
    rec.setdefault("sessions_today", 0)
    rec.setdefault("sessions_total", 0)
    rec.setdefault("listen_today_s", 0)
    rec.setdefault("today", None)
    rec.setdefault("listen_by_station", {})
    rec.setdefault("last_flag_at", None)
    rec.setdefault("last_flag_seq", None)
    rec.setdefault("flag_count", 0)
    rec["session_open"] = bool(rec.get("session_open"))
    rec["last_session_s"] = coerce_nonneg_int(rec.get("last_session_s"), 0) or 0
    rec["sessions_today"] = coerce_nonneg_int(rec.get("sessions_today"), 0) or 0
    rec["sessions_total"] = coerce_nonneg_int(rec.get("sessions_total"), 0) or 0
    rec["listen_today_s"] = coerce_nonneg_int(rec.get("listen_today_s"), 0) or 0
    rec["session_listen_at_start"] = coerce_nonneg_int(rec.get("session_listen_at_start"), 0) or 0
    rec["flag_count"] = coerce_nonneg_int(rec.get("flag_count"), 0) or 0
    start = rec.get("session_start")
    if start is not None:
        try:
            rec["session_start"] = float(start)
        except (TypeError, ValueError):
            rec["session_start"] = None
    hist = rec.get("hour_hist")
    if not isinstance(hist, list) or len(hist) != 24:
        rec["hour_hist"] = [0] * 24
    else:
        rec["hour_hist"] = [
            int(x) if isinstance(x, (int, float)) and not isinstance(x, bool) else 0
            for x in hist
        ]
    buckets = rec.get("listen_by_station")
    if not isinstance(buckets, dict):
        rec["listen_by_station"] = {}
    else:
        cleaned = {}
        for key, val in buckets.items():
            n = coerce_nonneg_int(val, 0) or 0
            if n:
                cleaned[str(key)] = n
        rec["listen_by_station"] = cleaned
    return rec


def roll_today(rec: dict, now: float) -> bool:
    ensure_session_state(rec)
    today = fortaleza_date_str(now)
    prev = rec.get("today")
    changed = False
    if prev and prev != today:
        rec["listen_today_s"] = 0
        rec["sessions_today"] = 0
        changed = True
    if prev != today:
        rec["today"] = today
        changed = True
    return changed


def open_session(rec: dict, now: float, listen_at_start: int) -> None:
    ensure_session_state(rec)
    rec["session_open"] = True
    rec["session_start"] = float(now)
    rec["session_listen_at_start"] = max(0, int(listen_at_start))
    rec["sessions_today"] = int(rec.get("sessions_today") or 0) + 1
    rec["sessions_total"] = int(rec.get("sessions_total") or 0) + 1
    hour = fortaleza_dt(now).hour
    rec["hour_hist"][hour] = int(rec["hour_hist"][hour]) + 1


def session_duration_s(rec: dict, now: float) -> int:
    listen_now = coerce_nonneg_int(rec.get("listen_s_server"), 0) or 0
    start_listen = coerce_nonneg_int(rec.get("session_listen_at_start"), 0) or 0
    listen_delta = max(0, listen_now - start_listen)
    start = rec.get("session_start")
    try:
        wall_s = max(0, int(now - float(start))) if start is not None else 0
    except (TypeError, ValueError):
        wall_s = 0
    # Prefer listen_s delta; wall clock is the fallback.
    return listen_delta if listen_delta > 0 else wall_s


def close_session(rec: dict, now: float) -> None:
    ensure_session_state(rec)
    if not rec.get("session_open"):
        return
    rec["last_session_s"] = session_duration_s(rec, now)
    rec["session_open"] = False


def add_listen_delta(rec: dict, delta: int) -> None:
    if delta <= 0:
        return
    rec["listen_today_s"] = int(rec.get("listen_today_s") or 0) + delta
    station = rec.get("station")
    key = str(station).strip() if station is not None else ""
    if not key:
        return
    buckets = rec.setdefault("listen_by_station", {})
    if not isinstance(buckets, dict):
        buckets = {}
        rec["listen_by_station"] = buckets
    buckets[key] = int(buckets.get(key) or 0) + delta


def apply_sessionize(did: str, rec: dict, now: float, listen_delta: int, playing_in_payload: bool | None) -> None:
    roll_today(rec, now)
    is_playing_now = is_currently_listening(did, rec, now)
    if is_playing_now and not rec.get("session_open"):
        listen_now = coerce_nonneg_int(rec.get("listen_s_server"), 0) or 0
        open_session(rec, now, max(0, listen_now - max(0, listen_delta)))
    if rec.get("session_open") and listen_delta > 0:
        add_listen_delta(rec, listen_delta)
    if playing_in_payload is False:
        close_session(rec, now)


def close_stale_sessions(now: float) -> bool:
    changed = False
    for did, rec in list(devices_registry.items()):
        if not isinstance(rec, dict):
            continue
        ensure_session_state(rec)
        if roll_today(rec, now):
            changed = True
        if not rec.get("session_open"):
            continue
        last_unix = last_seen_unix(did, rec)
        if last_unix is None or (now - last_unix) > PLAYING_WINDOW_S:
            close_session(rec, now)
            changed = True
    return changed


def parse_user_flag_seq(text: str) -> int | None:
    if not text:
        return None
    m = USER_FLAG_RE.search(text)
    if not m:
        return None
    try:
        return int(m.group(1))
    except (TypeError, ValueError):
        return None


def apply_user_flag(rec: dict, seq: int, ts: float) -> None:
    ensure_session_state(rec)
    rec["last_flag_seq"] = int(seq)
    rec["last_flag_at"] = utc_iso(ts)
    rec["flag_count"] = int(rec.get("flag_count") or 0) + 1


def ingest_user_flags(rec: dict, lines: list[str], now: float) -> None:
    for line in lines:
        seq = parse_user_flag_seq(line)
        if seq is not None:
            apply_user_flag(rec, seq, now)


def backfill_user_flags() -> None:
    for did, dq in device_logs.items():
        rec = devices_registry.setdefault(did, {"device_id": did})
        ensure_session_state(rec)
        flags: list[tuple[int, float]] = []
        for obj in dq:
            if not isinstance(obj, dict):
                continue
            line = obj.get("line") or obj.get("msg") or ""
            seq = parse_user_flag_seq(str(line))
            if seq is None:
                continue
            ts = obj.get("ts")
            try:
                ts_f = float(ts) if ts is not None else time.time()
            except (TypeError, ValueError):
                ts_f = time.time()
            flags.append((seq, ts_f))
        if not flags:
            continue
        last_seq, last_ts = flags[-1]
        rec["last_flag_seq"] = last_seq
        rec["last_flag_at"] = utc_iso(last_ts)
        rec["flag_count"] = max(int(rec.get("flag_count") or 0), len(flags))


def typical_hour(rec: dict) -> int | None:
    hist = rec.get("hour_hist")
    if not isinstance(hist, list) or len(hist) != 24:
        return None
    best = 0
    best_h = None
    for h, n in enumerate(hist):
        try:
            v = int(n)
        except (TypeError, ValueError):
            continue
        if v > best:
            best = v
            best_h = h
    return best_h


def api_session_s(rec: dict, now: float) -> int:
    fw = coerce_nonneg_int(rec.get("session_s"), 0) or 0
    if rec.get("session_open"):
        computed = session_duration_s(rec, now)
        return computed if computed > 0 else fw
    last = coerce_nonneg_int(rec.get("last_session_s"), 0) or 0
    return fw if fw > 0 else last


def listener_status(did: str, rec: dict, now: float) -> tuple[str, bool, bool]:
    last_unix = last_seen_unix(did, rec)
    online = bool(last_unix is not None and (now - last_unix) <= ONLINE_WINDOW_S)
    playing = is_currently_listening(did, rec, now)
    if playing:
        status = "playing"
    elif online:
        status = "silent"
    else:
        status = "offline"
    return status, online, playing


def station_listen_buckets(rec: dict) -> dict[str, int]:
    out: dict[str, int] = {}
    by_st = rec.get("listen_by_station")
    if isinstance(by_st, dict) and by_st:
        for key, val in by_st.items():
            label = station_label(key)
            if not label:
                continue
            out[label] = out.get(label, 0) + (coerce_nonneg_int(val, 0) or 0)
        return out
    label = station_label(rec.get("station"))
    loyalty = listener_loyalty_s(rec)
    if label and loyalty:
        out[label] = loyalty
    return out


def public_device_id(device_id: str) -> str:
    return hashlib.sha256(f"{device_id}{OUVINTES_ID_SALT}".encode("utf-8")).hexdigest()[:8]


def station_label(station) -> str:
    raw = "" if station is None else str(station).strip()
    if not raw:
        return ""
    mapped = STATION_LABELS.get(raw.upper()) or STATION_LABELS.get(raw)
    if mapped:
        return mapped
    if raw.lower().startswith("http"):
        return ""
    return raw[:20]


def normalize_city(city: str) -> str:
    return " ".join((city or "").split()).casefold()


def flush_geocode_now() -> None:
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    with log_lock:
        snapshot = json.dumps(geocode_cache, indent=2) + "\n"
    tmp = GEOCODE_PATH.with_name("geocode.json.tmp")
    tmp.write_text(snapshot)
    tmp.replace(GEOCODE_PATH)


def load_geocode_cache() -> None:
    if not GEOCODE_PATH.is_file():
        return
    try:
        data = json.loads(GEOCODE_PATH.read_text())
    except (json.JSONDecodeError, OSError):
        return
    if not isinstance(data, dict):
        return
    with log_lock:
        for key, val in data.items():
            if isinstance(val, dict):
                geocode_cache[str(key)] = val


def maybe_geocode_city(city: str) -> None:
    key = normalize_city(city)
    if not key:
        return
    now = time.time()
    with log_lock:
        cached = geocode_cache.get(key)
        if cached:
            if cached.get("ok"):
                return
            if (now - float(cached.get("at") or 0)) < GEO_TTL_S:
                return
        if key in geocode_inflight:
            return
        geocode_inflight.add(key)
    threading.Thread(
        target=_geocode_worker,
        args=(city.strip(), key),
        name=f"geocode-{key[:24]}",
        daemon=True,
    ).start()


def _geocode_worker(city: str, key: str) -> None:
    global _nominatim_last_ts
    geo = None
    try:
        with _nominatim_lock:
            wait = NOMINATIM_MIN_INTERVAL_S - (time.time() - _nominatim_last_ts)
            if wait > 0:
                time.sleep(wait)
            _nominatim_last_ts = time.time()
            q = urllib.parse.urlencode({
                "format": "json",
                "limit": "1",
                "countrycodes": "br",
                "q": city,
            })
            url = "https://nominatim.openstreetmap.org/search?" + q
            req = urllib.request.Request(
                url,
                headers={
                    "User-Agent": "SearaBoom/1.0",
                    "Accept-Language": "pt-BR,pt,en",
                },
            )
            with urllib.request.urlopen(req, timeout=8) as resp:
                body = json.loads(resp.read().decode("utf-8", errors="replace"))
        if isinstance(body, list) and body:
            hit = body[0] if isinstance(body[0], dict) else None
            if hit and hit.get("lat") is not None and hit.get("lon") is not None:
                geo = {
                    "ok": True,
                    "lat": float(hit["lat"]),
                    "lon": float(hit["lon"]),
                    "display_name": hit.get("display_name"),
                }
    except Exception:
        geo = None
    with log_lock:
        geocode_inflight.discard(key)
        geocode_cache[key] = geo if geo else {"ok": False, "at": time.time()}
    try:
        flush_geocode_now()
    except OSError:
        pass


def listener_coords(rec: dict) -> tuple[float | None, float | None]:
    city = (rec.get("city") or "").strip()
    if city:
        cached = geocode_cache.get(normalize_city(city))
        if cached and cached.get("ok") and cached.get("lat") is not None and cached.get("lon") is not None:
            try:
                return float(cached["lat"]), float(cached["lon"])
            except (TypeError, ValueError):
                return None, None
        return None, None
    geo = rec.get("geo") or {}
    if isinstance(geo, dict) and geo.get("lat") is not None and geo.get("lon") is not None:
        try:
            return float(geo["lat"]), float(geo["lon"])
        except (TypeError, ValueError):
            return None, None
    return None, None


def public_listener_row(did: str, rec: dict, now: float) -> dict:
    ensure_session_state(rec)
    loyalty = listener_loyalty_s(rec)
    status, online, playing = listener_status(did, rec, now)
    name = (rec.get("name") or "").strip() or "Ouvinte"
    city = (rec.get("city") or "").strip()
    if not city:
        geo = rec.get("geo") or {}
        if isinstance(geo, dict):
            city = (geo.get("city") or "").strip()
    rssi = rec.get("rssi")
    if isinstance(rssi, bool):
        rssi = None
    elif isinstance(rssi, (int, float)):
        rssi = int(rssi)
    else:
        rssi = None
    last_flag_seq = rec.get("last_flag_seq")
    try:
        last_flag_seq = int(last_flag_seq) if last_flag_seq is not None else None
    except (TypeError, ValueError):
        last_flag_seq = None
    row = {
        "id": public_device_id(did),
        "device_id": did,
        "name": name,
        "city": city,
        "station": station_label(rec.get("station")),
        "listen_s": loyalty,
        "listen_today_s": int(rec.get("listen_today_s") or 0),
        "last_session_s": int(rec.get("last_session_s") or 0),
        "session_s": api_session_s(rec, now),
        "sessions_today": int(rec.get("sessions_today") or 0),
        "sessions_total": int(rec.get("sessions_total") or 0),
        "playing": playing,
        "online": online,
        "status": status,
        "rssi": rssi,
        "fw": rec.get("fw"),
        "last_seen": rec.get("last_seen"),
        "last_flag_at": rec.get("last_flag_at"),
        "last_flag_seq": last_flag_seq,
        "flag_count": int(rec.get("flag_count") or 0),
        "typical_hour": typical_hour(rec),
    }
    lat, lon = listener_coords(rec)
    if lat is not None and lon is not None:
        row["lat"] = lat
        row["lon"] = lon
    return row


def warmup_city_geocode() -> None:
    with log_lock:
        cities = [
            (rec.get("city") or "").strip()
            for rec in devices_registry.values()
            if isinstance(rec, dict)
        ]
    for city in cities:
        if city:
            maybe_geocode_city(city)


def update_registry(device_id: str, data: dict, ip: str, now: float, n_new_lines: int) -> dict:
    rec = devices_registry.get(device_id)
    if rec is None:
        rec = {
            "device_id": device_id,
            "first_seen": utc_iso(now),
            "last_seen": utc_iso(now),
            "lines": 0,
        }
        devices_registry[device_id] = rec
    rec["device_id"] = device_id
    rec["last_seen"] = utc_iso(now)
    rec["lines"] = int(rec.get("lines") or 0) + n_new_lines
    if ip:
        rec["ip"] = ip
    for key in ("fw", "reset", "uptime_ms", "heap", "rssi", "station", "seq", "name", "city"):
        if key not in data:
            continue
        val = data[key]
        if key in ("name", "city"):
            if not isinstance(val, str):
                val = "" if val is None else str(val)
            val = "".join(ch for ch in val.strip() if ch >= " " and ch not in "\"\\<>")[:40]
        rec[key] = val
    ensure_session_state(rec)
    listen_delta = 0
    if "listen_s" in data:
        fw = coerce_nonneg_int(data.get("listen_s"), 0) or 0
        last_fw = coerce_nonneg_int(rec.get("listen_s"), None)
        old_server = coerce_nonneg_int(rec.get("listen_s_server"), 0) or 0
        if fw >= old_server:
            new_server = fw
        else:
            delta = max(0, fw - last_fw) if last_fw is not None else 0
            new_server = old_server + delta
        listen_delta = max(0, new_server - old_server)
        rec["listen_s"] = fw
        rec["listen_s_server"] = new_server
    if "session_s" in data:
        sess = coerce_nonneg_int(data.get("session_s"), None)
        if sess is not None:
            rec["session_s"] = sess
    playing_in_payload = None
    if "playing" in data:
        playing = coerce_bool(data.get("playing"))
        rec["playing"] = playing
        playing_in_payload = playing
        if playing:
            rec["playing_at"] = utc_iso(now)
    device_seen[device_id] = now
    apply_sessionize(device_id, rec, now, listen_delta, playing_in_payload)
    return rec


def device_view(did: str, now: float) -> dict:
    rec = dict(devices_registry.get(did) or {"device_id": did})
    rec["device_id"] = did
    last_unix = device_seen.get(did)
    rec["online"] = bool(last_unix is not None and (now - last_unix) <= ONLINE_WINDOW_S)
    if "last_seen" not in rec:
        rec["last_seen"] = utc_iso(last_unix) if last_unix is not None else None
    if "lines" not in rec:
        rec["lines"] = len(device_logs.get(did, []))
    rec.setdefault("geo", {})
    rec["listen_s"] = listener_loyalty_s(rec)
    rec["session_s"] = coerce_nonneg_int(rec.get("session_s"), 0) or 0
    rec["playing"] = is_currently_listening(did, rec, now)
    rec.setdefault("name", rec.get("name") or "")
    rec.setdefault("city", rec.get("city") or "")
    return rec


def resolve_device_id(raw: str) -> str | None:
    did = urllib.parse.unquote(raw or "").strip().lower()
    if not did:
        return None
    if did in devices_registry or did in device_logs:
        return did
    alt = did.replace("-", ":")
    if alt in devices_registry or alt in device_logs:
        return alt
    return None


def maybe_geolocate(device_id: str, ip: str) -> None:
    if not ip or not ip_ok_for_geo(ip):
        return
    now = time.time()
    with log_lock:
        rec = devices_registry.get(device_id) or {}
        cached = geo_cache.get(ip)
        if cached and (now - cached.get("at", 0)) < GEO_TTL_S:
            geo = cached.get("geo")
            if geo and rec.get("geo") != geo:
                rec["geo"] = geo
                schedule_save_devices()
            return
        if ip in geo_inflight:
            return
        geo_inflight.add(ip)
    threading.Thread(
        target=_geo_worker,
        args=(device_id, ip),
        name=f"geo-{ip}",
        daemon=True,
    ).start()


def _geo_worker(device_id: str, ip: str) -> None:
    geo = None
    try:
        url = (
            "http://ip-api.com/json/"
            + urllib.parse.quote(ip, safe=":")
            + "?fields=status,country,countryCode,region,regionName,city,lat,lon,query"
        )
        req = urllib.request.Request(url, headers={"User-Agent": "SearaBoom/1.0"})
        with urllib.request.urlopen(req, timeout=GEO_LOOKUP_TIMEOUT_S) as resp:
            body = json.loads(resp.read().decode("utf-8", errors="replace"))
        if isinstance(body, dict) and body.get("status") == "success":
            geo = {
                "country": body.get("country"),
                "country_code": body.get("countryCode"),
                "region": body.get("regionName") or body.get("region"),
                "city": body.get("city"),
                "lat": body.get("lat"),
                "lon": body.get("lon"),
            }
    except Exception:
        geo = None
    with log_lock:
        geo_inflight.discard(ip)
        geo_cache[ip] = {"at": time.time(), "geo": geo}
        if geo:
            rec = devices_registry.get(device_id)
            if rec is not None and rec.get("ip") == ip:
                rec["geo"] = geo
                schedule_save_devices()


def load_persisted_logs() -> None:
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    if DEVICES_PATH.is_file():
        try:
            data = json.loads(DEVICES_PATH.read_text())
        except (json.JSONDecodeError, OSError):
            data = None
        if isinstance(data, dict):
            for key, rec in data.items():
                if not isinstance(rec, dict):
                    continue
                did = str(rec.get("device_id") or key).strip().lower()
                rec = dict(rec)
                rec["device_id"] = did
                devices_registry[did] = rec
                unix = parse_iso_ts(rec.get("last_seen"))
                if unix is not None:
                    device_seen[did] = unix
                ip = rec.get("ip")
                geo = rec.get("geo")
                if ip and geo:
                    geo_cache[str(ip)] = {"at": time.time(), "geo": geo}
    for path in sorted(LOGS_DIR.glob("*.jsonl")):
        loaded: list[dict] = []
        total = 0
        try:
            with path.open(encoding="utf-8") as f:
                for raw in f:
                    raw = raw.strip()
                    if not raw:
                        continue
                    total += 1
                    try:
                        obj = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(obj, dict):
                        loaded.append(obj)
        except OSError:
            continue
        if not loaded:
            continue
        did = str(loaded[-1].get("device_id") or path.stem.replace("-", ":")).strip().lower()
        dq = device_logs[did]
        for obj in loaded[-LOG_LINES_PER_DEVICE:]:
            dq.append(obj)
        rec = devices_registry.setdefault(did, {"device_id": did, "lines": 0})
        rec["device_id"] = did
        if int(rec.get("lines") or 0) < total:
            rec["lines"] = total
        last = loaded[-1]
        last_seq = last.get("seq")
        if last_seq is not None:
            try:
                last_seq_i = int(last_seq)
                rec_seq = rec.get("seq")
                rec_seq_i = int(rec_seq) if rec_seq is not None else None
                if rec_seq_i is None or last_seq_i > rec_seq_i:
                    rec["seq"] = last_seq_i
            except (TypeError, ValueError):
                pass
        last_ts = last.get("ts")
        if did not in device_seen and isinstance(last_ts, (int, float)):
            device_seen[did] = float(last_ts)
            rec.setdefault("last_seen", utc_iso(float(last_ts)))
            rec.setdefault("first_seen", rec["last_seen"])
        rec.setdefault("first_seen", rec.get("last_seen") or utc_iso())
        rec.setdefault("last_seen", rec.get("first_seen"))


load_persisted_logs()
backfill_user_flags()
load_geocode_cache()
threading.Thread(target=warmup_city_geocode, name="geocode-warmup", daemon=True).start()
atexit.register(flush_devices_now)


def _handle_stop(_signum, _frame):
    try:
        flush_devices_now()
    except Exception:
        pass
    raise SystemExit(0)


signal.signal(signal.SIGTERM, _handle_stop)
signal.signal(signal.SIGINT, _handle_stop)


@app.get("/")
def index():
    return send_from_directory(STATIC_DIR, "index.html")


@app.get("/admin")
def admin():
    return send_from_directory(STATIC_DIR, "admin.html")


@app.get("/ovintes")
def ovintes_page():
    return send_from_directory(STATIC_DIR, "ovintes.html")


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
    now = time.time()
    with log_lock:
        ids = sorted(set(device_logs.keys()) | set(devices_registry.keys()))
        devices = []
        for did in ids:
            rec = device_view(did, now)
            devices.append({
                "device_id": did,
                "last_seen": rec.get("last_seen"),
                "lines": rec.get("lines", len(device_logs.get(did, []))),
                "fw": rec.get("fw"),
                "ip": rec.get("ip"),
                "geo": rec.get("geo") or {},
                "online": rec.get("online"),
                "rssi": rec.get("rssi"),
                "station": rec.get("station"),
                "name": rec.get("name"),
                "city": rec.get("city"),
                "listen_s": rec.get("listen_s"),
                "session_s": rec.get("session_s"),
                "playing": rec.get("playing"),
            })
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
    if not isinstance(data, dict):
        data = {}
    device_id = str(data.get("device_id") or "unknown").strip().lower()
    text = data.get("text") or ""
    if not isinstance(text, str):
        text = str(text)
    now = time.time()
    ip = client_ip()
    seq = data.get("seq") if "seq" in data else None
    entries: list[dict] = []
    city = ""
    with log_lock:
        rec = devices_registry.get(device_id)
        prev_seq = rec.get("seq") if rec else None
        try:
            prev_i = int(prev_seq) if prev_seq is not None else None
            new_i = int(seq) if seq is not None else None
        except (TypeError, ValueError):
            prev_i, new_i = None, None
        if prev_i is not None and new_i is not None and new_i - prev_i > 1:
            entries.append(make_log_entry(device_id, f"log_gap prev={prev_i} got={new_i}", now, new_i))
        for line in text.splitlines():
            if not line:
                continue
            entries.append(make_log_entry(device_id, line, now, seq))
        n_text = sum(1 for line in text.splitlines() if line)
        rec = update_registry(device_id, data, ip, now, len(entries))
        ingest_user_flags(rec, [e.get("line") or "" for e in entries], now)
        city = (rec.get("city") or "").strip()
        if entries:
            path = device_log_path(device_id)
            with path.open("a", encoding="utf-8") as f:
                for entry in entries:
                    device_logs[device_id].append(entry)
                    f.write(json.dumps(entry, separators=(",", ":")) + "\n")
                    publish_log_event(entry)
        schedule_save_devices()
    maybe_geolocate(device_id, ip)
    if city:
        maybe_geocode_city(city)
    return jsonify({"ok": True, "accepted": n_text, "device_id": device_id})


@app.get("/api/logs")
def logs_get():
    device = (request.args.get("device") or "").strip().lower()
    try:
        limit = min(max(int(request.args.get("limit", "200")), 0), LOG_LINES_PER_DEVICE)
    except (TypeError, ValueError):
        limit = 200
    level = (request.args.get("level") or "").strip().upper() or None
    since = None
    since_raw = request.args.get("since")
    if since_raw not in (None, ""):
        try:
            since = float(since_raw)
        except (TypeError, ValueError):
            since = None
    with log_lock:
        if device:
            lines = list(device_logs.get(device, []))
        else:
            merged: list[dict] = []
            for dq in device_logs.values():
                merged.extend(dq)
            merged.sort(key=lambda e: e.get("ts", 0))
            lines = merged
    if since is not None:
        lines = [e for e in lines if float(e.get("ts") or 0) > since]
    if level:
        lines = [e for e in lines if (e.get("level") or "") == level]
    lines = lines[-limit:]
    return jsonify({"ok": True, "lines": lines})


@app.get("/api/devices")
def devices_list():
    now = time.time()
    with log_lock:
        if close_stale_sessions(now):
            schedule_save_devices()
        ids = sorted(set(devices_registry.keys()) | set(device_logs.keys()))
        devices = [device_view(did, now) for did in ids]
    return jsonify({"ok": True, "devices": devices})


@app.get("/api/ouvintes")
def ouvintes_api():
    now = time.time()
    cities_needed: list[str] = []
    with log_lock:
        if close_stale_sessions(now):
            schedule_save_devices()
        ids = set(devices_registry.keys()) | set(device_logs.keys())
        rows = []
        listening = 0
        online_n = 0
        silent_n = 0
        stations: dict[str, dict] = {
            label: {"listen_s": 0, "playing": 0} for label in KNOWN_STATIONS
        }
        for did in ids:
            rec = devices_registry.get(did) or {"device_id": did}
            city = (rec.get("city") or "").strip()
            if city:
                cached = geocode_cache.get(normalize_city(city))
                if not (cached and cached.get("ok")):
                    cities_needed.append(city)
            row = public_listener_row(did, rec, now)
            rows.append(row)
            if row.get("status") == "playing":
                listening += 1
            if row.get("online"):
                online_n += 1
            if row.get("status") == "silent":
                silent_n += 1
            for label, secs in station_listen_buckets(rec).items():
                bucket = stations.setdefault(label, {"listen_s": 0, "playing": 0})
                bucket["listen_s"] += secs
            label = row.get("station") or ""
            if label and row.get("playing"):
                stations.setdefault(label, {"listen_s": 0, "playing": 0})
                stations[label]["playing"] += 1
        rows.sort(key=lambda r: (
            STATUS_RANK.get(str(r.get("status") or ""), 9),
            -int(r.get("listen_s") or 0),
            str(r.get("name") or "").casefold(),
        ))
    for city in dict.fromkeys(cities_needed):
        maybe_geocode_city(city)
    return jsonify({
        "ok": True,
        "listening": listening,
        "online": online_n,
        "silent": silent_n,
        "stations": stations,
        "listeners": rows,
    })


@app.get("/api/devices/<device_id>")
def device_one(device_id: str):
    with log_lock:
        did = resolve_device_id(device_id)
        if not did:
            return jsonify({"error": "unknown device", "device_id": device_id}), 404
        rec = device_view(did, time.time())
        rec["recent"] = list(device_logs.get(did, []))[-50:]
    return jsonify(rec)


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
    """Unused if firmware stays on direct Brasilstream (0.2.0+).

    Boxes open 8396/8404.brasilstream.com.br themselves. This proxy only
    relabeled Content-Type to audio/adts for ADF. Remove STREAMS and this
    route once no field firmware still hits /stream/102|/stream/104.
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
