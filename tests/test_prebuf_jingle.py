#!/usr/bin/env python3
"""Host spec: prebuffer fill chime, not a stacked prompt, fill policy unchanged."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "firmware/main/main.c").read_text()
CP = (ROOT / "firmware/main/clip_player.c").read_text()
CH = (ROOT / "firmware/main/clip_player.h").read_text()
CMAKE = (ROOT / "firmware/main/CMakeLists.txt").read_text()
BUF = (ROOT / "firmware/main/radio_buf.h").read_text()
VER = (ROOT / "firmware/VERSION").read_text().strip()
SDK = (ROOT / "firmware/sdkconfig.defaults").read_text()
AAC = ROOT / "firmware/clips/prebuf.aac"


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
        return 0
    return (frames * 1024 * 1000) // sr


def test_fill_policy_unchanged() -> None:
    assert VER == "0.5.27"
    assert 'CONFIG_SEARABOOM_FW_VERSION="0.5.27"' in SDK
    assert "#define SB_HTTP_START_BYTES (224 * 1024)" in BUF
    assert "#define SB_HTTP_UNDERRUN_BYTES (64 * 1024)" in BUF
    assert "#define SB_HTTP_RB_SIZE (256 * 1024)" in BUF
    assert "radio_buf_play_need" in BUF
    rp = (ROOT / "firmware/main/radio_player.c").read_text()
    assert "#define SB_HTTP_SLOW_PREBUF_SPEAK_MS 12000" in rp
    assert "s_prebuffer_last_filled" in rp
    assert "SB_HTTP_SLOW_GROW_BYTES" in rp


def test_clip_wired() -> None:
    assert "SB_CLIP_PREBUF" in CH
    assert CH.index("SB_CLIP_PREBUF") < CH.index("SB_CLIP_COUNT")
    assert '"../clips/prebuf.aac"' in CMAKE
    assert 'asm("_binary_prebuf_aac_start")' in CP
    assert '[SB_CLIP_PREBUF] = "prebuf"' in CP
    assert "[SB_CLIP_PREBUF] = prebuf_aac_start" in CP
    assert "[SB_CLIP_PREBUF] = prebuf_aac_end" in CP


def test_chime_asset() -> None:
    assert AAC.is_file(), "missing firmware/clips/prebuf.aac"
    data = AAC.read_bytes()
    assert data[:2] == b"\xff\xf1" or data[:2] == b"\xff\xf9"
    ms = adts_duration_ms(data)
    assert 800 <= ms <= 1200, f"chime duration {ms} ms, want ~1 s"
    # AAC-LC ADTS, 44100 mono like the spoken clips.
    sidx = (data[2] >> 2) & 0x0F
    ch = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03)
    assert sidx == 4, f"sample rate index {sidx}, want 44100 (4)"
    assert ch == 1, f"channels {ch}, want mono"


def test_loop_with_gap_not_capped() -> None:
    assert "#define SB_PREBUF_CHIME_PAUSE_MS 5000" in CP
    pause_fn = CP[CP.index("static int clip_loop_pause_ms"):]
    pause_fn = pause_fn.split("esp_err_t clip_player_init", 1)[0]
    assert "SB_CLIP_PREBUF" in pause_fn
    assert "SB_PREBUF_CHIME_PAUSE_MS" in pause_fn
    tick = CP[CP.index("void clip_player_tick(void)"):]
    tick = tick.split("\nstatic bool clip_is_field", 1)[0]
    assert "SB_CLIP_NET_SLOW" in tick
    assert "SB_CLIP_WIFI_WEAK" in tick
    assert "SB_CLIP_PREBUF" not in tick  # no max_plays cap; loop until stop


def test_main_trigger() -> None:
    assert "clip_player_loop(SB_CLIP_PREBUF)" in MAIN
    assert "radio_player_is_prebuffering()" in MAIN
    assert "SB_CLIP_OTA_DONE" in MAIN
    assert "SB_CLIP_NET_SLOW" in MAIN
    assert "SB_CLIP_WIFI_WEAK" in MAIN
    # Start only on silent fill, not while a spoken prompt is up.
    start = MAIN[MAIN.index("clip_player_loop(SB_CLIP_PREBUF)") - 400 :]
    start = start.split("clip_player_loop(SB_CLIP_PREBUF)", 1)[0]
    assert "!spoken" in start
    assert "radio_player_is_prebuffering()" in start
    # Stop when the station is audible.
    assert "playing == SB_CLIP_PREBUF && !radio_player_is_prebuffering()" in MAIN
    # Prompts may still start while the jingle is the active clip.
    assert "if (!clip_player_is_active() || jingle)" in MAIN


def main() -> int:
    test_fill_policy_unchanged()
    test_clip_wired()
    test_chime_asset()
    test_loop_with_gap_not_capped()
    test_main_trigger()
    print("test_prebuf_jingle: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
