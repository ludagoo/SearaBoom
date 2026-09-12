"""Port detect, factory image fetch, esptool flash, and serial confirm."""
from __future__ import annotations

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

from factory_flasher.layout import (
    FACTORY_FILES,
    USB_VENDOR_IDS,
    esptool_write_args,
    file_map,
    slot_ready,
)

DEFAULT_FACTORY_URL = os.environ.get(
    "SEARABOOM_FACTORY_URL", "https://searaboom.goossen.dev"
).rstrip("/")
CACHE_DIR = Path(
    os.environ.get(
        "SEARABOOM_FACTORY_CACHE",
        str(Path.home() / ".cache" / "searaboom" / "factory"),
    )
)
QA_CONFIG = Path(
    os.environ.get(
        "SEARABOOM_QA_BOXES",
        str(Path.home() / ".config" / "searaboom" / "qa-boxes.json"),
    )
)

PROGRESS_RE = re.compile(r"\((\d+)\s*%\)")
VER_RE = re.compile(r"version=([0-9]+\.[0-9]+\.[0-9]+)")
BOARD_RE = re.compile(r"board=([^\s]+)")
CAL_HOLD_PLUS = re.compile(r"^cal:\s*hold\s*\+", re.I)
CAL_HOLD_MINUS = re.compile(r"^cal:\s*hold\s*-", re.I)
CAL_HANDS_OFF = re.compile(r"^cal:\s*hands off", re.I)
CAL_DONE = re.compile(r"^cal done|touch cal ESP_OK", re.I)
CAL_FAIL = re.compile(r"^cal fail|touch cal ESP_FAIL", re.I)
CAL_CEILING_S = 110.0

FlashLineCb = Callable[[str], None]


@dataclass(frozen=True)
class UsbPort:
    device: str
    serial: str
    vid: int
    pid: int
    manufacturer: str
    product: str
    hwid: str

    @property
    def identity(self) -> str:
        if self.serial:
            return self.serial
        return self.hwid or self.device


def parse_progress_line(line: str) -> int | None:
    m = PROGRESS_RE.search(line or "")
    if not m:
        return None
    return max(0, min(100, int(m.group(1))))


def parse_version_line(line: str) -> str | None:
    m = VER_RE.search(line or "")
    return m.group(1) if m else None


def parse_board_line(line: str) -> str | None:
    m = BOARD_RE.search(line or "")
    return m.group(1) if m else None


def cal_prompt_from_line(line: str) -> str | None:
    raw = line or ""
    if "\n" in raw:
        last = None
        for part in raw.splitlines():
            p = cal_prompt_from_line(part)
            if p:
                last = p
        return last
    text = raw.strip()
    if CAL_FAIL.search(text):
        return "fail"
    if CAL_DONE.search(text):
        return "done"
    if CAL_HOLD_PLUS.search(text):
        return "hold+"
    if CAL_HOLD_MINUS.search(text):
        return "hold-"
    if CAL_HANDS_OFF.search(text):
        return "hands-off"
    return None


def cal_terminal(text: str) -> str | None:
    last = None
    for part in (text or "").splitlines():
        prompt = cal_prompt_from_line(part)
        if prompt in ("done", "fail"):
            last = prompt
    return last


def norm_usb_serial(serial: str) -> str:
    return (serial or "").replace(":", "").replace("-", "").lower()


def qa_usb_serials() -> set[str]:
    found: set[str] = set()
    if not QA_CONFIG.is_file():
        return found
    try:
        data = json.loads(QA_CONFIG.read_text())
    except (OSError, json.JSONDecodeError):
        return found
    for rec in (data.get("boxes") or {}).values():
        serial = (rec.get("usb_serial") or "").strip()
        if serial:
            found.add(norm_usb_serial(serial))
    return found


def is_qa_device_path(device: str) -> bool:
    text = str(device or "")
    try:
        real = str(Path(text).resolve())
    except OSError:
        real = text
    if "searaboom-qa" in text or "searaboom-qa" in real:
        return True
    return False


def qa_device_paths() -> set[str]:
    found: set[str] = set()
    dev = Path("/dev")
    if dev.is_dir():
        for path in dev.glob("searaboom-qa-*"):
            found.add(str(path))
            try:
                found.add(str(path.resolve()))
            except OSError:
                pass
    if QA_CONFIG.is_file():
        try:
            data = json.loads(QA_CONFIG.read_text())
        except (OSError, json.JSONDecodeError):
            data = {}
        for rec in (data.get("boxes") or {}).values():
            serial = (rec.get("usb_serial") or "").strip()
            if not serial:
                continue
            by_id = Path("/dev/serial/by-id") / (
                f"usb-Espressif_USB_JTAG_serial_debug_unit_{serial}-if00"
            )
            if by_id.exists():
                found.add(str(by_id))
                try:
                    found.add(str(by_id.resolve()))
                except OSError:
                    pass
    return found


def is_searaboom_port(port: UsbPort) -> bool:
    if port.vid in USB_VENDOR_IDS:
        return True
    blob = f"{port.manufacturer} {port.product} {port.hwid}".lower()
    return "espressif" in blob or "usb jtag" in blob or "cp210" in blob or "ch340" in blob


def list_usb_ports(lister: Callable[[], Iterable] | None = None) -> list[UsbPort]:
    if lister is None:
        from serial.tools import list_ports

        lister = list_ports.comports
    out: list[UsbPort] = []
    for info in lister():
        device = getattr(info, "device", "") or ""
        vid = int(getattr(info, "vid", None) or 0)
        pid = int(getattr(info, "pid", None) or 0)
        port = UsbPort(
            device=device,
            serial=str(getattr(info, "serial_number", None) or ""),
            vid=vid,
            pid=pid,
            manufacturer=str(getattr(info, "manufacturer", None) or ""),
            product=str(getattr(info, "product", None) or ""),
            hwid=str(getattr(info, "hwid", None) or ""),
        )
        if is_searaboom_port(port):
            out.append(port)
    return out


def eligible_ports(
    ports: Iterable[UsbPort],
    *,
    qa_paths: Iterable[str] | None = None,
    qa_serials: Iterable[str] | None = None,
) -> list[UsbPort]:
    blocked: set[str] = set()
    if qa_paths is None:
        source = list(qa_device_paths())
        blocked_serials = qa_usb_serials()
    else:
        source = list(qa_paths)
        blocked_serials = {norm_usb_serial(s) for s in (qa_serials or []) if s}
    for raw in source:
        blocked.add(raw)
        try:
            path = Path(raw)
            if path.exists():
                blocked.add(str(path.resolve()))
        except OSError:
            pass
    out: list[UsbPort] = []
    for port in ports:
        if is_qa_device_path(port.device):
            continue
        try:
            real = str(Path(port.device).resolve())
        except OSError:
            real = port.device
        if port.device in blocked or real in blocked:
            continue
        if port.serial and norm_usb_serial(port.serial) in blocked_serials:
            continue
        out.append(port)
    return out


def new_ports(current: Iterable[UsbPort], baseline_ids: set[str]) -> list[UsbPort]:
    return [p for p in current if p.identity not in baseline_ids]


def load_manifest(path: Path) -> dict:
    data = json.loads(Path(path).read_text())
    if not isinstance(data, dict):
        raise ValueError("factory.json is not an object")
    return data


def validate_manifest(meta: dict) -> None:
    files = meta.get("files") or []
    by_name = {item.get("filename"): item for item in files if isinstance(item, dict)}
    for item in FACTORY_FILES:
        rec = by_name.get(item.filename)
        if not rec:
            raise ValueError(f"factory.json missing {item.filename}")
        offset = rec.get("offset")
        if offset is not None and int(offset) != item.offset:
            raise ValueError(
                f"{item.filename} offset {offset} != expected {item.offset}"
            )


def discover_local_slot(explicit: Path | None = None) -> Path | None:
    if explicit and slot_ready(explicit):
        return Path(explicit)
    env = os.environ.get("SEARABOOM_FACTORY_DIR")
    if env and slot_ready(env):
        return Path(env)
    here = Path(__file__).resolve().parents[1]
    local = here / "server" / "firmware" / "factory"
    if slot_ready(local):
        return local
    return None


def fetch_factory_image(
    base_url: str,
    dest_root: Path | None = None,
    *,
    opener: Callable[[str], bytes] | None = None,
) -> Path:
    dest_root = dest_root or CACHE_DIR
    base = (base_url or DEFAULT_FACTORY_URL).rstrip("/")
    get = opener or (lambda url: urllib.request.urlopen(url, timeout=60).read())
    raw = get(f"{base}/api/factory")
    meta = json.loads(raw.decode("utf-8") if isinstance(raw, (bytes, bytearray)) else raw)
    if not meta.get("ready"):
        raise RuntimeError("factory image is not ready on the server")
    validate_manifest(meta)
    version = str(meta.get("version") or "unknown")
    dest = Path(dest_root) / version
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "factory.json").write_text(json.dumps(meta, indent=2) + "\n")
    for item in FACTORY_FILES:
        target = dest / item.filename
        url = f"{base}/api/factory/{item.filename}"
        target.write_bytes(get(url))
    if not slot_ready(dest):
        raise RuntimeError(f"incomplete factory download in {dest}")
    return dest


def resolve_image_dir(
    *,
    image_dir: Path | None = None,
    factory_url: str = DEFAULT_FACTORY_URL,
    download: bool = True,
) -> Path:
    local = discover_local_slot(image_dir)
    if local:
        meta_path = local / "factory.json"
        if meta_path.is_file():
            validate_manifest(load_manifest(meta_path))
        return local
    if not download:
        raise FileNotFoundError("no local factory image")
    return fetch_factory_image(factory_url)


def esptool_python() -> str:
    override = os.environ.get("SEARABOOM_ESPTOOL_PYTHON")
    if override:
        return override
    return sys.executable


def flash_port(
    port: str,
    image_dir: Path,
    *,
    on_line: FlashLineCb | None = None,
    runner: Callable[..., int] | None = None,
) -> int:
    args = [esptool_python(), "-u", "-m", "esptool", *esptool_write_args(port, file_map(image_dir))]
    if on_line:
        on_line(" ".join(args))
    if runner is not None:
        return int(runner(args, on_line))
    import subprocess

    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        text = line.rstrip("\n")
        if on_line:
            on_line(text)
    return int(proc.wait())


def wait_for_port(device: str, timeout_s: float = 10.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if Path(device).exists():
            return True
        time.sleep(0.2)
    return Path(device).exists()


def collect_serial_output(
    read_chunk: Callable[[], str],
    *,
    now: Callable[[], float],
    deadline: float,
    stop_when: Callable[[str], bool] | None = None,
    on_line: FlashLineCb | None = None,
    idle_sleep: Callable[[float], None] | None = None,
    idle_s: float = 0.05,
) -> str:
    """Read until stop_when(acc) or deadline. Returns early on a terminal line."""
    sleep = idle_sleep or time.sleep
    acc = ""
    while now() < deadline:
        chunk = read_chunk() or ""
        if chunk:
            acc += chunk
            if on_line:
                for line in chunk.splitlines():
                    on_line(line)
            if stop_when and stop_when(acc):
                break
        else:
            if now() >= deadline:
                break
            sleep(idle_s)
    return acc


def serial_command(
    port: str,
    command: str,
    wait_s: float = 1.5,
    *,
    on_line: FlashLineCb | None = None,
    stop_when: Callable[[str], bool] | None = None,
    exchange: Callable[[str, str, float], str] | None = None,
) -> str:
    if exchange is not None:
        text = exchange(port, command, wait_s)
        if on_line:
            for line in text.splitlines():
                on_line(line)
        return text
    import serial

    ser = serial.Serial(port, 115200, timeout=0.4)
    try:
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write((command + "\n").encode())
        ser.flush()

        def read_chunk() -> str:
            data = ser.read(4096)
            if not data:
                return ""
            return data.decode("utf-8", "replace")

        return collect_serial_output(
            read_chunk,
            now=time.time,
            deadline=time.time() + wait_s,
            stop_when=stop_when,
            on_line=on_line,
            idle_sleep=lambda _s: None,
            idle_s=0.0,
        )
    finally:
        ser.close()
