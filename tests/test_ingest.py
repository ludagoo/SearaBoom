#!/usr/bin/env python3
import os
import sys
import tempfile
from pathlib import Path

TMP = Path(tempfile.mkdtemp(prefix="searaboom-ingest-"))
os.environ["SEARABOOM_LOGS_DIR"] = str(TMP)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
import app as sb  # noqa: E402


def test_parse_byte_range() -> None:
    assert sb.parse_byte_range(None, 100) is None
    assert sb.parse_byte_range("bytes=0-9", 100) == (0, 9)
    assert sb.parse_byte_range("bytes=50-", 100) == (50, 99)
    assert sb.parse_byte_range("bytes=-10", 100) == (90, 99)
    try:
        sb.parse_byte_range("bytes=100-110", 100)
        raise AssertionError("expected ValueError")
    except ValueError:
        pass


def test_ingest_old_and_new() -> None:
    sb.devices_registry.clear()
    sb.device_logs.clear()
    sb.device_seen.clear()
    c = sb.app.test_client()
    r = c.post("/api/logs", json={"device_id": "aa:bb:cc:dd:ee:ff", "text": "I (1) radio: go live"})
    assert r.status_code == 200, r.data
    body = r.get_json()
    assert body["ok"] is True
    assert body["accepted"] == 1
    assert body["device_id"] == "aa:bb:cc:dd:ee:ff"

    r = c.post("/api/logs", json={
        "device_id": "aa:bb:cc:dd:ee:ff",
        "fw": "0.5.20",
        "reset": "PANIC",
        "uptime_ms": 1200,
        "heap": 140000,
        "heap_min": 90000,
        "heap_int": 16000,
        "crashes": 2,
        "rssi": -62,
        "seq": 3,
        "text": "E (99) ota_update: boom",
    })
    assert r.status_code == 200, r.data
    body = r.get_json()
    assert body["seq"] == 3
    rec = sb.devices_registry["aa:bb:cc:dd:ee:ff"]
    assert rec["reset"] == "PANIC"
    assert rec["crashes"] == 2
    assert rec["heap_min"] == 90000
    assert rec["heap_int"] == 16000
    assert rec["rssi"] == -62
    assert rec["last_crash_reset"] == "PANIC"
    assert rec["crash_count"] == 1

    r = c.post("/api/logs", json={
        "device_id": "aa:bb:cc:dd:ee:ff",
        "reset": "PANIC",
        "uptime_ms": 5000,
        "seq": 4,
        "text": "",
    })
    assert r.status_code == 200, r.data
    rec = sb.devices_registry["aa:bb:cc:dd:ee:ff"]
    assert rec["crash_count"] == 1

    r = c.post("/api/logs", json={
        "device_id": "aa:bb:cc:dd:ee:ff",
        "uptime_ms": 8000,
        "seq": 7,
        "text": "I (8) radio: hi",
    })
    assert r.status_code == 200, r.data
    lines = [e["line"] for e in sb.device_logs["aa:bb:cc:dd:ee:ff"]]
    assert any(line.startswith("log_gap") for line in lines)


def test_bad_json() -> None:
    c = sb.app.test_client()
    r = c.post("/api/logs", data="{nope", content_type="application/json")
    assert r.status_code == 400
    assert r.get_json()["error"] == "invalid json"


def test_firmware_range() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="searaboom-fw-"))
    fw = tmp / "tiny.bin"
    payload = b"ABCDEFGHIJ"
    fw.write_bytes(payload)
    old = sb.FW_DIR
    sb.FW_DIR = tmp
    try:
        c = sb.app.test_client()
        r = c.get("/api/firmware/download/tiny.bin")
        assert r.status_code == 200, r.data
        assert r.data == payload
        assert r.headers.get("Accept-Ranges") == "bytes"
        r = c.get("/api/firmware/download/tiny.bin", headers={"Range": "bytes=2-5"})
        assert r.status_code == 206, r.data
        assert r.data == b"CDEF"
        assert r.headers.get("Content-Range") == "bytes 2-5/10"
        r = c.get("/api/firmware/download/tiny.bin", headers={"Range": "bytes=99-100"})
        assert r.status_code == 416
    finally:
        sb.FW_DIR = old


if __name__ == "__main__":
    test_parse_byte_range()
    test_ingest_old_and_new()
    test_bad_json()
    test_firmware_range()
    print("ok")
