#!/usr/bin/env python3
"""Host spec: sintonizando once, then looping activity jingle. Fill policy unchanged."""
from __future__ import annotations

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
TTS = (ROOT / "scripts/gen_ui_clips.py").read_text()


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
    assert VER == "0.5.28"
    assert 'CONFIG_SEARABOOM_FW_VERSION="0.5.28"' in SDK
    assert "#define SB_HTTP_START_BYTES (224 * 1024)" in BUF
    assert "#define SB_HTTP_UNDERRUN_BYTES (64 * 1024)" in BUF
    assert "#define SB_HTTP_RB_SIZE (256 * 1024)" in BUF
    rp = (ROOT / "firmware/main/radio_player.c").read_text()
    assert "#define SB_HTTP_SLOW_PREBUF_SPEAK_MS 12000" in rp
    assert "s_prebuffer_last_filled" in rp
    assert "SB_HTTP_SLOW_GROW_BYTES" in rp


def test_sintonizando_once() -> None:
    assert "Sintonizando Rádio Seara" in TTS
    assert "fill_tune_clip" in MAIN
    assert "clip_player_play(fill_tune_clip(), false)" in MAIN
    assert "s_fill_tune_done" in MAIN
    # Tune is one-shot, not looped as the fill activity.
    assert "clip_player_loop(fill_tune_clip()" not in MAIN
    assert "clip_player_loop(SB_CLIP_TUNE_102)" not in MAIN
    assert "fill_next_clip" not in MAIN
    spoken = MAIN[MAIN.index("bool spoken = "):MAIN.index("bool spoken = ") + 220]
    assert "SB_CLIP_OTA_DONE" in spoken
    assert "SB_CLIP_NET_SLOW" in spoken
    assert "SB_CLIP_WIFI_WEAK" in spoken
    assert "TUNE" not in spoken


def test_jingle_loops_after_tune() -> None:
    assert "clip_player_loop(SB_CLIP_PREBUF)" in MAIN
    assert "s_fill_tune_done" in MAIN
    body = MAIN[MAIN.index("else if (!s_fill_tune_done)"):]
    body = body.split("playing = clip_player_playing()", 1)[0]
    assert "clip_player_play(fill_tune_clip(), false)" in body
    assert "clip_player_loop(SB_CLIP_PREBUF)" in body
    # Tune starts first; jingle only in the tune_done branch.
    assert body.index("clip_player_play(fill_tune_clip(), false)") < body.index(
        "clip_player_loop(SB_CLIP_PREBUF)"
    )
    assert "#define SB_PREBUF_JINGLE_PAUSE_MS 400" in CP
    pause = CP[CP.index("static int clip_loop_pause_ms"):]
    pause = pause.split("esp_err_t clip_player_init", 1)[0]
    assert "SB_CLIP_PREBUF" in pause
    assert "SB_PREBUF_JINGLE_PAUSE_MS" in pause
    tick = CP[CP.index("void clip_player_tick(void)"):]
    tick = tick.split("\nstatic bool clip_is_field", 1)[0]
    assert "SB_CLIP_PREBUF" not in tick  # no max_plays; loop until stop


def test_jingle_asset() -> None:
    assert '"../clips/prebuf.aac"' in CMAKE
    assert AAC.is_file()
    data = AAC.read_bytes()
    ms = adts_duration_ms(data)
    assert 1400 <= ms <= 2000, f"jingle duration {ms} ms, want a short multi-note phrase"
    sidx = (data[2] >> 2) & 0x0F
    ch = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03)
    assert sidx == 4, f"sample rate index {sidx}, want 44100 (4)"
    assert ch == 1, f"channels {ch}, want mono"


def test_stops_when_audible() -> None:
    assert "if (!clip_player_is_active() || fill)" in MAIN
    assert "clip_is_fill_pattern(playing)" in MAIN
    assert "!radio_player_is_prebuffering()" in MAIN


def main() -> int:
    test_fill_policy_unchanged()
    test_sintonizando_once()
    test_jingle_loops_after_tune()
    test_jingle_asset()
    test_stops_when_audible()
    print("test_prebuf_jingle: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
