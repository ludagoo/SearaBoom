#!/usr/bin/env python3
"""Resolve dedicated QA USB boxes. Config: ~/.config/searaboom/qa-boxes.json."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

CONFIG_PATH = Path(
    os.environ.get(
        "SEARABOOM_QA_BOXES",
        str(Path.home() / ".config/searaboom/qa-boxes.json"),
    )
)
KNOWN_IDS = ("zero-fast", "supermini-fast", "zero-soak", "supermini-soak")
LOCK_DIR = Path(os.environ.get("SEARABOOM_QA_LOCK_DIR", "/tmp/searaboom-qa-locks"))


def load_config() -> dict:
    if not CONFIG_PATH.is_file():
        return {
            "quiet_volume": 1,
            "listen_box": "zero-fast",
            "boxes": {i: {"usb_serial": "", "hw": "", "role": ""} for i in KNOWN_IDS},
        }
    data = json.loads(CONFIG_PATH.read_text())
    data.setdefault("quiet_volume", 1)
    data.setdefault("listen_box", "zero-fast")
    data.setdefault("boxes", {})
    return data


def box_ids_for_job(job: str) -> list[str]:
    cfg = load_config()
    if job == "soak":
        return ["zero-soak", "supermini-soak"]
    if job == "listen":
        return [cfg.get("listen_box") or "zero-fast"]
    if job in ("unattended", "hands", "fast"):
        return ["zero-fast", "supermini-fast"]
    raise SystemExit(f"unknown job {job}")


def box_record(box_id: str) -> dict:
    cfg = load_config()
    rec = cfg.get("boxes", {}).get(box_id)
    if not rec:
        raise SystemExit(f"unknown box {box_id}")
    return rec


def serial_by_id_path(usb_serial: str) -> Path | None:
    if not usb_serial:
        return None
    p = Path("/dev/serial/by-id") / f"usb-Espressif_USB_JTAG_serial_debug_unit_{usb_serial}-if00"
    if p.exists():
        return p
    return None


def device_path(box_id: str) -> Path | None:
    symlink = Path(f"/dev/searaboom-qa-{box_id}")
    if symlink.exists():
        return symlink
    rec = box_record(box_id)
    serial = (rec.get("usb_serial") or "").strip()
    if not serial:
        return None
    return serial_by_id_path(serial)


def lock_path(box_id: str) -> Path:
    LOCK_DIR.mkdir(parents=True, exist_ok=True)
    return LOCK_DIR / f"{box_id}.lock"


def udev_rules() -> str:
    cfg = load_config()
    lines = [
        "# Generated from qa-boxes.json — dedicated SearaBoom QA fixtures.",
        "# Do not flash these from an interactive agent.",
        "",
    ]
    for box_id, rec in cfg.get("boxes", {}).items():
        serial = (rec.get("usb_serial") or "").strip()
        if not serial:
            continue
        lines.append(
            f'SUBSYSTEM=="tty", ATTRS{{idVendor}}=="303a", ATTRS{{serial}}=="{serial}", '
            f'SYMLINK+="searaboom-qa-{box_id}", MODE="0666", GROUP="uucp", TAG+="uaccess"'
        )
    lines.append("")
    return "\n".join(lines)


def status() -> dict:
    cfg = load_config()
    boxes = {}
    for box_id, rec in cfg.get("boxes", {}).items():
        try:
            path = device_path(box_id)
        except SystemExit:
            path = None
        boxes[box_id] = {
            "id": box_id,
            "usb_serial": rec.get("usb_serial") or "",
            "hw": rec.get("hw") or "",
            "role": rec.get("role") or "",
            "note": rec.get("note") or "",
            "configured": bool((rec.get("usb_serial") or "").strip()),
            "device": str(path) if path else None,
            "present": bool(path and path.exists()),
            "symlink": f"/dev/searaboom-qa-{box_id}",
        }
    return {
        "ok": True,
        "config": str(CONFIG_PATH),
        "quiet_volume": cfg.get("quiet_volume", 1),
        "listen_box": cfg.get("listen_box"),
        "boxes": boxes,
    }


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(
            "usage: qa_boxes.py path <id> | lock <id> | job <job> | status | udev | config",
            file=sys.stderr,
        )
        return 2
    cmd = argv[1]
    if cmd == "config":
        print(CONFIG_PATH)
        return 0
    if cmd == "status":
        json.dump(status(), sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0
    if cmd == "udev":
        sys.stdout.write(udev_rules())
        return 0
    if cmd == "job":
        if len(argv) < 3:
            return 2
        print("\n".join(box_ids_for_job(argv[2])))
        return 0
    if cmd in ("path", "lock") and len(argv) >= 3:
        box_id = argv[2]
        if cmd == "lock":
            print(lock_path(box_id))
            return 0
        path = device_path(box_id)
        rec = box_record(box_id)
        if not (rec.get("usb_serial") or "").strip():
            print(f"box {box_id} has no usb_serial in {CONFIG_PATH}", file=sys.stderr)
            return 3
        if not path:
            print(f"box {box_id} unplugged (serial {rec.get('usb_serial')})", file=sys.stderr)
            return 3
        print(path)
        return 0
    print("unknown command", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
