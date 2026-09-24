#!/usr/bin/env python3
"""Host spec: sintonizando once, then looping tuner clip until audible."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "firmware/main/main.c").read_text()
RP = (ROOT / "firmware/main/radio_player.c").read_text()
RH = (ROOT / "firmware/main/radio_player.h").read_text()
CP = (ROOT / "firmware/main/clip_player.c").read_text()
CH = (ROOT / "firmware/main/clip_player.h").read_text()
CMAKE = (ROOT / "firmware/main/CMakeLists.txt").read_text()
BUF = (ROOT / "firmware/main/radio_buf.h").read_text()
VER = (ROOT / "firmware/VERSION").read_text().strip()
SDK = (ROOT / "firmware/sdkconfig.defaults").read_text()
TTS = (ROOT / "scripts/gen_ui_clips.py").read_text()


def test_fill_policy_unchanged() -> None:
    assert VER == "0.5.28"
    assert 'CONFIG_SEARABOOM_FW_VERSION="0.5.28"' in SDK
    assert "#define SB_HTTP_START_BYTES (224 * 1024)" in BUF
    assert "#define SB_HTTP_UNDERRUN_BYTES (64 * 1024)" in BUF
    assert "#define SB_HTTP_RB_SIZE (256 * 1024)" in BUF
    assert "#define SB_HTTP_SLOW_PREBUF_SPEAK_MS 12000" in RP
    assert "s_prebuffer_last_filled" in RP
    assert "SB_HTTP_SLOW_GROW_BYTES" in RP
    assert "CONFIG_COMPILER_OPTIMIZATION_SIZE=y" in SDK
    assert "CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y" in SDK
    assert "CONFIG_COMPILER_OPTIMIZATION_DEBUG=y" not in SDK
    cmake = (ROOT / "firmware/CMakeLists.txt").read_text()
    assert "CONFIG_COMPILER_OPTIMIZATION_DEBUG=y" in cmake
    assert "file(REMOVE" in cmake


def test_sintonizando_once() -> None:
    assert "Sintonizando Rádio Seara" in TTS
    assert "fill_tune_clip" in MAIN
    assert "clip_player_play(fill_tune_clip(), false)" in MAIN
    assert "s_fill_tune_done" in MAIN
    assert "clip_player_loop(fill_tune_clip()" not in MAIN
    assert "clip_player_loop(SB_CLIP_TUNE_102)" not in MAIN
    spoken = MAIN[MAIN.index("bool spoken = "):MAIN.index("bool spoken = ") + 220]
    assert "SB_CLIP_OTA_DONE" in spoken
    assert "SB_CLIP_NET_SLOW" in spoken
    assert "SB_CLIP_WIFI_WEAK" in spoken
    assert "TUNE" not in spoken


def test_no_fill_jingle() -> None:
    assert "clip_player_loop(SB_CLIP_PREBUF)" not in MAIN
    assert "SB_CLIP_PREBUF" not in MAIN
    assert "SB_CLIP_PREBUF" not in CH
    assert "SB_PREBUF_FILL_PAUSE_MS" not in MAIN
    assert "s_fill_gap_until_ms" not in MAIN
    assert "prebuf.aac" not in CMAKE
    assert "prebuf.aac" not in TTS
    assert not (ROOT / "firmware/clips/prebuf.aac").exists()


def test_tune_fill_until_audible() -> None:
    assert "clip_player_loop(SB_CLIP_TUNE_FILL)" in MAIN
    assert "radio_player_station_audible" in MAIN
    assert "tune fill off" in MAIN
    assert "SB_CLIP_TUNE_FILL" in CH
    assert "tune_fill" in CP
    assert "tune_fill.aac" in CMAKE
    assert "tune_fill.aac" in TTS
    assert (ROOT / "firmware/clips/tune_fill.aac").is_file()
    assert (ROOT / "firmware/clips/tune_fill.aac").stat().st_size > 300 * 1024
    assert "analog_tune_sample" not in RP
    assert "mix_tune_static_s16le" not in RP
    assert "s_tune_stations" not in RP
    assert "s_tune_static" not in RP
    assert "radio_player_station_audible" in RH
    assert "pcm_upmix_radio_peak" in RP[RP.index("bool radio_player_station_audible"):RP.index(
        "bool radio_player_station_audible"
    ) + 400]
    begin = RP[RP.index("static void radio_prebuffer_begin"):RP.index(
        "static bool radio_prebuffer_release_if_ready"
    )]
    assert "radio_pcm_pause()" not in begin
    assert "mix_restart()" not in begin
    tap = RP[RP.index("static int tap_process"):RP.index("static audio_element_handle_t tap_init")]
    assert "pcm_note_s16" in tap
    assert "mix_tune_static" not in tap
    assert "analog_tune" not in tap
    assert "SB_TUNE_FILL_PAUSE_MS 0" in CP


def test_fill_clip_latches_off() -> None:
    """After tune fill off, a quiet rm2s block must not restart the clip."""
    assert "s_fill_clip_done" in MAIN
    reset = MAIN[MAIN.index("static void fill_pattern_reset"):MAIN.index(
        "static void volume_cb"
    )]
    assert "s_fill_tune_done = false" in reset
    assert "s_fill_clip_done = false" in reset
    loop = MAIN.split("clip_player_loop(SB_CLIP_TUNE_FILL)", 1)[0]
    assert "!s_fill_clip_done" in loop[-400:]
    off = MAIN[MAIN.index("tune fill off"):MAIN.index("tune fill off") + 220]
    assert "s_fill_clip_done = true" in off
    assert "s_fill_clip_done = true" in MAIN[MAIN.index("if (radio_player_station_audible())"):]
    rearm = MAIN[MAIN.index("Stall/recover re-arms fill"):MAIN.index(
        "Stall/recover re-arms fill"
    ) + 280]
    assert "radio_player_is_prebuffering()" in rearm
    assert "s_fill_clip_done = false" in rearm


def test_release_ungates() -> None:
    release = RP[RP.index("static bool radio_prebuffer_release_if_ready"):RP.index(
        "static void radio_mark_started"
    )]
    assert "amp_apply_saved" in release
    assert "mix_restart()" in release
    assert "radio_pcm_resume()" not in release
    assert "radio_pcm_pause()" not in release


def main() -> int:
    test_fill_policy_unchanged()
    test_sintonizando_once()
    test_no_fill_jingle()
    test_tune_fill_until_audible()
    test_fill_clip_latches_off()
    test_release_ungates()
    print("test_prebuf_jingle: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
