#!/usr/bin/env python3
"""Auto-cal policy numbers: start 0.25, settle 0.08–0.50, not 0.13."""
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
    assert (ROOT / "firmware/VERSION").read_text().strip() == "0.5.28"
    assert "#define SB_TOUCH_SENS 0.25f" in VB
    assert "#define SB_TOUCH_SENS 0.50f" not in VB
    assert "#define SB_TOUCH_SENS 0.13f" not in VB
    assert "#define SB_TOUCH_SENS 0.10f" not in VB
    assert "#define SB_TOUCH_AUTO_CEIL 0.50f" in H
    assert "#define SB_TOUCH_AUTO_FLOOR 0.08f" in H
    assert "#define SB_TOUCH_CAL_FAIL 0.025f" in H
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
    floor_v = 0.08
    ceil_v = 0.50
    assert "#define SB_TOUCH_AUTO_FRAC 0.85f" in H
    assert "#define SB_TOUCH_AUTO_BIAS 0.02f" in H

    def from_peak(peak: float) -> float:
        s = peak * frac + bias
        return min(ceil_v, max(floor_v, s))

    assert from_peak(0.05) == 0.08
    assert abs(from_peak(0.10) - 0.105) < 1e-6
    assert abs(from_peak(0.20) - 0.19) < 1e-6
    assert from_peak(0.80) == 0.50


def test_factory_refine_cannot_persist_below_floor() -> None:
    floor_v = 0.08
    ceil_v = 0.50
    refine_frac = 0.20

    def refine(factory: float, proposed: float) -> float:
        lo = factory * (1.0 - refine_frac)
        hi = factory * (1.0 + refine_frac)
        p = min(hi, max(lo, proposed))
        return min(ceil_v, max(floor_v, p))

    assert "return touch_auto_clamp_auto(proposed);" in H
    assert "SB_TOUCH_AUTO_FLOOR" in ST
    assert abs(refine(0.10, 0.15) - 0.12) < 1e-6
    assert abs(refine(0.10, 0.08) - 0.08) < 1e-6
    assert refine(0.05, 0.04) == 0.08
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


def test_step_settles_both_ways() -> None:
    """Host-compile the step. Misses only soften; fired presses can firm."""
    assert "static inline float touch_auto_target" in H
    assert "static inline float touch_auto_step_missed" in H
    assert "static inline float touch_auto_step_fired" in H
    assert "#define SB_TOUCH_AUTO_STEP_MAX 0.05f" in H
    assert "#define SB_TOUCH_AUTO_STEP_UP 0.03f" in H
    assert "#define SB_TOUCH_AUTO_FIRM_GAP 0.02f" in H
    assert "#define SB_TOUCH_AUTO_PEAK_WET 0.60f" in H
    assert "leak-graze" not in VB
    assert '"missed-down"' in VB
    assert '"fired-down"' in VB
    assert '"fired-up"' in VB
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

    if (!near(touch_auto_target(0.20f), 0.2125f)) {
        return fail("target");
    }
    if (!near(touch_auto_target(0.04f), 0.08f)) {
        return fail("target floor");
    }
    if (!near(touch_auto_target(0.80f), 0.50f)) {
        return fail("target ceil");
    }
    if (!near(touch_auto_idf_trip(0.08f), 0.064f)) {
        return fail("floor trip");
    }

    s = touch_auto_step_missed(0.25f, 0.04f);
    if (!near(s, 0.20f)) {
        return fail("missed first");
    }
    s = touch_auto_step_missed(s, 0.04f);
    if (!near(s, 0.15f)) {
        return fail("missed second");
    }
    s = touch_auto_step_missed(s, 0.04f);
    if (!near(s, 0.10f)) {
        return fail("missed third");
    }
    s = touch_auto_step_missed(s, 0.04f);
    if (!near(s, 0.08f)) {
        return fail("missed floor");
    }
    s = touch_auto_step_missed(s, 0.04f);
    if (!near(s, 0.08f)) {
        return fail("floor sticks");
    }
    if (!near(touch_auto_step_missed(0.08f, 0.40f), 0.08f)) {
        return fail("missed must not firm");
    }
    if (!near(touch_auto_step_missed(0.25f, 0.70f), 0.25f)) {
        return fail("wet miss ignored");
    }
    if (!near(touch_auto_step_fired(0.25f, 0.70f), 0.25f)) {
        return fail("wet fire ignored");
    }
    if (!near(touch_auto_step_fired(0.25f, 0.61f), 0.25f)) {
        return fail("wet just over ignored");
    }
    /* 0.40 * 0.85 / 0.8 = 0.425, firm by 0.03 → 0.28. */
    if (!near(touch_auto_step_fired(0.25f, 0.40f), 0.28f)) {
        return fail("fired up cap");
    }
    /* 0.25 * 0.85 / 0.8 = 0.265625, gap under 0.02. */
    if (!near(touch_auto_step_fired(0.25f, 0.25f), 0.25f)) {
        return fail("fired up gap");
    }
    /* 0.22 * 0.85 / 0.8 = 0.23375, small soften. */
    if (!near(touch_auto_step_fired(0.25f, 0.22f), 0.23375f)) {
        return fail("fired down snug");
    }
    /* 0.18 * 0.85 / 0.8 = 0.19125, down cap 0.05 → 0.20. */
    if (!near(touch_auto_step_fired(0.25f, 0.18f), 0.20f)) {
        return fail("fired down cap");
    }
    /* Factory +20% of 0.25 is 0.30. Two firm steps pass it. */
    s = touch_auto_step_fired(0.25f, 0.50f);
    if (!near(s, 0.28f)) {
        return fail("firm step 1");
    }
    s = touch_auto_step_fired(s, 0.50f);
    if (!near(s, 0.31f)) {
        return fail("firm past factory window");
    }
    if (s > 0.50f) {
        return fail("above ceil");
    }
    if (!near(touch_auto_step_fired(0.48f, 0.55f), 0.50f)) {
        return fail("firm to ceil");
    }
    if (!near(touch_auto_step_fired(0.49f, 0.55f), 0.49f)) {
        return fail("gap blocks last bit");
    }
    if (touch_auto_step_missed(0.08f, 0.03f) < 0.08f - 0.0001f) {
        return fail("below floor");
    }
    if (touch_auto_step_fired(0.50f, 0.55f) > 0.50f + 0.0001f) {
        return fail("fired above ceil");
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
    test_step_settles_both_ways()
    print("ok")
