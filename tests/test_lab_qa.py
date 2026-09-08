#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from lab_parse import needs_firmware_qa, parse_hw_comment


def test_parse_hw_comment() -> None:
    assert parse_hw_comment("/hw-test listen please") == "listen"
    assert parse_hw_comment("/hw-test hands") == "hands"
    assert parse_hw_comment("/hw-test soak") == "soak"
    assert parse_hw_comment("/hw-test") == "unattended"
    assert parse_hw_comment("looks good") is None
    assert parse_hw_comment("") is None


def test_needs_firmware_qa() -> None:
    assert needs_firmware_qa(["firmware/main/main.c"])
    assert needs_firmware_qa(["server/app.py", "firmware/clips/x.aac"])
    assert not needs_firmware_qa(["README.md", "server/app.py"])
    assert not needs_firmware_qa([])


if __name__ == "__main__":
    test_parse_hw_comment()
    test_needs_firmware_qa()
    print("ok")
