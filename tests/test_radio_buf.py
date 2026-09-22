#!/usr/bin/env python3
"""Host spec for radio HTTP ringbuffer start / recover watermarks.

Live 0.5.25 on Lucas QA Zero (d0:cf:13:07:de:fc):
  stall rb-drop rb≈98–101kB / 262144 need=131072 every ~200–300 ms.

Root cause encoded here:
  mixer consume ≈ Icecast produce, so fill hovers where play started.
  Start/resume at ~100 KB with need=128 KB sits on the 96 KB log-band edge.

Firmware policy lives in firmware/main/radio_buf.h (compiled below).
One 224 KB fill for start / recover / wifi-weak / http-slow. Leave PLAY
at 64 KB. Silence from empty is ~28 s at ~8 KB/s (~20 s from underrun).
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
# Policy this PR checks (must match radio_buf.h).
FILL = 224 * 1024
START = FILL
RECOVER = FILL
UNDERRUN = 64 * 1024
WIFI_RESUME = FILL
SLOW_RESUME = FILL


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
    assert d["SB_HTTP_START_BYTES"] == START
    assert d["SB_HTTP_RECOVER_BYTES"] == RECOVER
    assert d["SB_HTTP_UNDERRUN_BYTES"] == UNDERRUN
    assert d["SB_HTTP_SLOW_LOW_BYTES"] == 64 * 1024
    assert d["SB_HTTP_SLOW_RESUME_BYTES"] == SLOW_RESUME
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == WIFI_RESUME
    assert d["SB_WIFI_HTTP_LOW_BYTES"] == d["SB_HTTP_UNDERRUN_BYTES"]
    assert d["SB_HTTP_START_BYTES"] < d["SB_HTTP_RB_SIZE"]
    # One fill height for every resume path.
    assert d["SB_HTTP_START_BYTES"] == FILL == 229376
    assert d["SB_HTTP_RECOVER_BYTES"] == FILL
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == FILL
    assert d["SB_HTTP_SLOW_RESUME_BYTES"] == FILL
    # Above the live hover so go_live does not start on the 96 KB band edge.
    assert d["SB_HTTP_START_BYTES"] > OLD_START_HOVER
    # Leave PLAY at 64 KB, not 96 KB. Long quiet up to 224 KB is the policy.
    assert d["SB_HTTP_UNDERRUN_BYTES"] == 64 * 1024
    assert d["SB_HTTP_UNDERRUN_BYTES"] != 96 * 1024
    assert d["SB_HTTP_UNDERRUN_BYTES"] < d["SB_HTTP_RECOVER_BYTES"]
    return d


def test_python_hysteresis(d: dict[str, int]) -> None:
    start = d["SB_HTTP_START_BYTES"]
    recover = d["SB_HTTP_RECOVER_BYTES"]
    underrun = d["SB_HTTP_UNDERRUN_BYTES"]

    assert OLD_START_HOVER < start
    assert recover == start == FILL
    assert underrun == 64 * 1024

    def gate(now: int, filled: int) -> int:
        if now == d["RADIO_BUF_PLAY"]:
            return d["RADIO_BUF_REFILL"] if 0 <= filled < underrun else d["RADIO_BUF_PLAY"]
        if now == d["RADIO_BUF_REFILL"]:
            return d["RADIO_BUF_PLAY"] if filled >= recover else d["RADIO_BUF_REFILL"]
        return d["RADIO_BUF_PLAY"] if filled >= start else d["RADIO_BUF_WAIT"]

    # Boot: 100 KB after ident stays silent; 224 KB goes live.
    st = d["RADIO_BUF_WAIT"]
    for filled in (0, 64 * 1024, OLD_START_HOVER, 112 * 1024, 200 * 1024, start - 1):
        st = gate(st, filled)
        assert st == d["RADIO_BUF_WAIT"], f"started too early at {filled}"
    st = gate(st, start)
    assert st == d["RADIO_BUF_PLAY"]

    # PLAY stays PLAY until fill drops under 64 KB (96 KB is too high).
    st = gate(d["RADIO_BUF_PLAY"], OLD_START_HOVER)
    assert st == d["RADIO_BUF_PLAY"], "100 KB is still above the 64 KB leave-PLAY line"
    st = gate(d["RADIO_BUF_PLAY"], 96 * 1024)
    assert st == d["RADIO_BUF_PLAY"], "96 KB must not start a refill"
    st = gate(d["RADIO_BUF_PLAY"], 98 * 1024)
    assert st == d["RADIO_BUF_PLAY"]
    st = gate(d["RADIO_BUF_PLAY"], 101 * 1024)
    assert st == d["RADIO_BUF_PLAY"]
    st = gate(d["RADIO_BUF_PLAY"], underrun)
    assert st == d["RADIO_BUF_PLAY"], "exactly 64 KB is still PLAY"

    # Dip under 64 KB: mute and refill all the way to 224 KB.
    st = gate(d["RADIO_BUF_PLAY"], underrun - 1)
    assert st == d["RADIO_BUF_REFILL"]
    st = gate(st, OLD_START_HOVER)
    assert st == d["RADIO_BUF_REFILL"], "100 KB is still below 224 KB recover"
    st = gate(st, OLD_NEED)
    assert st == d["RADIO_BUF_REFILL"], "128 KB is still below 224 KB recover"
    st = gate(st, 200 * 1024)
    assert st == d["RADIO_BUF_REFILL"], "200 KB is still below 224 KB recover"
    st = gate(d["RADIO_BUF_REFILL"], recover)
    assert st == d["RADIO_BUF_PLAY"]

    # Healthy hover at start/recover stays PLAY. 64 KB is the floor, not a mute.
    for filled in (recover, underrun, 200 * 1024):
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
    int samples[] = {0, 65536, 98304, 102400, 114688, 131072, 204800, 229376, 262144};
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
    assert f"need0={FILL}" in hdr
    assert f"need1={FILL}" in hdr
    assert d["SB_HTTP_RECOVER_BYTES"] == FILL
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == FILL

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

    # Explicit live-bug / policy cases against the C gate.
    assert "wait 102400 -> 0" in out  # old ident hover: still wait
    assert "wait 114688 -> 0" in out  # 112 KB still wait
    assert "wait 204800 -> 0" in out  # 200 KB still wait
    assert "wait 229376 -> 1" in out  # 224 KB: play
    assert "play 102400 -> 1" in out  # 100 KB is above 64 KB: stay PLAY
    assert "play 98304 -> 1" in out   # 96 KB must not start a refill
    assert "play 65536 -> 1" in out   # exactly 64 KB: stay PLAY
    assert "play 0 -> 2" in out       # empty: refill
    assert "refill 102400 -> 2" in out  # 100 KB still shy of 224
    assert "refill 114688 -> 2" in out  # 112 KB still shy of 224
    assert "refill 204800 -> 2" in out  # 200 KB still shy of 224
    assert "refill 229376 -> 1" in out


def test_prebuf_speak_waits_for_healthy_fill() -> int:
    """8 s used to fire internet lenta during a healthy 15–28 s fill to 224 KB."""
    rp = (ROOT / "firmware" / "main" / "radio_player.c").read_text()
    m = re.search(r"^#define SB_HTTP_SLOW_PREBUF_SPEAK_MS (\d+)\s*$", rp, re.M)
    assert m, "SB_HTTP_SLOW_PREBUF_SPEAK_MS missing"
    ms = int(m.group(1))
    assert ms > 30000, (
        f"prebuf speak {ms} ms must exceed a healthy 0→224 KB fill (~28 s at ~8 KB/s)"
    )
    assert ms != 8000
    speak = rp[rp.index("bool radio_player_http_slow_should_speak(void)"):]
    speak = speak.split("\nvoid ", 1)[0]
    assert "SB_HTTP_SLOW_PREBUF_SPEAK_MS" in speak
    assert "s_prebuffering" in speak
    assert "SB_HTTP_RECOVER_BYTES" in speak
    # Critical path (fill < 64 KB) still speaks when the buffer is actually stuck.
    assert "SB_HTTP_SLOW_LOW_BYTES" in speak
    return ms


def main() -> int:
    d = test_header_numbers()
    test_python_hysteresis(d)
    test_c_gate_matches(d)
    speak_ms = test_prebuf_speak_waits_for_healthy_fill()
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
        f"underrun={d['SB_HTTP_UNDERRUN_BYTES']} "
        f"wifi_resume={d['SB_WIFI_HTTP_RESUME_BYTES']} "
        f"slow_resume={d['SB_HTTP_SLOW_RESUME_BYTES']} "
        f"(need={FILL}) / {CAPACITY}"
    )
    print(f"  prebuf_speak_ms={speak_ms}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
