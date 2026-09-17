#!/usr/bin/env python3
"""UI vs content: button beeps and spoken clips sit well above the stream."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RP = (ROOT / "firmware/main/radio_player.c").read_text()
VB = (ROOT / "firmware/main/volume_buttons.c").read_text()
CP = (ROOT / "firmware/main/clip_player.c").read_text()
VER = (ROOT / "firmware/VERSION").read_text().strip()
SDK = (ROOT / "firmware/sdkconfig.defaults").read_text()

SB_BEEP_AMP = 22000
SB_LIMIT_AMP = 26000
SB_BEEP_CONTENT_DUCK = 4
SB_BEEP_DUCK_FULL = 256
SB_MIX_RADIO_DUCK_DB = -18


def beep_content_scale(pos: int, length: int) -> int:
    edge = max(1, length // 3)
    ducked = SB_BEEP_DUCK_FULL // SB_BEEP_CONTENT_DUCK
    if pos < edge:
        return SB_BEEP_DUCK_FULL - ((SB_BEEP_DUCK_FULL - ducked) * pos) // edge
    if pos > length - 1 - edge:
        return SB_BEEP_DUCK_FULL - ((SB_BEEP_DUCK_FULL - ducked) * (length - 1 - pos)) // edge
    return ducked


def mix_sample(content: int, tone: int, pos: int, length: int) -> int:
    scale = beep_content_scale(pos, length)
    mixed = (content * scale) // SB_BEEP_DUCK_FULL + tone
    return max(-32768, min(32767, mixed))


def test_firmware_constants() -> None:
    assert "#define SB_BEEP_AMP 22000" in RP
    assert "#define SB_LIMIT_AMP 26000" in RP
    assert "#define SB_BEEP_CONTENT_DUCK 4" in RP
    assert "#define SB_MIX_RADIO_DUCK_DB (-18)" in RP
    assert "#define SB_MIX_RADIO_GAIN_DB 0" in RP
    assert "#define SB_MIX_CLIP_GAIN_DB 0" in RP
    assert "#define SB_BEEP_AMP 4200" not in RP
    assert "#define SB_LIMIT_AMP 5200" not in RP
    assert ".gain = {0, -12}" not in RP
    assert "SB_MIX_RADIO_DUCK_DB" in RP
    assert VER == "0.5.25"
    assert 'CONFIG_SEARABOOM_FW_VERSION="0.5.25"' in SDK


def test_beep_sits_above_hot_stream() -> None:
    length = 2425  # 55 ms at 44.1 kHz
    mid = length // 2
    content = 28000
    tone = SB_BEEP_AMP
    ducked = (content * beep_content_scale(mid, length)) // SB_BEEP_DUCK_FULL
    mixed = mix_sample(content, tone, mid, length)
    assert beep_content_scale(mid, length) == SB_BEEP_DUCK_FULL // SB_BEEP_CONTENT_DUCK
    assert ducked == content // SB_BEEP_CONTENT_DUCK
    assert tone > ducked * 2
    assert mixed == ducked + tone
    old_beep = 4200
    assert tone > old_beep * 5
    assert abs(tone) > abs(content - ducked)


def test_duck_fades_with_tone_edge() -> None:
    length = 120
    assert beep_content_scale(0, length) == SB_BEEP_DUCK_FULL
    mid = beep_content_scale(length // 2, length)
    assert mid == SB_BEEP_DUCK_FULL // SB_BEEP_CONTENT_DUCK
    assert beep_content_scale(length - 1, length) == SB_BEEP_DUCK_FULL


def test_buttons_and_clips_use_shared_split() -> None:
    assert "radio_player_beep();" in VB
    assert "radio_player_beep_limit();" in VB
    assert "SB_BEEP_AMP" not in VB
    assert "clip_player_set_volume" in CP
    assert "radio_player_set_volume(volume)" in CP
    assert "i2s_alc_volume_set" not in CP


if __name__ == "__main__":
    test_firmware_constants()
    test_beep_sits_above_hot_stream()
    test_duck_fades_with_tone_edge()
    test_buttons_and_clips_use_shared_split()
    print("ok")
