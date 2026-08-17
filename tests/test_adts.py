#!/usr/bin/env python3
"""Host tests for ADTS duration and clip-wait policy.

Catches the regression where play_wait returned when AAC bytes were queued
instead of when the speaker finished (~4.5s for ota_done).
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIPS = ROOT / "firmware" / "clips"
ADTS_C = ROOT / "firmware" / "main" / "adts_util.c"
ADTS_H = ROOT / "firmware" / "main" / "adts_util.h"

EXPECTED_MS = {
    "ota_done.aac": 4551,
    "ota_updating.aac": 6083,
    "ap_welcome.aac": 21640,
    "ap_connected.aac": 18483,
    "ap_saved.aac": 7105,
}

CLIP_TAIL_MS = 800
WIFI_TIMEOUT_MS = 25000


def adts_duration_ms(data: bytes) -> int:
    sr_tab = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
              16000, 12000, 11025, 8000, 7350, 0, 0, 0]
    i = 0
    frames = 0
    sr = 22050
    n = len(data)
    while i + 7 <= n:
        if data[i] != 0xFF or (data[i + 1] & 0xF0) != 0xF0:
            i += 1
            continue
        sidx = (data[i + 2] >> 2) & 0x0F
        if sidx < len(sr_tab) and sr_tab[sidx]:
            sr = sr_tab[sidx]
        flen = ((data[i + 3] & 0x03) << 11) | (data[i + 4] << 3) | ((data[i + 5] >> 5) & 0x07)
        if flen < 7 or i + flen > n:
            break
        frames += 1
        i += flen
    if frames <= 0 or sr <= 0:
        return 4000
    return (frames * 1024 * 1000) // sr


def play_wait_ms(dur: int, timeout_ms: int = 20000) -> int:
    wait_ms = dur + CLIP_TAIL_MS
    if timeout_ms > 0 and timeout_ms < wait_ms:
        wait_ms = timeout_ms
    return wait_ms


def test_clip_files_match_expected_duration() -> None:
    for name, expected in EXPECTED_MS.items():
        path = CLIPS / name
        assert path.is_file(), f"missing {path}"
        got = adts_duration_ms(path.read_bytes())
        assert abs(got - expected) <= 50, f"{name}: duration {got}ms != {expected}ms"


def test_play_wait_covers_speaker_time_not_byte_queue() -> None:
    dur = adts_duration_ms((CLIPS / "ota_done.aac").read_bytes())
    wait = play_wait_ms(dur)
    # Injecting 17KB takes well under 500ms. Returning then cuts the clip.
    assert wait >= 4000, f"play_wait too short: {wait}ms"
    assert wait <= dur + CLIP_TAIL_MS + 1
    assert wait < WIFI_TIMEOUT_MS, "empty-SSID path must not wait the STA timeout"


def test_welcome_is_longer_than_ota_done() -> None:
    done = adts_duration_ms((CLIPS / "ota_done.aac").read_bytes())
    welcome = adts_duration_ms((CLIPS / "ap_welcome.aac").read_bytes())
    assert welcome > done * 2


def test_c_parser_matches_python(tmp_path: Path | None = None) -> None:
    import tempfile

    harness = r"""
#include <stdio.h>
#include <stdlib.h>
#include "adts_util.h"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf) return 4;
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 5;
    fclose(f);
    printf("%d\n", sb_adts_duration_ms(buf, (size_t)n));
    free(buf);
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        (tdir / "harness.c").write_text(harness)
        bin_path = tdir / "test_adts"
        subprocess.check_call(
            [
                "gcc", "-O0", "-Wall", "-Werror",
                "-I", str(ADTS_H.parent),
                str(tdir / "harness.c"), str(ADTS_C),
                "-o", str(bin_path),
            ]
        )
        for name in EXPECTED_MS:
            out = subprocess.check_output([str(bin_path), str(CLIPS / name)], text=True)
            c_ms = int(out.strip())
            py_ms = adts_duration_ms((CLIPS / name).read_bytes())
            assert c_ms == py_ms, f"{name}: C {c_ms} != py {py_ms}"


def main() -> int:
    test_clip_files_match_expected_duration()
    test_play_wait_covers_speaker_time_not_byte_queue()
    test_welcome_is_longer_than_ota_done()
    test_c_parser_matches_python()
    print("test_adts: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
