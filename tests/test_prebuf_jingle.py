#!/usr/bin/env python3
"""Host spec: fill pattern is sintonizando + chime, fill policy unchanged."""
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
    assert VER == "0.5.27"
    assert 'CONFIG_SEARABOOM_FW_VERSION="0.5.27"' in SDK
    assert "#define SB_HTTP_START_BYTES (224 * 1024)" in BUF
    assert "#define SB_HTTP_UNDERRUN_BYTES (64 * 1024)" in BUF
    assert "#define SB_HTTP_RB_SIZE (256 * 1024)" in BUF
    rp = (ROOT / "firmware/main/radio_player.c").read_text()
    assert "#define SB_HTTP_SLOW_PREBUF_SPEAK_MS 12000" in rp
    assert "s_prebuffer_last_filled" in rp
    assert "SB_HTTP_SLOW_GROW_BYTES" in rp


def test_sintonizando_clip_name() -> None:
    assert "Sintonizando Rádio Seara" in TTS
    assert '"id": "tune_102"' in TTS
    assert '"id": "tune_104"' in TTS
    assert "SB_CLIP_TUNE_102" in CH
    assert "SB_CLIP_TUNE_104" in CH
    assert "fill_tune_clip" in MAIN
    assert "SB_CLIP_TUNE_104" in MAIN
    assert "SB_CLIP_TUNE_102" in MAIN
    # Station pick matches boot ident (URL2 → 104.7).
    assert MAIN.count("? SB_CLIP_TUNE_104 : SB_CLIP_TUNE_102") >= 2


def test_clip_wired() -> None:
    assert "SB_CLIP_PREBUF" in CH
    assert '"../clips/prebuf.aac"' in CMAKE
    assert '[SB_CLIP_PREBUF] = "prebuf"' in CP
    assert AAC.is_file()
    data = AAC.read_bytes()
    ms = adts_duration_ms(data)
    assert 800 <= ms <= 1200, f"chime duration {ms} ms, want ~1 s"


def test_alternates_with_short_gap() -> None:
    assert "#define SB_PREBUF_FILL_PAUSE_MS 400" in MAIN
    assert "5000" not in MAIN.split("SB_PREBUF_FILL_PAUSE_MS", 1)[1][:80]
    assert "fill_next_clip" in MAIN
    nxt = MAIN[MAIN.index("static sb_clip_id_t fill_next_clip"):]
    nxt = nxt.split("static void fill_pattern_reset", 1)[0]
    assert "SB_CLIP_PREBUF" in nxt
    assert "fill_tune_clip" in nxt
    helpers = MAIN[MAIN.index("static bool clip_is_fill_pattern"):MAIN.index("static sb_clip_id_t fill_next_clip")]
    assert "SB_CLIP_PREBUF" in helpers
    assert "SB_CLIP_TUNE_102" in helpers
    assert "SB_CLIP_TUNE_104" in helpers
    assert "clip_player_play(next, false)" in MAIN
    assert "clip_player_loop(SB_CLIP_PREBUF)" not in MAIN
    assert "SB_PREBUF_CHIME_PAUSE_MS" not in CP


def test_main_trigger() -> None:
    assert "radio_player_is_prebuffering()" in MAIN
    # Spoken blockers do not include sintonizando.
    spoken = MAIN[MAIN.index("bool spoken = "):MAIN.index("bool spoken = ") + 220]
    assert "SB_CLIP_OTA_DONE" in spoken
    assert "SB_CLIP_NET_SLOW" in spoken
    assert "SB_CLIP_WIFI_WEAK" in spoken
    assert "TUNE" not in spoken
    assert "if (!clip_player_is_active() || fill)" in MAIN
    assert "clip_is_fill_pattern(playing)" in MAIN
    assert "!radio_player_is_prebuffering()" in MAIN


def main() -> int:
    test_fill_policy_unchanged()
    test_sintonizando_clip_name()
    test_clip_wired()
    test_alternates_with_short_gap()
    test_main_trigger()
    print("test_prebuf_jingle: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
