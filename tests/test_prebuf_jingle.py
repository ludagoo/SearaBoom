#!/usr/bin/env python3
"""Host spec: sintonizando once, then analog tuning static until audible."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "firmware/main/main.c").read_text()
RP = (ROOT / "firmware/main/radio_player.c").read_text()
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
    assert "SB_PREBUF_FILL_PAUSE_MS" not in MAIN
    assert "s_fill_gap_until_ms" not in MAIN


def test_analog_static_until_audible() -> None:
    assert "mix_tune_static_s16le" in RP
    assert "analog_tune_sample" in RP
    assert "s_tune_static = true" in RP
    begin = RP[RP.index("static void radio_prebuffer_begin"):RP.index(
        "static bool radio_prebuffer_release_if_ready"
    )]
    assert "s_tune_static = true" in begin
    assert "radio_pcm_pause()" not in begin
    assert "mix_restart()" not in begin
    note = RP[RP.index("static void pcm_note_s16"):RP.index("void radio_player_pcm_arm")]
    assert "s_tune_static = false" in note
    assert "SB_PCM_HOLD_ABS" in note
    tap = RP[RP.index("static int tap_process"):RP.index("static audio_element_handle_t tap_init")]
    assert "pcm_note_s16" in tap
    assert "mix_tune_static_s16le" in tap
    assert tap.index("pcm_note_s16") < tap.index("mix_tune_static_s16le")
    assert "mix_restart()" not in begin
    sample = RP[RP.index("static int16_t analog_tune_sample"):RP.index(
        "static void mix_tune_static_s16le"
    )]
    assert "SB_STATIC_POP" not in RP
    assert ">> 24" not in sample
    assert "s_tune_stations" in sample
    assert "SB_DIAL_PASS" in sample
    assert "prox" in sample
    assert "SB_STATIC_HISS" in sample
    hiss = int(RP.split("#define SB_STATIC_HISS ")[1].split()[0])
    whistle = int(RP.split("#define SB_STATIC_WHISTLE ")[1].split()[0])
    assert whistle > hiss, f"stations should whistle by, not hiss-only ({hiss}/{whistle})"
    assert hiss >= 1500, f"static between stations too quiet ({hiss})"


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
    test_analog_static_until_audible()
    test_release_ungates()
    print("test_prebuf_jingle: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
