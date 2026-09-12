"""Zip the desktop flasher so operators can download it from the factory page."""
from __future__ import annotations

import io
import zipfile
from pathlib import Path

PKG_DIR = Path(__file__).resolve().parent
SKIP_DIR_NAMES = {".venv", "__pycache__", ".git"}
SKIP_SUFFIXES = {".pyc", ".pyo"}


def iter_flasher_files(root: Path | None = None) -> list[tuple[Path, str]]:
    root = Path(root or PKG_DIR)
    out: list[tuple[Path, str]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if any(part in SKIP_DIR_NAMES for part in path.parts):
            continue
        if path.suffix in SKIP_SUFFIXES:
            continue
        rel = path.relative_to(root)
        out.append((path, str(Path("factory_flasher") / rel)))
    return out


def desktop_zip_bytes() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path, arc in iter_flasher_files():
            zf.write(path, arcname=arc)
        readme = PKG_DIR / "README.md"
        if readme.is_file():
            zf.writestr(
                "README.md",
                "Unpack, then:\n"
                "  cd factory_flasher\n"
                "  ./install-serial-linux.sh   # once per factory PC\n"
                "  ./run.sh\n\n"
                + readme.read_text(),
            )
    return buf.getvalue()
