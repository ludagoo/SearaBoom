#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))

from factory_layout import (  # noqa: E402
    FACTORY_FILES,
    esptool_write_args,
    factory_plan,
    offset_hex,
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


def test_layout_go_matches_python() -> None:
    go = (ROOT / "factory_flasher" / "internal" / "layout" / "layout.go").read_text()
    for item in FACTORY_FILES:
        assert f'Filename: "{item.filename}"' in go
        assert f"Offset: {offset_hex(item.offset)}," in go or f"Offset: 0x{item.offset:X}," in go or f"Offset: 0x{item.offset:x}," in go
    assert 'Chip      = "esp32s3"' in go
    assert "Baud      = 460800" in go
    assert 'Before    = "default_reset"' in go
    assert 'After     = "hard_reset"' in go


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


def test_firmware_ignores_old_cal_rev() -> None:
    store_h = (ROOT / "firmware/main/config_store.h").read_text()
    vol = (ROOT / "firmware/main/volume_buttons.c").read_text()
    assert "#define SB_TOUCH_SENS_REV 3" in store_h
    assert "rev < SB_TOUCH_SENS_REV" in vol


def test_udev_copy_matches_scripts() -> None:
    a = (ROOT / "scripts" / "99-searaboom-esp.rules").read_text()
    b = (ROOT / "factory_flasher" / "web" / "99-searaboom-esp.rules").read_text()
    assert a == b


def test_public_page_offers_curl_install_not_zip() -> None:
    html = (ROOT / "server" / "static" / "index.html").read_text()
    assert "desktop.zip" not in html
    assert "./run.sh" not in html
    assert "127.0.0.1:8765" not in html
    assert "esp-web-tools" not in html
    assert "web-tools.json" not in html
    assert "WebSerial" not in html
    assert "Open this page in Chrome" not in html
    assert "Flash a box" not in html
    assert "/api/factory/flasher" in html
    assert "curl -fsSL -o searaboom-factory-flasher-linux-amd64" in html
    assert "chmod +x searaboom-factory-flasher-linux-amd64" in html
    assert html.count("<pre") == 1
    assert "guessFlasherId" in html
    assert "linux-arm64" in html
    assert "windows-amd64" in html
    assert 'id="os"' in html
    assert "Linux x86_64" in html
    assert "<h1>SearaBoom</h1>" in html
    assert "Paste the command to download the TUI." in html
    assert "fetches <code>/api/factory</code> at runtime" in html
    assert "firmware is not in the binary" in html
    assert "0.5." not in html
    assert "any ESP32-S3 plugged into this computer is flashed" not in html
    assert "While ARM" not in html
    assert "esp-web-install" not in html


def test_python_tree_removed() -> None:
    gone = [
        "core.py",
        "session.py",
        "server.py",
        "packaging.py",
        "__main__.py",
        "run.sh",
        "requirements.txt",
        "layout.py",
    ]
    for name in gone:
        assert not (ROOT / "factory_flasher" / name).exists(), name


def test_binary_source_does_not_pin_live_firmware() -> None:
    """Factory PCs always fetch whatever /api/factory is serving."""
    image_go = (ROOT / "factory_flasher" / "internal" / "image" / "image.go").read_text()
    main_go = (ROOT / "factory_flasher" / "main.go").read_text()
    tui_go = (ROOT / "factory_flasher" / "internal" / "tui" / "tui.go").read_text()
    assert 'DefaultFactoryURL = "https://searaboom.goossen.dev"' in image_go
    assert "image.DefaultFactoryURL" in main_go
    assert "0.5.20" not in image_go
    assert "0.5.20" not in main_go
    assert "0.5.20" not in tui_go


def test_flasher_is_tui_not_browser() -> None:
    main_go = (ROOT / "factory_flasher" / "main.go").read_text()
    readme = (ROOT / "factory_flasher" / "README.md").read_text()
    assert "openBrowser" not in main_go
    assert "8765" not in main_go
    assert "127.0.0.1" not in main_go
    assert "httpserver" not in main_go
    assert not (ROOT / "factory_flasher" / "web" / "index.html").exists()
    assert not (ROOT / "factory_flasher" / "internal" / "httpserver").exists()
    assert "terminal UI" in readme or "TUI" in readme
    assert "curl -fsSL" in readme
    assert "127.0.0.1:8765" not in readme


def test_flasher_api(tmp_path: Path | None = None) -> None:
    dest = Path("/tmp/searaboom-flasher-api-test")
    dest.mkdir(parents=True, exist_ok=True)
    payload = b"fake-linux-amd64-binary"
    bin_path = dest / "searaboom-factory-flasher-linux-amd64"
    bin_path.write_bytes(payload)
    os.environ["SEARABOOM_FLASHER_DIR"] = str(dest)
    sys.path.insert(0, str(ROOT / "server"))
    import app as searaboom_app  # noqa: WPS433

    client = searaboom_app.app.test_client()
    cat = client.get("/api/factory/flasher").get_json()
    assert cat["tool"] == "searaboom-factory-flasher"
    ids = [d["id"] for d in cat["downloads"]]
    assert ids == [
        "linux-amd64",
        "linux-arm64",
        "windows-amd64",
        "darwin-amd64",
        "darwin-arm64",
    ]
    linux = next(d for d in cat["downloads"] if d["id"] == "linux-amd64")
    assert linux["ready"] is True
    assert linux["sha256"] == hashlib.sha256(payload).hexdigest()
    win = next(d for d in cat["downloads"] if d["id"] == "windows-amd64")
    assert win["ready"] is False

    gone = client.get("/api/factory/desktop.zip")
    assert gone.status_code == 410
    assert "flasher" in gone.get_json()

    dl = client.get("/api/factory/flasher/linux-amd64")
    assert dl.status_code == 200
    assert dl.data == payload

    missing = client.get("/api/factory/flasher/windows-amd64")
    assert missing.status_code == 404

    fac = client.get("/api/factory").get_json()
    assert "flasher" in fac
    assert fac["flasher"]["downloads"][0]["id"] == "linux-amd64"


if __name__ == "__main__":
    test_layout_matches_restore_script()
    test_layout_go_matches_python()
    test_esptool_args_order()
    test_plan_build_paths()
    test_firmware_ignores_old_cal_rev()
    test_udev_copy_matches_scripts()
    test_public_page_offers_curl_install_not_zip()
    test_python_tree_removed()
    test_binary_source_does_not_pin_live_firmware()
    test_flasher_is_tui_not_browser()
    test_flasher_api()
    print("ok")
