#ifndef TOUCH_AUTO_CAL_H
#define TOUCH_AUTO_CAL_H

#include <stdint.h>
#include "config_store.h"

#ifndef SB_TOUCH_CAL_MIN
#define SB_TOUCH_CAL_MIN 0.025f
#define SB_TOUCH_CAL_MAX 0.50f
#define SB_TOUCH_CAL_FAIL 0.025f
#define SB_TOUCH_CAL_FRAC 0.80f
#endif

/* Field auto-cal from real presses (rev 4). Factory `touch cal` stays rev 3.
 * Higher channel_sens = firmer. Boot live at SB_TOUCH_SENS 0.25. Auto
 * floor 0.08 / ceil 0.50 (clamp unchanged) so boxes can settle on different
 * numbers without using 0.13 as the start. */

#define SB_TOUCH_AUTO_FIRST_N 7
#define SB_TOUCH_AUTO_LEAK_N 7
#define SB_TOUCH_AUTO_CLEAN_HOLD_MS 200
#define SB_TOUCH_AUTO_CLEAN_HOLD_MAX_MS 2000
#define SB_TOUCH_AUTO_GRAZE_HOLD_MS 80
#define SB_TOUCH_AUTO_PEAK_WET 0.60f
#define SB_TOUCH_AUTO_DUAL_REL 0.03f
#define SB_TOUCH_AUTO_SETTLE_MS 30000
#define SB_TOUCH_AUTO_LEAK_MIN_MS 3600000
#define SB_TOUCH_AUTO_LEAK_DISAGREE 0.20f
#define SB_TOUCH_AUTO_LEAK_STEP_FIRM 0.04f
#define SB_TOUCH_AUTO_LEAK_STEP_SOFT 0.03f
#define SB_TOUCH_AUTO_FACTORY_REFINE 0.20f
#define SB_TOUCH_AUTO_FRAC 0.85f
#define SB_TOUCH_AUTO_BIAS 0.02f
#define SB_TOUCH_AUTO_FLOOR 0.08f
#define SB_TOUCH_AUTO_CEIL 0.50f
#define SB_TOUCH_AUTO_IDF_DIV 0.8f
#define SB_TOUCH_AUTO_GRAZE_OVER 1.3f
#define SB_TOUCH_AUTO_CHATTER_N 20
#define SB_TOUCH_AUTO_CHATTER_MS 30000
/* Missed bumps and fired presses that were too light step down by at most this. */
#define SB_TOUCH_AUTO_STEP_MAX 0.05f
/* Fired presses firm more slowly than they soften. */
#define SB_TOUCH_AUTO_STEP_UP 0.03f
/* Do not firm unless the target is at least this far above live. */
#define SB_TOUCH_AUTO_FIRM_GAP 0.02f

static inline float touch_auto_clamp_hard(float sens)
{
    if (sens < SB_TOUCH_CAL_MIN) {
        return SB_TOUCH_CAL_MIN;
    }
    if (sens > SB_TOUCH_CAL_MAX) {
        return SB_TOUCH_CAL_MAX;
    }
    return sens;
}

/* Auto persist never goes below 0.08 / above 0.50. */
static inline float touch_auto_clamp_auto(float sens)
{
    if (sens < SB_TOUCH_AUTO_FLOOR) {
        sens = SB_TOUCH_AUTO_FLOOR;
    }
    if (sens > SB_TOUCH_AUTO_CEIL) {
        sens = SB_TOUCH_AUTO_CEIL;
    }
    return touch_auto_clamp_hard(sens);
}

/* peak → stored channel_sens, firmer than factory peak*0.80. */
static inline float touch_auto_from_peak(float peak)
{
    return touch_auto_clamp_auto(peak * SB_TOUCH_AUTO_FRAC + SB_TOUCH_AUTO_BIAS);
}

/* ±20% of factory, then auto floor/ceil. A factory window under 0.08
 * is raised to the floor. */
static inline float touch_auto_refine_factory(float factory, float proposed)
{
    float lo = factory * (1.0f - SB_TOUCH_AUTO_FACTORY_REFINE);
    float hi = factory * (1.0f + SB_TOUCH_AUTO_FACTORY_REFINE);
    if (proposed < lo) {
        proposed = lo;
    }
    if (proposed > hi) {
        proposed = hi;
    }
    return touch_auto_clamp_auto(proposed);
}

static inline float touch_auto_idf_trip(float channel_sens)
{
    return channel_sens * SB_TOUCH_AUTO_IDF_DIV;
}

/* Trip sits a little under the peak: trip = peak * 0.85, so
 * sens = clamp(peak * 0.85 / 0.8, floor, ceil). Factory ±20% is not applied. */
static inline float touch_auto_target(float peak)
{
    return touch_auto_clamp_auto(peak * SB_TOUCH_AUTO_FRAC / SB_TOUCH_AUTO_IDF_DIV);
}

/* Button never fired. Step down toward the target, at most 0.05.
 * A miss never firms the pad. Peak above 0.60 is ignored. */
static inline float touch_auto_step_missed(float live, float peak)
{
    float target;
    float next;

    if (!(peak > SB_TOUCH_CAL_FAIL) || peak > SB_TOUCH_AUTO_PEAK_WET) {
        return live;
    }
    target = touch_auto_target(peak);
    if (!(target < live)) {
        return live;
    }
    next = live - SB_TOUCH_AUTO_STEP_MAX;
    if (next < target) {
        next = target;
    }
    if (next < SB_TOUCH_AUTO_FLOOR) {
        next = SB_TOUCH_AUTO_FLOOR;
    }
    if (!(next < live)) {
        return live;
    }
    return next;
}

/* Button fired. Step toward the target. Up at most 0.03, and only when
 * the target is at least 0.02 above live. Down at most 0.05. Wet peaks
 * (above 0.60) do not move. Factory ±20% is not applied. */
static inline float touch_auto_step_fired(float live, float peak)
{
    float target;
    float next;

    if (!(peak > SB_TOUCH_CAL_FAIL) || peak > SB_TOUCH_AUTO_PEAK_WET) {
        return live;
    }
    target = touch_auto_target(peak);
    if (target > live) {
        if (target < live + SB_TOUCH_AUTO_FIRM_GAP) {
            return live;
        }
        next = live + SB_TOUCH_AUTO_STEP_UP;
        if (next > target) {
            next = target;
        }
        if (next > SB_TOUCH_AUTO_CEIL) {
            next = SB_TOUCH_AUTO_CEIL;
        }
        if (!(next > live)) {
            return live;
        }
        return next;
    }
    if (target < live) {
        next = live - SB_TOUCH_AUTO_STEP_MAX;
        if (next < target) {
            next = target;
        }
        if (next < SB_TOUCH_AUTO_FLOOR) {
            next = SB_TOUCH_AUTO_FLOOR;
        }
        if (!(next < live)) {
            return live;
        }
        return next;
    }
    return live;
}

static inline int touch_auto_is_graze(int hold_ms, float peak, float live_sens)
{
    return hold_ms < SB_TOUCH_AUTO_GRAZE_HOLD_MS
           && peak < touch_auto_idf_trip(live_sens) * SB_TOUCH_AUTO_GRAZE_OVER;
}

static inline int touch_auto_is_clean(int hold_ms, float peak)
{
    return hold_ms >= SB_TOUCH_AUTO_CLEAN_HOLD_MS
           && hold_ms <= SB_TOUCH_AUTO_CLEAN_HOLD_MAX_MS
           && peak >= SB_TOUCH_CAL_FAIL
           && peak <= SB_TOUCH_AUTO_PEAK_WET;
}

static inline float touch_auto_median(const float *v, int n)
{
    float a[SB_TOUCH_AUTO_FIRST_N];
    int i;
    int j;

    if (n <= 0) {
        return 0;
    }
    if (n > SB_TOUCH_AUTO_FIRST_N) {
        n = SB_TOUCH_AUTO_FIRST_N;
    }
    for (i = 0; i < n; i++) {
        a[i] = v[i];
    }
    for (i = 1; i < n; i++) {
        float x = a[i];
        j = i;
        while (j > 0 && a[j - 1] > x) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = x;
    }
    return a[n / 2];
}

static inline float touch_auto_leak_step(float live, float target)
{
    float next;

    if (target > live) {
        next = live * (1.0f + SB_TOUCH_AUTO_LEAK_STEP_FIRM);
        if (next > target) {
            next = target;
        }
    } else {
        next = live * (1.0f - SB_TOUCH_AUTO_LEAK_STEP_SOFT);
        if (next < target) {
            next = target;
        }
    }
    return touch_auto_clamp_auto(next);
}

static inline int touch_auto_leak_disagrees(float live, float target)
{
    float d;

    if (live <= 0) {
        return 1;
    }
    d = target - live;
    if (d < 0) {
        d = -d;
    }
    return (d / live) > SB_TOUCH_AUTO_LEAK_DISAGREE;
}

#endif
