#!/usr/bin/env python3
from __future__ import annotations

import io
import json
import sys
import zipfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from factory_flasher.core import (  # noqa: E402
    UsbPort,
    eligible_ports,
    esptool_write_args,
    fetch_factory_image,
    is_qa_device_path,
    new_ports,
    parse_board_line,
    parse_progress_line,
    parse_version_line,
    cal_prompt_from_line,
    validate_manifest,
)
from factory_flasher.layout import FACTORY_FILES, factory_plan, offset_hex  # noqa: E402
from factory_flasher.packaging import desktop_zip_bytes  # noqa: E402
from factory_flasher.session import FactorySession  # noqa: E402


def _port(device: str, serial: str, vid: int = 0x303A) -> UsbPort:
    return UsbPort(
        device=device,
        serial=serial,
        vid=vid,
        pid=0x1001,
        manufacturer="Espressif",
        product="USB JTAG/serial debug unit",
        hwid=f"USB VID:PID=303A:1001 SER={serial}",
    )


def test_layout_matches_restore_script() -> None:
    restore = (ROOT / "scripts" / "hw_restore.sh").read_text()
    snap = (ROOT / "scripts" / "snapshot_factory.sh").read_text()
    restore_l = restore.lower()
    for item in FACTORY_FILES:
        assert offset_hex(item.offset).lower() in restore_l
        assert item.filename in restore
        assert item.filename in snap
    assert "--before default_reset" in restore
    assert "--after hard_reset" in restore
    assert "--chip esp32s3" in restore


def test_esptool_args_order() -> None:
    tmp = Path("/tmp/searaboom-factory-layout-test")
    tmp.mkdir(parents=True, exist_ok=True)
    files = {}
    for item in FACTORY_FILES:
        path = tmp / item.filename
        path.write_bytes(b"x")
        files[item.filename] = path
    args = esptool_write_args("/dev/ttyACM9", files)
    assert args[:6] == ["--chip", "esp32s3", "-p", "/dev/ttyACM9", "-b", "460800"]
    assert "--before" in args and "default_reset" in args
    assert "--after" in args and "hard_reset" in args
    joined = " ".join(args)
    assert "0x0 " in joined or joined.endswith("0x0")
    pos = {item.filename: args.index(str(files[item.filename])) for item in FACTORY_FILES}
    off = {item.filename: args[pos[item.filename] - 1] for item in FACTORY_FILES}
    assert off["bootloader.bin"] == "0x0"
    assert off["partition-table.bin"] == "0x8000"
    assert off["ota_data_initial.bin"] == "0xf000"
    assert off["app.bin"] == "0x20000"
    assert off["storage.bin"] == "0x3a0000"


def test_plan_build_paths() -> None:
    plan = factory_plan(Path("/build"))
    by_key = {r["key"]: r for r in plan}
    assert by_key["app"]["build"] == Path("/build/searaboom.bin")
    assert by_key["app"]["offset"] == 0x20000


def test_progress_and_serial_parsers() -> None:
    assert parse_progress_line("Writing at 0x00020000... (67 %)") == 67
    assert parse_progress_line("hello") is None
    assert parse_version_line("version=0.5.20 kconfig=0.5.20 board=s3-zero") == "0.5.20"
    assert parse_board_line("board=s3-supermini i2s dout=6") == "s3-supermini"
    assert cal_prompt_from_line("cal: hold +") == "hold+"
    assert cal_prompt_from_line("cal: hold -") == "hold-"
    assert cal_prompt_from_line("touch cal ESP_OK") == "done"
    assert cal_prompt_from_line("cal fail: no +") == "fail"


def test_qa_paths_skipped() -> None:
    assert is_qa_device_path("/dev/searaboom-qa-zero-fast")
    ports = [
        _port("/dev/searaboom-qa-zero-fast", "AAA"),
        _port("/dev/ttyACM3", "BBB"),
    ]
    ok = eligible_ports(ports, qa_paths=["/dev/searaboom-qa-zero-fast"])
    assert [p.serial for p in ok] == ["BBB"]


def test_new_ports_after_arm_baseline() -> None:
    a = _port("/dev/ttyACM0", "A")
    b = _port("/dev/ttyACM1", "B")
    assert [p.serial for p in new_ports([a, b], {"A"})] == ["B"]


def test_manifest_offset_guard() -> None:
    meta = {
        "version": "0.5.20",
        "files": [
            {"filename": item.filename, "offset": item.offset}
            for item in FACTORY_FILES
        ],
    }
    validate_manifest(meta)
    bad = json.loads(json.dumps(meta))
    bad["files"][3]["offset"] = 1
    try:
        validate_manifest(bad)
        raise AssertionError("expected offset mismatch")
    except ValueError as exc:
        assert "app.bin" in str(exc)


def test_fetch_image(tmp_path: Path | None = None) -> None:
    dest = Path("/tmp/searaboom-factory-fetch-test")
    dest.mkdir(parents=True, exist_ok=True)
    blobs = {}
    meta = {
        "ready": True,
        "version": "9.9.9",
        "files": [
            {"filename": item.filename, "offset": item.offset, "size": 1}
            for item in FACTORY_FILES
        ],
    }
    blobs["https://example.test/api/factory"] = json.dumps(meta).encode()
    for item in FACTORY_FILES:
        blobs[f"https://example.test/api/factory/{item.filename}"] = b"bin-" + item.key.encode()

    got = fetch_factory_image(
        "https://example.test",
        dest,
        opener=lambda url: blobs[url],
    )
    assert got.name == "9.9.9"
    assert (got / "app.bin").read_bytes() == b"bin-app"


def test_session_disarmed_does_not_flash() -> None:
    flashed = []
    ports = [_port("/dev/ttyACM5", "NEW")]

    session = FactorySession(
        image_dir=Path("/tmp"),
        image_version="0.5.20",
        list_ports=lambda: ports,
        flash=lambda *a: flashed.append(a) or 0,
        serial_cmd=lambda *a: "",
        wait_port=lambda *a: True,
        qa_paths=[],
    )
    session.tick()
    assert flashed == []
    assert session.snapshot()["phase"] == "idle"


def test_session_arm_and_flash_pass() -> None:
    flashed = []
    ports = [_port("/dev/ttyACM5", "BOX1")]

    def serial_cmd(port, command, wait_s):
        if command == "ver":
            return "version=0.5.20 kconfig=0.5.20 board=s3-zero\n"
        if command == "board":
            return "board=s3-zero i2s dout=6\n"
        if command == "touch cal":
            return "cal: hold +\ncal: hold -\ncal done\ntouch cal ESP_OK\n"
        return ""

    def flash(port, image_dir, on_line):
        flashed.append(port)
        on_line("Writing at 0x00020000... (50 %)")
        on_line("Writing at 0x00020000... (100 %)")
        return 0

    session = FactorySession(
        image_dir=Path("/tmp"),
        image_version="0.5.20",
        list_ports=lambda: ports,
        flash=flash,
        serial_cmd=serial_cmd,
        wait_port=lambda *a: True,
        qa_paths=[],
        sleep=lambda _s: None,
    )
    session.arm(True)
    session.tick()
    snap = session.snapshot()
    assert flashed == ["/dev/ttyACM5"]
    assert snap["phase"] == "pass"
    assert snap["progress"] == 100
    assert all(s["status"] == "pass" for s in snap["confirm"])
    assert snap["boxes_done"] == 1


def test_session_flash_fail() -> None:
    session = FactorySession(
        image_dir=Path("/tmp"),
        image_version="0.5.20",
        list_ports=lambda: [_port("/dev/ttyACM5", "BOX2")],
        flash=lambda *a: 1,
        serial_cmd=lambda *a: "",
        wait_port=lambda *a: True,
        qa_paths=[],
        sleep=lambda _s: None,
    )
    session.arm(True)
    session.tick()
    snap = session.snapshot()
    assert snap["phase"] == "fail"
    assert "esptool" in snap["last_error"] or "esptool" in snap["message"].lower() or snap["last_error"]


def test_session_skips_qa_even_if_new() -> None:
    flashed = []
    session = FactorySession(
        image_dir=Path("/tmp"),
        image_version="0.5.20",
        list_ports=lambda: [_port("/dev/searaboom-qa-zero-fast", "QA")],
        flash=lambda *a: flashed.append("nope") or 0,
        serial_cmd=lambda *a: "",
        wait_port=lambda *a: True,
        qa_paths=["/dev/searaboom-qa-zero-fast"],
        sleep=lambda _s: None,
    )
    session.arm(True)
    session.tick()
    assert flashed == []
    assert session.snapshot()["phase"] == "watching"


def test_desktop_zip_contains_runner() -> None:
    data = desktop_zip_bytes()
    zf = zipfile.ZipFile(io.BytesIO(data))
    names = zf.namelist()
    assert "factory_flasher/run.sh" in names
    assert "factory_flasher/layout.py" in names
    assert "factory_flasher/static/index.html" in names
    assert not any("__pycache__" in n for n in names)


def test_list_ports_filters_by_vid() -> None:
    from factory_flasher.core import list_usb_ports

    fake = [
        SimpleNamespace(
            device="/dev/ttyACM0",
            vid=0x303A,
            pid=1,
            serial_number="X",
            manufacturer="Espressif",
            product="USB JTAG",
            hwid="USB VID:PID=303A:0001",
        ),
        SimpleNamespace(
            device="/dev/ttyUSB9",
            vid=0x1234,
            pid=1,
            serial_number="nope",
            manufacturer="Other",
            product="Hub",
            hwid="USB VID:PID=1234:0001",
        ),
    ]
    ports = list_usb_ports(lambda: fake)
    assert [p.device for p in ports] == ["/dev/ttyACM0"]


if __name__ == "__main__":
    test_layout_matches_restore_script()
    test_esptool_args_order()
    test_plan_build_paths()
    test_progress_and_serial_parsers()
    test_qa_paths_skipped()
    test_new_ports_after_arm_baseline()
    test_manifest_offset_guard()
    test_fetch_image()
    test_session_disarmed_does_not_flash()
    test_session_arm_and_flash_pass()
    test_session_flash_fail()
    test_session_skips_qa_even_if_new()
    test_desktop_zip_contains_runner()
    test_list_ports_filters_by_vid()
    print("ok")
