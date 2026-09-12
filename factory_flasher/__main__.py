#!/usr/bin/env python3
"""SearaBoom factory desktop flasher.

  python3 -m factory_flasher
  python3 -m factory_flasher --demo
  python3 -m factory_flasher --write-zip
  python3 -m factory_flasher --image-dir /path/to/factory
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from factory_flasher.core import (
    DEFAULT_FACTORY_URL,
    UsbPort,
    load_manifest,
    resolve_image_dir,
)
from factory_flasher.packaging import desktop_zip_bytes
from factory_flasher.server import run_poll_loop, serve
from factory_flasher.session import FactorySession


def _demo_ports() -> list[UsbPort]:
    return [
        UsbPort(
            device="/dev/ttyACM-demo",
            serial="DEMOBOX",
            vid=0x303A,
            pid=0x1001,
            manufacturer="Espressif",
            product="USB JTAG/serial debug unit",
            hwid="USB VID:PID=303A:1001 SER=DEMOBOX",
        )
    ]


def _demo_flash(port: str, image_dir: Path, on_line) -> int:
    steps = [
        "esptool.py v4.12.0",
        "Chip is ESP32-S3",
        "Configuring flash size...",
        "Writing at 0x00000000... (12 %)",
        "Writing at 0x00008000... (28 %)",
        "Writing at 0x0000f000... (41 %)",
        "Writing at 0x00020000... (67 %)",
        "Writing at 0x003a0000... (100 %)",
        "Hash of data verified.",
        "Hard resetting via RTS pin...",
    ]
    for line in steps:
        on_line(line)
        time.sleep(0.05)
    return 0


def _demo_serial(port: str, command: str, wait_s: float) -> str:
    cmd = (command or "").strip().lower()
    if cmd == "ver":
        return "version=0.5.20 kconfig=0.5.20 board=s3-zero\n"
    if cmd == "board":
        return "board=s3-zero i2s dout=6 bclk=7 ws=8 vol+=3 vol-=2 led=21+48 rise21=0 rise47=0 rise48=0 pulled_low=none\n"
    if cmd == "touch cal":
        time.sleep(0.05)
        return (
            "touch cal: hold + then -\n"
            "cal: hands off\n"
            "cal: hold +\n"
            "cal: + peak=0.180\n"
            "cal: hold -\n"
            "cal: - peak=0.175\n"
            "cal done vol+=0.144 vol-=0.140\n"
            "touch cal ESP_OK\n"
        )
    return ""


def image_version(image_dir: Path) -> str:
    meta_path = Path(image_dir) / "factory.json"
    if meta_path.is_file():
        try:
            return str(load_manifest(meta_path).get("version") or "")
        except (OSError, ValueError, json.JSONDecodeError):
            return ""
    return ""


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="SearaBoom factory desktop flasher")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--factory-url", default=DEFAULT_FACTORY_URL)
    p.add_argument("--image-dir", type=Path, default=None)
    p.add_argument("--no-download", action="store_true")
    p.add_argument("--no-browser", action="store_true")
    p.add_argument(
        "--demo",
        action="store_true",
        help="UI walkthrough without USB hardware (does not flash)",
    )
    p.add_argument(
        "--write-zip",
        nargs="?",
        const="searaboom-factory-flasher.zip",
        type=Path,
        help="Write the operator zip and exit (does not start the UI or flash)",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.write_zip:
        dest = Path(args.write_zip)
        dest.write_bytes(desktop_zip_bytes())
        print(str(dest.resolve()))
        return 0
    if args.demo:
        image_dir = args.image_dir or Path("/tmp/searaboom-factory-demo")
        image_dir.mkdir(parents=True, exist_ok=True)
        version = "0.5.20"
        session = FactorySession(
            image_dir=image_dir,
            image_version=version,
            list_ports=_demo_ports,
            flash=_demo_flash,
            serial_cmd=_demo_serial,
            wait_port=lambda device, timeout: True,
            qa_paths=[],
            sleep=lambda _s: None,
        )
        print("DEMO mode — no USB writes.", file=sys.stderr)
    else:
        try:
            image_dir = resolve_image_dir(
                image_dir=args.image_dir,
                factory_url=args.factory_url,
                download=not args.no_download,
            )
        except Exception as exc:
            print(f"factory image: {exc}", file=sys.stderr)
            return 2
        session = FactorySession(
            image_dir=image_dir,
            image_version=image_version(image_dir),
        )
        print(f"factory image {session._state.image_version or '?'} at {image_dir}", file=sys.stderr)

    stop_poll = run_poll_loop(session)
    httpd = serve(
        session,
        host=args.host,
        port=args.port,
        open_browser=not args.no_browser,
    )
    url = f"http://{args.host}:{args.port}/"
    print(f"factory flasher UI {url}", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("stopping", file=sys.stderr)
    finally:
        stop_poll.set()
        session.stop()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
