#!/usr/bin/env python3
"""Auto-cal policy numbers: start 0.50, settle 0.15–0.50, not 0.13."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
H = (ROOT / "firmware/main/touch_auto_cal.h").read_text()
VB = (ROOT / "firmware/main/volume_buttons.c").read_text()
CS = (ROOT / "firmware/main/config_store.h").read_text()
ST = (ROOT / "firmware/main/config_store.c").read_text()
MAIN = (ROOT / "firmware/main/main.c").read_text()


def test_start_is_clamp_max_not_013() -> None:
    assert "#define SB_TOUCH_SENS 0.50f" in VB
    assert "#define SB_TOUCH_SENS 0.13f" not in VB
    assert "#define SB_TOUCH_SENS 0.10f" not in VB
    assert "#define SB_TOUCH_AUTO_CEIL 0.50f" in H
    assert "#define SB_TOUCH_AUTO_FLOOR 0.15f" in H
    assert "#define SB_TOUCH_AUTO_FIRST_N 7" in H


def test_rev4_keeps_factory_snapshot() -> None:
    assert "#define SB_TOUCH_AUTO_REV 4" in CS
    assert "#define SB_TOUCH_SENS_REV 3" in CS
    assert "tsens_f_up" in ST
    assert "config_store_save_touch_sens_auto" in ST
    assert "config_store_reset_auto_touch_sens" in ST
    assert "volume_buttons_reset_auto" in MAIN


def test_auto_from_peak_bias() -> None:
    frac = 0.85
    bias = 0.02
    floor_v = 0.15
    ceil_v = 0.50
    assert "#define SB_TOUCH_AUTO_FRAC 0.85f" in H
    assert "#define SB_TOUCH_AUTO_BIAS 0.02f" in H

    def from_peak(peak: float) -> float:
        s = peak * frac + bias
        return min(ceil_v, max(floor_v, s))

    assert from_peak(0.10) == 0.15
    assert abs(from_peak(0.20) - 0.19) < 1e-6
    assert from_peak(0.80) == 0.50
