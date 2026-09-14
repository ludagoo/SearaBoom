#!/usr/bin/env python3
"""vol_curve v3: knobs 1–21 stay v2 loudness; 22/23/24 add I2S ALC headroom."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
H = (ROOT / "firmware/main/searaboom.h").read_text()
RP = (ROOT / "firmware/main/radio_player.c").read_text()
CS = (ROOT / "firmware/main/config_store.c").read_text()
SC = (ROOT / "firmware/main/serial_cmd.c").read_text()

SB_VOLUME_MIN = 1
SB_VOL_CURVE2_MAX = 21
SB_VOLUME_MAX = 24
SB_ALC_MIN_DB = -36
SB_ALC_CURVE2_MAX_DB = 2


def volume_to_alc(volume: int) -> int:
    if volume < SB_VOLUME_MIN:
        volume = SB_VOLUME_MIN
    if volume > SB_VOLUME_MAX:
        volume = SB_VOLUME_MAX
    if volume <= SB_VOL_CURVE2_MAX:
        return SB_ALC_MIN_DB + ((volume - 1) * (SB_ALC_CURVE2_MAX_DB - SB_ALC_MIN_DB)) // (
            SB_VOL_CURVE2_MAX - 1
        )
    extra = {22: 4, 23: 6, 24: 9}
    return extra[volume]


def test_headers_accept_new_max() -> None:
    assert "#define SB_VOLUME_MAX 24" in H
    assert "#define SB_VOL_CURVE2_MAX 21" in H
    assert "#define SB_VOL_CURVE 3" in H
    assert "#define SB_DEFAULT_VOLUME SB_VOL_CURVE2_MAX" in H


def test_v2_steps_unchanged() -> None:
    expected = {
        1: -36,
        10: -19,
        18: -4,
        20: 0,
        21: 2,
    }
    for step, alc in expected.items():
        assert volume_to_alc(step) == alc, (step, volume_to_alc(step), alc)


def test_extra_clicks() -> None:
    assert volume_to_alc(22) == 4
    assert volume_to_alc(23) == 6
    assert volume_to_alc(24) == 9
    assert volume_to_alc(25) == 9


def test_firmware_wires_curve_and_help() -> None:
    assert "SB_ALC_CLICK22_DB 4" in RP
    assert "SB_ALC_CLICK23_DB 6" in RP
    assert "SB_ALC_CLICK24_DB 9" in RP
    assert "SB_VOL_CURVE" in CS
    assert "SB_VOLUME_MAX" in SC
    assert "vol_curve v3" in RP


if __name__ == "__main__":
    test_headers_accept_new_max()
    test_v2_steps_unchanged()
    test_extra_clicks()
    test_firmware_wires_curve_and_help()
    print("ok")
