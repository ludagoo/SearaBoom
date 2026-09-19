#!/usr/bin/env python3
"""Ghost/stuck pads must not walk volume; one tap is still one step."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
H = (ROOT / "firmware/main/touch_auto_cal.h").read_text()
VB = (ROOT / "firmware/main/volume_buttons.c").read_text()
SB = (ROOT / "firmware/main/searaboom.h").read_text()
VER = (ROOT / "firmware/VERSION").read_text().strip()
SDK = (ROOT / "firmware/sdkconfig.defaults").read_text()

DEBOUNCE_MS = 250
HOLD_MAX_MS = 2000
IDF_DIV = 0.8
BIAS = 0.02
FLOOR = 0.15
CEIL = 0.50
LEAK_FIRM = 0.04
WET = 0.60
STUCK_OVER = 2.0


def clamp_auto(sens: float) -> float:
    return min(CEIL, max(FLOOR, sens))


def trip(live: float) -> float:
    return live * IDF_DIV


def may_repeat(hold_ms: int, chatter_freeze: bool) -> bool:
    return hold_ms >= 0 and hold_ms < HOLD_MAX_MS and not chatter_freeze


def stuck_should_firm(peak: float, live: float) -> bool:
    return peak > 0 and peak <= WET and peak < trip(live) * STUCK_OVER


def stuck_sens(live: float, peak: float) -> float:
    nxt = clamp_auto(peak / IDF_DIV + BIAS)
    if nxt <= live:
        nxt = clamp_auto(live * (1.0 + LEAK_FIRM))
    return nxt


def fire_count(hold_ms: int, chatter_freeze: bool = False) -> int:
    """PRESS edge = 1 step; then every 250 ms while may_repeat."""
    if hold_ms < 0:
        return 0
    n = 1
    if chatter_freeze:
        return n
    t = DEBOUNCE_MS
    while t < HOLD_MAX_MS and t <= hold_ms:
        n += 1
        t += DEBOUNCE_MS
    return n


def test_firmware_wires_hold_cap() -> None:
    assert "#define SB_DEBOUNCE_MS 250" in SB
    assert "#define SB_TOUCH_AUTO_CLEAN_HOLD_MAX_MS 2000" in H
    assert "#define SB_TOUCH_AUTO_CHATTER_N 20" in H
    assert "#define SB_TOUCH_AUTO_CHATTER_MS 30000" in H
    assert "#define SB_TOUCH_VOL_STUCK_PEAK_OVER 2.0f" in H
    assert "touch_vol_may_repeat" in H
    assert "touch_vol_may_repeat" in VB
    assert "s_hold_armed" in VB
    assert "hold_disarm" in VB
    assert "stuck-hold" in VB
    assert "pad vol stop-repeat" in VB
    poll = VB.split("void volume_buttons_poll(void)", 1)[1]
    assert "touch_vol_may_repeat(hold_ms, (int)s_learn_freeze)" in poll
    assert "s_hold_armed" in poll
    # Unbounded "while down, fire every 250 ms" is gone.
    assert "else if ((now - s_last_ms) >= SB_DEBOUNCE_MS)" not in poll
    assert "if ((now - s_last_ms) >= SB_DEBOUNCE_MS)" in poll
    assert VER == "0.5.26"
    assert 'CONFIG_SEARABOOM_FW_VERSION="0.5.26"' in SDK


def test_alc_curve_untouched() -> None:
    assert "#define SB_VOLUME_MAX 34" in SB
    assert "#define SB_VOL_CURVE2_MAX 21" in SB
    assert "#define SB_VOL_CURVE 5" in SB
    assert "listen-confirmed product ceiling" in SB
    assert "SB_ALC_CLICK35" not in SB
    assert "SB_VOLUME_MAX 35" not in SB
    assert "SB_VOLUME_MAX 63" not in SB


def test_one_tap_one_step() -> None:
    assert fire_count(0) == 1
    assert fire_count(80) == 1
    assert fire_count(200) == 1
    assert fire_count(249) == 1
    assert fire_count(250) == 2
    assert fire_count(499) == 2
    assert fire_count(500) == 3


def test_stuck_hold_stops_repeat() -> None:
    # QA ghosts: + 5488 / 2805 / 4588 ms, − 4180 ms. Old code walked ~hold/250.
    for ghost in (2805, 4180, 4588, 5488):
        old = ghost // DEBOUNCE_MS + 1
        now = fire_count(ghost)
        assert now == fire_count(HOLD_MAX_MS - 1)
        assert now == 8
        assert now < old
        assert may_repeat(ghost, False) is False
    assert fire_count(1999) == 8
    assert fire_count(2000) == 8
    assert fire_count(10_000) == 8
    assert may_repeat(1999, False) is True
    assert may_repeat(2000, False) is False


def test_chatter_freeze_stops_repeat() -> None:
    assert fire_count(5000, chatter_freeze=True) == 1
    assert may_repeat(0, True) is False
    assert may_repeat(100, False) is True
    freeze_src = VB.split("static void auto_note_chatter", 1)[1].split(
        "static void auto_queue", 1
    )[0]
    assert "s_learn_freeze = true" in freeze_src
    assert "fire(" not in freeze_src


def test_stuck_firms_ghost_not_mash() -> None:
    live = 0.355
    # QA + ghosts after first-N: peaks 0.38–0.51 vs trip 0.284.
    assert abs(trip(live) - 0.284) < 1e-6
    assert stuck_should_firm(0.38, live)
    assert stuck_should_firm(0.51, live)
    assert not stuck_should_firm(0.70, live)
    assert not stuck_should_firm(0.0, live)
    assert abs(stuck_sens(live, 0.38) - 0.495) < 1e-6
    assert stuck_sens(live, 0.51) == 0.50
    # Peak below live: do not go softer; +4% firm step.
    assert abs(stuck_sens(live, 0.20) - live * 1.04) < 1e-6
    assert "touch_vol_stuck_should_firm" in VB
    assert "touch_vol_stuck_sens" in VB


if __name__ == "__main__":
    test_firmware_wires_hold_cap()
    test_alc_curve_untouched()
    test_one_tap_one_step()
    test_stuck_hold_stops_repeat()
    test_chatter_freeze_stops_repeat()
    test_stuck_firms_ghost_not_mash()
    print("ok")
