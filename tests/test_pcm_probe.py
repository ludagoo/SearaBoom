#!/usr/bin/env python3
"""Host spec for the on-device AUDIOTEST PCM probe.

Firmware generates 1 s of 44.1 kHz stereo s16:
  0-80 ms   8 kHz
  80-920 ms 440 Hz
  920-1000 ms 3 kHz

The mixer tap classifies 512-sample windows by zero-crossing rate.
If the clip path cuts the tail, end_hz is not in the 3 kHz band.
"""
from __future__ import annotations

import math
import struct
import sys

SR = 44100
PROBE_MS = 1000
START_MS = 80
END_MS = 80
START_HZ = 8000
END_HZ = 3000
MID_HZ = 440
AMP = 12000
WIN = 512
START_LO, START_HI = 5500, 10500
END_LO, END_HI = 2000, 4500


def tone_frame(frame: int) -> int:
    ms = frame * 1000 // SR
    if ms < START_MS:
        hz = START_HZ
    elif ms >= PROBE_MS - END_MS:
        hz = END_HZ
    else:
        hz = MID_HZ
    phase = (frame * hz) / SR
    return int(AMP * math.sin(2 * math.pi * phase))


def classify_window(samples: list[int]) -> int | None:
    peak = max(abs(x) for x in samples)
    if peak < 2000:
        return None
    zc = 0
    prev = samples[0]
    for v in samples[1:]:
        if (prev < 0) != (v < 0):
            zc += 1
        prev = v
    n = len(samples)
    return (zc * SR) // (2 * n)


def test_probe_markers() -> None:
    n = SR * PROBE_MS // 1000
    pcm = [tone_frame(i) for i in range(n)]
    first_hz = last_hz = None
    first_i = last_i = None
    for i in range(0, n - WIN, WIN):
        hz = classify_window(pcm[i : i + WIN])
        if hz is None:
            continue
        if first_hz is None:
            first_hz = hz
            first_i = i
        last_hz = hz
        last_i = i
    assert first_hz is not None and last_hz is not None
    assert START_LO <= first_hz <= START_HI, f"start_hz={first_hz}"
    assert END_LO <= last_hz <= END_HI, f"end_hz={last_hz}"
    dur_ms = (last_i - first_i) * 1000 // SR
    assert 800 <= dur_ms <= 1000, f"dur_ms={dur_ms}"
    packed = struct.pack(f"<{n}h", *pcm)
    assert len(packed) == n * 2


def test_cutoff_fails_end_band() -> None:
    n = SR * PROBE_MS // 1000
    cut = int(n * 0.85)
    pcm = [tone_frame(i) for i in range(cut)]
    last_hz = None
    for i in range(0, len(pcm) - WIN, WIN):
        hz = classify_window(pcm[i : i + WIN])
        if hz is not None:
            last_hz = hz
    assert last_hz is not None
    assert not (END_LO <= last_hz <= END_HI), f"truncated clip still looked like end_hz={last_hz}"


def main() -> int:
    test_probe_markers()
    test_cutoff_fails_end_band()
    print("test_pcm_probe: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
