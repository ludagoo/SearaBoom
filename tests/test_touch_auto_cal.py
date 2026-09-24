#!/usr/bin/env python3
"""Auto-cal policy numbers: start 0.25, settle 0.15–0.50, not 0.13."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
H = (ROOT / "firmware/main/touch_auto_cal.h").read_text()
VB = (ROOT / "firmware/main/volume_buttons.c").read_text()
CS = (ROOT / "firmware/main/config_store.h").read_text()
ST = (ROOT / "firmware/main/config_store.c").read_text()
MAIN = (ROOT / "firmware/main/main.c").read_text()


def test_start_is_025_not_013() -> None:
    assert "#define SB_TOUCH_SENS 0.25f" in VB
    assert "#define SB_TOUCH_SENS 0.50f" not in VB
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


def test_step_softer_down_only() -> None:
    """Host-compile touch_auto_step_softer. Weak bumps step 0.05, never up."""
    assert "static inline float touch_auto_step_softer" in H
    assert "#define SB_TOUCH_AUTO_STEP_MAX 0.05f" in H
    assert "#define SB_TOUCH_AUTO_TRIP_UNDER 0.02f" in H
    assert "leak-graze" not in VB
    assert '"missed"' in VB
    assert '"fired"' in VB
    assert "now_ms() + 5000" not in VB
    assert "now_ms() + SB_TOUCH_AUTO_SETTLE_MS" in VB
    harness = r"""
#define CONFIG_STORE_H
#include "touch_auto_cal.h"
#include <stdio.h>

static int near(float a, float b)
{
    float d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d < 0.001f;
}

static int fail(const char *msg)
{
    fprintf(stderr, "fail: %s\n", msg);
    return 1;
}

int main(void)
{
    float s;

    s = touch_auto_step_softer(0.25f, 0.04f);
    if (!near(s, 0.20f)) {
        return fail("first weak step");
    }
    s = touch_auto_step_softer(s, 0.04f);
    if (!near(s, 0.15f)) {
        return fail("second weak step");
    }
    s = touch_auto_step_softer(s, 0.04f);
    if (!near(s, 0.15f)) {
        return fail("floor");
    }
    if (!near(touch_auto_step_softer(0.25f, 0.80f), 0.25f)) {
        return fail("hard press must not firm");
    }
    if (!near(touch_auto_step_softer(0.25f, 0.70f), 0.25f)) {
        return fail("wet peak must not firm");
    }
    /* 0.19 - 0.02 = 0.17 trip → sens 0.2125, inside the 0.05 cap. */
    if (!near(touch_auto_step_softer(0.25f, 0.19f), 0.2125f)) {
        return fail("snug under peak");
    }
    /* 0.22 - 0.02 = 0.20 trip → sens 0.25, already there. */
    if (!near(touch_auto_step_softer(0.25f, 0.22f), 0.25f)) {
        return fail("already just under");
    }
    if (!near(touch_auto_step_softer(0.50f, 0.10f), 0.45f)) {
        return fail("cap");
    }
    if (!near(touch_auto_step_softer(0.10f, 0.90f), 0.10f)) {
        return fail("do not raise a low live");
    }
    if (!near(touch_auto_step_softer(0.16f, 0.04f), 0.15f)) {
        return fail("floor on the last bit");
    }
    /* Factory ±20% of 0.25 would stop at 0.20. The step does not. */
    s = touch_auto_step_softer(0.25f, 0.05f);
    s = touch_auto_step_softer(s, 0.05f);
    if (!near(s, 0.15f)) {
        return fail("down to floor despite factory window");
    }
    if (touch_auto_step_softer(0.15f, 0.03f) < 0.15f - 0.0001f) {
        return fail("below floor");
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "harness.c"
        src.write_text(harness)
        bin_path = Path(td) / "test_touch_step"
        subprocess.check_call(
            [
                "gcc", "-O0", "-Wall", "-Werror",
                "-I", str(ROOT / "firmware/main"),
                str(src),
                "-o", str(bin_path),
            ]
        )
        subprocess.check_call([str(bin_path)])


if __name__ == "__main__":
    test_start_is_025_not_013()
    test_rev4_keeps_factory_snapshot()
    test_auto_from_peak_bias()
    test_factory_refine_cannot_persist_below_floor()
    test_one_pad_rev4_does_not_retire_other_first_n()
    test_step_softer_down_only()
    print("ok")
