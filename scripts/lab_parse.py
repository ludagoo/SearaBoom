"""PR comment + path helpers (no Origin/crypto deps)."""
from __future__ import annotations

FIRMWARE_PREFIXES = ("firmware/",)


def parse_hw_comment(body: str) -> str | None:
    text = (body or "").strip()
    if not text:
        return None
    line = text.splitlines()[0].strip().lower()
    if line.startswith("/hw-test listen"):
        return "listen"
    if line.startswith("/hw-test hands"):
        return "hands"
    if line.startswith("/hw-test soak"):
        return "soak"
    if line == "/hw-test" or line.startswith("/hw-test"):
        return "unattended"
    return None


def needs_firmware_qa(paths: list[str]) -> bool:
    for raw in paths:
        p = str(raw).replace("\\", "/").lstrip("./")
        if p.startswith(FIRMWARE_PREFIXES) or p == "firmware":
            return True
    return False
