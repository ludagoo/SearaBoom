#!/usr/bin/env python3
"""Host spec for radio HTTP ringbuffer start / recover watermarks.

Live 0.5.25 on Lucas QA Zero (d0:cf:13:07:de:fc):
  stall rb-drop rb≈98–101kB / 262144 need=131072 every ~200–300 ms.

Root cause encoded here:
  mixer consume ≈ Icecast produce, so fill hovers where play started.
  Start/resume at ~100–128 KB therefore sits on the 128 KB rb-drop line.

Firmware policy lives in firmware/main/radio_buf.h (compiled below).
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware" / "main" / "radio_buf.h"

# Historical (0.5.25 / PR #3-era) numbers the live box is hitting.
OLD_START_HOVER = 100 * 1024  # post-ident prefetch, then go_live immediately
OLD_NEED = 128 * 1024  # SB_HTTP_SLOW_RESUME_BYTES — need=131072 in logs
OLD_RB_DROP_LINE = 128 * 1024
CAPACITY = 256 * 1024


def parse_c_int_defines(path: Path) -> dict[str, int]:
    text = path.read_text()
    defs: dict[str, int] = {}
    # Resolve one-level aliases after collecting literals.
    lit = re.compile(
        r"^#define\s+(SB_[A-Z0-9_]+|RADIO_BUF_[A-Z0-9_]+)\s+"
        r"(?:\((\d+)\s*\*\s*(\d+)\)|(\d+)|([A-Z0-9_]+))\s*$",
        re.M,
    )
    pending: list[tuple[str, str]] = []
    for m in lit.finditer(text):
        name, a, b, n, alias = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if a and b:
            defs[name] = int(a) * int(b)
        elif n:
            defs[name] = int(n)
        elif alias:
            pending.append((name, alias))
    for name, alias in pending:
        assert alias in defs, f"{name} aliases unknown {alias}"
        defs[name] = defs[alias]
    return defs


def test_header_numbers() -> dict[str, int]:
    d = parse_c_int_defines(HEADER)
    assert d["SB_HTTP_RB_SIZE"] == CAPACITY
    assert d["SB_HTTP_START_BYTES"] == 224 * 1024
    assert d["SB_HTTP_RECOVER_BYTES"] == 224 * 1024
    assert d["SB_HTTP_UNDERRUN_BYTES"] == OLD_RB_DROP_LINE
    assert d["SB_HTTP_SLOW_RESUME_BYTES"] == d["SB_HTTP_RECOVER_BYTES"]
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == d["SB_HTTP_RECOVER_BYTES"]
    assert d["SB_WIFI_HTTP_LOW_BYTES"] == d["SB_HTTP_UNDERRUN_BYTES"]
    # Almost full: at least 7/8 of the 256 KB rb, but leave writer slack.
    assert d["SB_HTTP_START_BYTES"] * 8 >= d["SB_HTTP_RB_SIZE"] * 7
    assert d["SB_HTTP_START_BYTES"] < d["SB_HTTP_RB_SIZE"]
    assert d["SB_HTTP_RECOVER_BYTES"] > OLD_NEED
    assert d["SB_HTTP_RECOVER_BYTES"] > d["SB_HTTP_UNDERRUN_BYTES"]
    return d


def test_python_hysteresis(d: dict[str, int]) -> None:
    start = d["SB_HTTP_START_BYTES"]
    recover = d["SB_HTTP_RECOVER_BYTES"]
    underrun = d["SB_HTTP_UNDERRUN_BYTES"]

    # Live hover must not start audible play.
    assert OLD_START_HOVER < start
    assert OLD_NEED < recover

    def gate(now: int, filled: int) -> int:
        if now == d["RADIO_BUF_PLAY"]:
            return d["RADIO_BUF_REFILL"] if 0 <= filled < underrun else d["RADIO_BUF_PLAY"]
        if now == d["RADIO_BUF_REFILL"]:
            return d["RADIO_BUF_PLAY"] if filled >= recover else d["RADIO_BUF_REFILL"]
        return d["RADIO_BUF_PLAY"] if filled >= start else d["RADIO_BUF_WAIT"]

    # Boot: 100 KB after ident stays silent; 224 KB goes live.
    st = d["RADIO_BUF_WAIT"]
    for filled in (0, 64 * 1024, OLD_START_HOVER, OLD_NEED, start - 1):
        st = gate(st, filled)
        assert st == d["RADIO_BUF_WAIT"], f"started too early at {filled}"
    st = gate(st, start)
    assert st == d["RADIO_BUF_PLAY"]

    # Playing at ~100 KB (the live storm) must drop into refill, not stay PLAY.
    st = gate(d["RADIO_BUF_PLAY"], OLD_START_HOVER)
    assert st == d["RADIO_BUF_REFILL"]
    # Old resume watermark is still skinny — stay muted.
    st = gate(st, OLD_NEED)
    assert st == d["RADIO_BUF_REFILL"], "must not resume at need=131072"
    st = gate(st, recover)
    assert st == d["RADIO_BUF_PLAY"]

    # Healthy hover just under full stays PLAY (above the 128 KB rb-drop line).
    for filled in (recover, 200 * 1024, underrun):
        st = gate(d["RADIO_BUF_PLAY"], filled)
        assert st == d["RADIO_BUF_PLAY"], f"false underrun at {filled}"


def test_c_gate_matches(d: dict[str, int]) -> None:
    harness = r"""
#include <stdio.h>
#include "radio_buf.h"
int main(void) {
    printf("cap=%d start=%d recover=%d underrun=%d need0=%d need1=%d\n",
           SB_HTTP_RB_SIZE, SB_HTTP_START_BYTES, SB_HTTP_RECOVER_BYTES,
           SB_HTTP_UNDERRUN_BYTES, radio_buf_play_need(0), radio_buf_play_need(1));
    int samples[] = {0, 65536, 102400, 131072, 204800, 229376, 262144};
    const char *names[] = {"wait", "play", "refill"};
    for (int n = 0; n < 3; n++) {
        for (int i = 0; i < (int)(sizeof(samples)/sizeof(samples[0])); i++) {
            printf("%s %d -> %d\n", names[n], samples[i],
                   radio_buf_gate(n, samples[i]));
        }
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        (tdir / "harness.c").write_text(harness)
        bin_path = tdir / "test_radio_buf"
        subprocess.check_call(
            [
                "gcc", "-O0", "-Wall", "-Werror",
                "-I", str(HEADER.parent),
                str(tdir / "harness.c"),
                "-o", str(bin_path),
            ]
        )
        out = subprocess.check_output([str(bin_path)], text=True)
    lines = out.strip().splitlines()
    hdr = lines[0]
    assert f"cap={CAPACITY}" in hdr
    assert f"start={d['SB_HTTP_START_BYTES']}" in hdr
    assert f"recover={d['SB_HTTP_RECOVER_BYTES']}" in hdr
    assert f"underrun={d['SB_HTTP_UNDERRUN_BYTES']}" in hdr
    assert f"need0={d['SB_HTTP_RECOVER_BYTES']}" in hdr
    assert f"need1={d['SB_WIFI_HTTP_RESUME_BYTES']}" in hdr

    name_to_now = {"wait": 0, "play": 1, "refill": 2}
    for line in lines[1:]:
        name, rest = line.split(" ", 1)
        filled_s, got_s = rest.split(" -> ")
        filled = int(filled_s)
        got = int(got_s)
        now = name_to_now[name]
        if now == 1:
            want = 2 if 0 <= filled < d["SB_HTTP_UNDERRUN_BYTES"] else 1
        elif now == 2:
            want = 1 if filled >= d["SB_HTTP_RECOVER_BYTES"] else 2
        else:
            want = 1 if filled >= d["SB_HTTP_START_BYTES"] else 0
        assert got == want, f"{line}: expected {want}"

    # Explicit live-bug cases against the C gate.
    assert "wait 102400 -> 0" in out
    assert "wait 131072 -> 0" in out
    assert "wait 229376 -> 1" in out
    assert "play 102400 -> 2" in out
    assert "refill 131072 -> 2" in out
    assert "refill 229376 -> 1" in out


def main() -> int:
    d = test_header_numbers()
    test_python_hysteresis(d)
    test_c_gate_matches(d)
    print("test_radio_buf: ok")
    print(
        f"  start={d['SB_HTTP_START_BYTES']} recover={d['SB_HTTP_RECOVER_BYTES']} "
        f"underrun={d['SB_HTTP_UNDERRUN_BYTES']} cap={d['SB_HTTP_RB_SIZE']}"
    )
    print(
        f"  before: start~{OLD_START_HOVER} recover={OLD_NEED} "
        f"(need=131072) / {CAPACITY}"
    )
    print(
        f"  after:  start={d['SB_HTTP_START_BYTES']} "
        f"recover={d['SB_HTTP_RECOVER_BYTES']} "
        f"(need={d['SB_HTTP_RECOVER_BYTES']}) / {CAPACITY}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
