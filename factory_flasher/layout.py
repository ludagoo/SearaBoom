"""USB factory flash layout.

Must stay aligned with firmware/build/flasher_args.json, scripts/hw_restore.sh,
and scripts/snapshot_factory.sh. The desktop flasher and the server both import this.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

CHIP = "esp32s3"
BAUD = 460800
BEFORE = "default_reset"
AFTER = "hard_reset"
FLASH_MODE = "dio"
FLASH_FREQ = "80m"
FLASH_SIZE = "4MB"

# Espressif USB-JTAG/serial, Silicon Labs CP210x, WCH CH340 (see udev rules).
USB_VENDOR_IDS = (0x303A, 0x10C4, 0x1A86)


@dataclass(frozen=True)
class FactoryFile:
    key: str
    offset: int
    filename: str
    build_rel: str


FACTORY_FILES: tuple[FactoryFile, ...] = (
    FactoryFile("bootloader", 0x0, "bootloader.bin", "bootloader/bootloader.bin"),
    FactoryFile("partitions", 0x8000, "partition-table.bin", "partition_table/partition-table.bin"),
    FactoryFile("otadata", 0xF000, "ota_data_initial.bin", "ota_data_initial.bin"),
    FactoryFile("app", 0x20000, "app.bin", "searaboom.bin"),
    FactoryFile("storage", 0x3A0000, "storage.bin", "storage.bin"),
)

REQUIRED_FILENAMES = tuple(item.filename for item in FACTORY_FILES)


def factory_plan(build_dir: Path | None = None) -> list[dict]:
    plan: list[dict] = []
    for item in FACTORY_FILES:
        rec: dict = {
            "key": item.key,
            "offset": item.offset,
            "filename": item.filename,
        }
        if build_dir is not None:
            rec["build"] = Path(build_dir) / item.build_rel
        plan.append(rec)
    return plan


def offset_hex(offset: int) -> str:
    return hex(offset)


def esptool_write_args(port: str, file_paths: dict[str, Path]) -> list[str]:
    """Args after `python -m esptool`. Matches scripts/hw_restore.sh."""
    missing = [name for name in REQUIRED_FILENAMES if name not in file_paths]
    if missing:
        raise FileNotFoundError(", ".join(missing))
    cmd = [
        "--chip", CHIP,
        "-p", port,
        "-b", str(BAUD),
        "--before", BEFORE,
        "--after", AFTER,
        "write_flash",
        "--flash_mode", FLASH_MODE,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", FLASH_SIZE,
    ]
    for item in FACTORY_FILES:
        path = Path(file_paths[item.filename])
        if not path.is_file():
            raise FileNotFoundError(item.filename)
        cmd.extend([offset_hex(item.offset), str(path)])
    return cmd


def slot_ready(directory: Path) -> bool:
    return all((Path(directory) / name).is_file() for name in REQUIRED_FILENAMES)


def file_map(directory: Path) -> dict[str, Path]:
    directory = Path(directory)
    mapping = {name: directory / name for name in REQUIRED_FILENAMES}
    missing = [name for name, path in mapping.items() if not path.is_file()]
    if missing:
        raise FileNotFoundError(", ".join(missing))
    return mapping
