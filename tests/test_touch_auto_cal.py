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


def test_factory_refine_cannot_persist_below_floor() -> None:
    floor_v = 0.15
    ceil_v = 0.50
    refine_frac = 0.20

    def refine(factory: float, proposed: float) -> float:
        lo = factory * (1.0 - refine_frac)
        hi = factory * (1.0 + refine_frac)
        p = min(hi, max(lo, proposed))
        return min(ceil_v, max(floor_v, p))

    assert "return touch_auto_clamp_auto(proposed);" in H
    assert "SB_TOUCH_AUTO_FLOOR" in ST
    assert refine(0.10, 0.15) == 0.15
    assert refine(0.10, 0.08) == 0.15
    assert abs(refine(0.20, 0.15) - 0.16) < 1e-6
    assert abs(refine(0.20, 0.19) - 0.19) < 1e-6


def test_one_pad_rev4_does_not_retire_other_first_n() -> None:
    boot = VB.split("rev >= SB_TOUCH_AUTO_REV", 1)[1].split("} else {", 1)[0]
    assert "s_first_done[0] = s_first_done[1] = true" not in boot
    assert "config_store_load_touch_auto_first" in boot
    assert "SB_TOUCH_AUTO_FIRST_UP" in boot
    assert "SB_TOUCH_AUTO_FIRST_DN" in boot
    assert "tsens_first" in ST
    assert "config_store_save_touch_sens_auto(s_sens_up, s_sens_dn, first)" in VB
    assert "nvs_set_u8(h, \"tsens_first\", first_mask)" in ST
