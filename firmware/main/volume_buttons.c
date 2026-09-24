#include "volume_buttons.h"
#include "searaboom.h"
#include "board.h"
#include "config_store.h"
#include "touch_auto_cal.h"
#include "radio_player.h"
#include "led_status.h"
#include "log_shipper.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "touch_element/touch_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "volume_buttons";

/* channel_sens is a press threshold: higher = firmer press needed.
 * Boot does not auto-run calibrate() (field OTA must not prompt).
 * Factory USB wipes NVS cal; the desktop flasher then runs `touch cal`.
 * QA/product start is 0.25 — softer than the 0.5.23 0.50 default, still
 * firmer than the 0.5.20 0.13 graze default.
 * Auto-cal stays on so boxes can settle to different numbers at or below 0.50. */
#define SB_TOUCH_SENS 0.25f
/* Cal: abs(smooth-idle)/idle vs frozen idle (not live benchmark). */
#define SB_TOUCH_CAL_SEE 0.008f
#define SB_TOUCH_CAL_MIN_DELTA 40u
#define SB_TOUCH_CAL_FRAC_V1 0.40f
#define SB_TOUCH_CAL_HOLD_N 15
#define SB_TOUCH_CAL_TIMEOUT_MS 45000
#define SB_CHORD_COMMIT_MS 800
#define SB_CHORD_SPAN_MS 4000
#define SB_CHORD_WIPE 4

static volume_btn_cb_t s_cb;
static volume_gesture_cb_t s_gesture;
static void *s_ctx;
static touch_button_handle_t s_btn[2];
static bool s_up_down;
static bool s_dn_down;
static bool s_chord;
static int s_vol_held;
static int s_chord_count;
static int64_t s_chord_first_ms;
static int64_t s_chord_last_ms;
static int64_t s_last_ms;
static bool s_ready;
static bool s_need_cal;
static bool s_calibrating;
static float s_sens_up = SB_TOUCH_SENS;
static float s_sens_dn = SB_TOUCH_SENS;
static uint8_t s_rev;
static float s_factory_up;
static float s_factory_dn;
static bool s_have_factory;
static bool s_pending_apply;
static float s_want_up;
static float s_want_dn;
static int64_t s_settle_until_ms;
static int64_t s_chatter_t0_ms;
static int s_chatter_n;
static bool s_learn_freeze;

typedef struct {
    bool tracking;
    bool tainted;
    int64_t press_ms;
    float peak;
    /* Bump above the noise floor that may never fire the button. */
    bool watch;
    bool watch_taint;
    bool watch_armed;
    int64_t watch_ms;
    float watch_peak;
} auto_pad_t;

static auto_pad_t s_auto[2];
static float s_first_peaks[2][SB_TOUCH_AUTO_FIRST_N];
static int s_first_n[2];
static bool s_first_done[2];
static float s_leak_peaks[2][SB_TOUCH_AUTO_LEAK_N];
static int s_leak_n[2];
static int s_leak_i[2];
static int s_graze_n[2];
static int64_t s_last_leak_ms[2];

static float peak_to_sens(float peak)
{
    float sens = peak * SB_TOUCH_CAL_FRAC;
    if (sens < SB_TOUCH_CAL_MIN) {
        sens = SB_TOUCH_CAL_MIN;
    }
    if (sens > SB_TOUCH_CAL_MAX) {
        sens = SB_TOUCH_CAL_MAX;
    }
    return sens;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static float live_sens(int idx)
{
    return idx == 0 ? s_sens_up : s_sens_dn;
}

static void auto_on_press(int idx);
static void auto_on_release(int idx);
static void auto_sample(void);
static void auto_flush(void);
static void auto_disarm_watch(void);

static void auto_clear_learn(void)
{
    memset(s_auto, 0, sizeof(s_auto));
    memset(s_first_peaks, 0, sizeof(s_first_peaks));
    memset(s_leak_peaks, 0, sizeof(s_leak_peaks));
    s_first_n[0] = s_first_n[1] = 0;
    s_first_done[0] = s_first_done[1] = false;
    s_leak_n[0] = s_leak_n[1] = 0;
    s_leak_i[0] = s_leak_i[1] = 0;
    s_graze_n[0] = s_graze_n[1] = 0;
    s_last_leak_ms[0] = s_last_leak_ms[1] = 0;
    s_pending_apply = false;
    s_learn_freeze = false;
    s_chatter_n = 0;
    s_chatter_t0_ms = 0;
}

/* ESP32-S3: TOUCH_PAD_NUMn <-> GPIOn for pads 1..14 */
static touch_pad_t gpio_to_touch(int gpio)
{
    if (gpio >= 1 && gpio <= 14) {
        return (touch_pad_t)gpio;
    }
    return TOUCH_PAD_NUM7;
}

static void fire(int delta)
{
    if (!s_cb || delta == 0) {
        return;
    }
    s_last_ms = esp_timer_get_time() / 1000;
    s_cb(delta, s_ctx);
}

static bool both_down(void)
{
    return s_up_down && s_dn_down;
}

static bool both_up(void)
{
    return !s_up_down && !s_dn_down;
}

static void chord_reset(void)
{
    s_chord_count = 0;
    s_chord_first_ms = 0;
    s_chord_last_ms = 0;
}

static void chord_commit(void)
{
    int n = s_chord_count;
    chord_reset();
    if (n < 2 || !s_gesture) {
        return;
    }
    if (n > SB_CHORD_WIPE) {
        n = SB_CHORD_WIPE;
    }
    ESP_LOGI(TAG, "pad chord taps=%d", n);
    s_gesture(n, s_ctx);
}

static void on_chord_complete(int64_t now_ms)
{
    if (s_chord_count > 0 && s_chord_first_ms
        && (now_ms - s_chord_first_ms) >= SB_CHORD_SPAN_MS) {
        chord_commit();
    }
    if (s_chord_count == 0) {
        s_chord_first_ms = now_ms;
    }
    s_chord_count++;
    s_chord_last_ms = now_ms;
    if (s_chord_count >= SB_CHORD_WIPE) {
        chord_commit();
    }
}

static void on_button_event(int delta, touch_button_event_t ev)
{
    bool *pad = (delta > 0) ? &s_up_down : &s_dn_down;

    if (ev == TOUCH_BUTTON_EVT_ON_PRESS) {
        *pad = true;
        ESP_LOGI(TAG, "vol%c press", delta > 0 ? '+' : '-');
        if (both_down()) {
            s_chord = true;
            s_vol_held = 0;
        }
        auto_on_press(delta > 0 ? 0 : 1);
        return;
    }
    if (ev == TOUCH_BUTTON_EVT_ON_RELEASE) {
        *pad = false;
        ESP_LOGI(TAG, "vol%c release", delta > 0 ? '+' : '-');
        if (s_vol_held == delta) {
            s_vol_held = 0;
        }
        auto_on_release(delta > 0 ? 0 : 1);
        if (s_chord && both_up()) {
            s_chord = false;
            on_chord_complete(esp_timer_get_time() / 1000);
        }
    }
}

static esp_err_t make_button(int gpio, int delta, float sens, touch_button_handle_t *out)
{
    touch_button_config_t cfg = {
        .channel_num = gpio_to_touch(gpio),
        .channel_sens = sens,
    };
    esp_err_t err = touch_button_create(&cfg, out);
    if (err != ESP_OK) {
        return err;
    }
    err = touch_button_set_dispatch_method(*out, TOUCH_ELEM_DISP_EVENT);
    if (err != ESP_OK) {
        return err;
    }
    return touch_button_subscribe_event(*out,
                                        TOUCH_ELEM_EVENT_ON_PRESS | TOUCH_ELEM_EVENT_ON_RELEASE,
                                        (void *)(intptr_t)delta);
}

static esp_err_t apply_sens(void)
{
    auto_disarm_watch();
    touch_element_stop();
    if (s_btn[0]) {
        touch_button_delete(s_btn[0]);
        s_btn[0] = NULL;
    }
    if (s_btn[1]) {
        touch_button_delete(s_btn[1]);
        s_btn[1] = NULL;
    }
    esp_err_t err = make_button(board_hw_vol_up_gpio(), +1, s_sens_up, &s_btn[0]);
    if (err == ESP_OK) {
        err = make_button(board_hw_vol_down_gpio(), -1, s_sens_dn, &s_btn[1]);
    }
    if (err == ESP_OK) {
        err = touch_element_start();
    }
    if (err == ESP_OK) {
        s_settle_until_ms = now_ms() + SB_TOUCH_AUTO_SETTLE_MS;
    }
    return err;
}

esp_err_t volume_buttons_init(volume_btn_cb_t cb, volume_gesture_cb_t gesture, void *ctx)
{
    touch_elem_global_config_t global = TOUCH_ELEM_GLOBAL_DEFAULT_CONFIG();
    /* 20 ms timer instead of 10 ms — volume does not need the default rate. */
    global.software.processing_period = 20;

    esp_err_t err = touch_element_install(&global);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_element_install failed: %s", esp_err_to_name(err));
        return err;
    }

    touch_button_global_config_t btn_global = TOUCH_BUTTON_GLOBAL_DEFAULT_CONFIG();
    err = touch_button_install(&btn_global);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_button_install failed: %s", esp_err_to_name(err));
        touch_element_uninstall();
        return err;
    }

    /* Rev 4 = field auto, use it. Rev 3 factory is snapshotted; live starts
     * at SB_TOUCH_SENS (0.25) unless auto already committed. Rev < 3 ignored
     * (0.5.20). */
    uint8_t rev = 0;
    float loaded_up = SB_TOUCH_SENS;
    float loaded_dn = SB_TOUCH_SENS;
    bool loaded = config_store_load_touch_sens(&loaded_up, &loaded_dn, &rev);
    s_have_factory = config_store_load_factory_touch_sens(&s_factory_up,
                                                         &s_factory_dn);
    if (loaded && rev >= SB_TOUCH_AUTO_REV) {
        uint8_t first = 0;
        s_sens_up = loaded_up;
        s_sens_dn = loaded_dn;
        s_rev = rev;
        /* Missing tsens_first (old one-pad write) → neither pad is done. */
        (void)config_store_load_touch_auto_first(&first);
        s_first_done[0] = (first & SB_TOUCH_AUTO_FIRST_UP) != 0;
        s_first_done[1] = (first & SB_TOUCH_AUTO_FIRST_DN) != 0;
    } else {
        if (loaded && rev >= SB_TOUCH_SENS_REV) {
            if (!s_have_factory) {
                config_store_ensure_factory_touch_snapshot(loaded_up, loaded_dn);
                s_factory_up = loaded_up;
                s_factory_dn = loaded_dn;
                s_have_factory = true;
            }
        }
        s_sens_up = SB_TOUCH_SENS;
        s_sens_dn = SB_TOUCH_SENS;
        s_rev = 0;
    }
    s_need_cal = false;
    s_settle_until_ms = now_ms() + SB_TOUCH_AUTO_SETTLE_MS;

    err = make_button(board_hw_vol_up_gpio(), +1, s_sens_up, &s_btn[0]);
    if (err == ESP_OK) {
        err = make_button(board_hw_vol_down_gpio(), -1, s_sens_dn, &s_btn[1]);
    }
    if (err == ESP_OK) {
        err = touch_element_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch button setup failed: %s", esp_err_to_name(err));
        touch_button_uninstall();
        touch_element_uninstall();
        s_btn[0] = NULL;
        s_btn[1] = NULL;
        return err;
    }

    s_cb = cb;
    s_gesture = gesture;
    s_ctx = ctx;
    s_ready = true;
    ESP_LOGI(TAG, "Touch vol-up=T%d vol-down=T%d sens=%.3f/%.3f rev=%u factory=%d first=%d/%d",
             (int)gpio_to_touch(board_hw_vol_up_gpio()),
             (int)gpio_to_touch(board_hw_vol_down_gpio()),
             s_sens_up, s_sens_dn, (unsigned)s_rev, (int)s_have_factory,
             (int)s_first_done[0], (int)s_first_done[1]);
    return ESP_OK;
}

bool volume_buttons_needs_cal(void)
{
    return s_need_cal;
}

void volume_buttons_get_sens(float *up, float *dn)
{
    if (up) {
        *up = s_sens_up;
    }
    if (dn) {
        *dn = s_sens_dn;
    }
}

static uint32_t pad_smooth(touch_pad_t ch)
{
    uint32_t smooth = 0;
    uint32_t raw = 0;
    touch_pad_filter_read_smooth(ch, &smooth);
    if (smooth == 0) {
        touch_pad_read_raw_data(ch, &raw);
        return raw;
    }
    return smooth;
}

static float pad_rel_now(touch_pad_t ch)
{
    uint32_t sm = pad_smooth(ch);
    uint32_t bm = 0;

    touch_pad_read_benchmark(ch, &bm);
    if (bm < 50) {
        return 0;
    }
    uint32_t ad = sm > bm ? sm - bm : bm - sm;
    return (float)ad / (float)bm;
}

static touch_pad_t auto_ch(int idx)
{
    int gpio = idx == 0 ? board_hw_vol_up_gpio() : board_hw_vol_down_gpio();
    return gpio_to_touch(gpio);
}

static void auto_note_chatter(int64_t t)
{
    if (s_chatter_t0_ms == 0 || (t - s_chatter_t0_ms) > SB_TOUCH_AUTO_CHATTER_MS) {
        s_chatter_t0_ms = t;
        s_chatter_n = 0;
    }
    s_chatter_n++;
    if (s_chatter_n >= SB_TOUCH_AUTO_CHATTER_N && !s_learn_freeze) {
        s_learn_freeze = true;
        ESP_LOGW(TAG, "touch auto freeze (pocket/chatter n=%d)", s_chatter_n);
    }
}

static void auto_queue(int idx, float next, const char *why)
{
    float live = live_sens(idx);

    next = touch_auto_clamp_auto(next);
    /* Presses only soften. A firmer proposal is not a teacher. */
    if (!(next < live) || (live - next) < 0.001f) {
        return;
    }
    if (!s_pending_apply) {
        s_want_up = s_sens_up;
        s_want_dn = s_sens_dn;
    }
    if (idx == 0) {
        s_want_up = next;
    } else {
        s_want_dn = next;
    }
    s_pending_apply = true;
    ESP_LOGI(TAG, "touch auto queue %c %.3f -> %.3f (%s)",
             idx == 0 ? '+' : '-', live, next, why);
}

static void auto_step_queue(int idx, float peak, const char *why)
{
    float next = touch_auto_step_softer(live_sens(idx), peak);
    auto_queue(idx, next, why);
}

static void auto_on_press(int idx)
{
    int64_t t = now_ms();
    float rel = pad_rel_now(auto_ch(idx));

    s_auto[idx].tracking = true;
    s_auto[idx].tainted = s_chord || both_down() || s_auto[idx].watch_taint;
    if (s_auto[idx].watch) {
        s_auto[idx].press_ms = s_auto[idx].watch_ms;
        s_auto[idx].peak = s_auto[idx].watch_peak;
    } else {
        s_auto[idx].press_ms = t;
        s_auto[idx].peak = rel;
    }
    if (rel > s_auto[idx].peak) {
        s_auto[idx].peak = rel;
    }
    /* Button owns this contact. Do not also count the falling tail. */
    s_auto[idx].watch = false;
    s_auto[idx].watch_armed = false;
    s_auto[idx].watch_taint = false;
    if (both_down()) {
        s_auto[0].tainted = true;
        s_auto[1].tainted = true;
    }
    auto_note_chatter(t);
}

static void auto_on_release(int idx)
{
    int64_t t = now_ms();
    int hold;
    float peak;
    float would;
    float live;
    bool tainted;
    bool clean;
    bool graze;
    char which = idx == 0 ? '+' : '-';

    if (!s_auto[idx].tracking) {
        return;
    }
    hold = (int)(t - s_auto[idx].press_ms);
    peak = s_auto[idx].peak;
    tainted = s_auto[idx].tainted || s_chord;
    live = live_sens(idx);
    would = touch_auto_step_softer(live, peak);
    clean = !tainted && touch_auto_is_clean(hold, peak);
    graze = !tainted && touch_auto_is_graze(hold, peak, live);
    s_auto[idx].tracking = false;

    ESP_LOGI(TAG,
             "touch auto pad=%c hold_ms=%d peak=%.3f trip=%.3f would=%.3f "
             "live=%.3f first=%d/%d done=%d tainted=%d clean=%d graze=%d",
             which, hold, peak, touch_auto_idf_trip(live), would, live,
             s_first_n[idx], SB_TOUCH_AUTO_FIRST_N, (int)s_first_done[idx],
             (int)tainted, (int)clean, (int)graze);

    if (s_calibrating || s_learn_freeze || t < s_settle_until_ms) {
        return;
    }
    if (tainted || !(peak > SB_TOUCH_CAL_FAIL)) {
        return;
    }
    /* Graze, not-clean, and wet peaks still soften when this press was
     * lighter than the live trip. They never firm the pad. */
    auto_step_queue(idx, peak, "fired");
}

static void auto_disarm_watch(void)
{
    int idx;

    for (idx = 0; idx < 2; idx++) {
        s_auto[idx].watch = false;
        s_auto[idx].watch_taint = false;
        s_auto[idx].watch_armed = false;
        s_auto[idx].watch_peak = 0;
    }
}

static void auto_finish_missed(int idx, int64_t t)
{
    auto_pad_t *p = &s_auto[idx];
    int hold = (int)(t - p->watch_ms);
    float peak = p->watch_peak;
    bool taint = p->watch_taint;
    bool other_down = idx == 0 ? s_dn_down : s_up_down;

    p->watch = false;
    p->watch_peak = 0;
    p->watch_taint = false;
    if (taint || other_down || s_chord || both_down()) {
        return;
    }
    if (hold < SB_TOUCH_AUTO_GRAZE_HOLD_MS
        || hold > SB_TOUCH_AUTO_CLEAN_HOLD_MAX_MS) {
        return;
    }
    if (!(peak > SB_TOUCH_CAL_FAIL)) {
        return;
    }
    if (s_calibrating || t < s_settle_until_ms) {
        return;
    }
    /* Same pocket/chatter guard as a fired press. The trip that crosses
     * 20 events in 30 s does not learn. */
    auto_note_chatter(t);
    if (s_learn_freeze) {
        return;
    }
    auto_step_queue(idx, peak, "missed");
}

static void auto_watch_pad(int idx, int64_t t)
{
    float rel = pad_rel_now(auto_ch(idx));
    float other = pad_rel_now(auto_ch(idx ^ 1));
    auto_pad_t *p = &s_auto[idx];

    if (p->tracking) {
        if (rel > p->peak) {
            p->peak = rel;
        }
        if (other >= SB_TOUCH_AUTO_DUAL_REL) {
            p->tainted = true;
        }
        p->watch = false;
        p->watch_armed = false;
        return;
    }
    if (rel <= SB_TOUCH_CAL_FAIL) {
        if (p->watch) {
            auto_finish_missed(idx, t);
        }
        p->watch_armed = true;
        return;
    }
    if (!p->watch_armed) {
        return;
    }
    if (!p->watch) {
        p->watch = true;
        p->watch_taint = false;
        p->watch_ms = t;
        p->watch_peak = rel;
    } else if (rel > p->watch_peak) {
        p->watch_peak = rel;
    }
    if (other >= SB_TOUCH_AUTO_DUAL_REL) {
        p->watch_taint = true;
    }
}

static void auto_sample(void)
{
    int64_t t = now_ms();
    int idx;

    for (idx = 0; idx < 2; idx++) {
        auto_watch_pad(idx, t);
    }
    if (s_auto[0].tracking && s_auto[1].tracking) {
        s_auto[0].tainted = true;
        s_auto[1].tainted = true;
    }
}

static void auto_flush(void)
{
    float old_up;
    float old_dn;
    esp_err_t err;

    if (!s_pending_apply || s_calibrating || s_chord || !both_up()) {
        return;
    }
    if (s_chord_count > 0) {
        return;
    }
    old_up = s_sens_up;
    old_dn = s_sens_dn;
    s_sens_up = s_want_up;
    s_sens_dn = s_want_dn;
    err = apply_sens();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch auto apply failed (%s), revert %.3f/%.3f",
                 esp_err_to_name(err), old_up, old_dn);
        s_sens_up = old_up;
        s_sens_dn = old_dn;
        (void)apply_sens();
        s_pending_apply = false;
        return;
    }
    {
        uint8_t first = 0;
        if (s_first_done[0]) {
            first |= SB_TOUCH_AUTO_FIRST_UP;
        }
        if (s_first_done[1]) {
            first |= SB_TOUCH_AUTO_FIRST_DN;
        }
        err = config_store_save_touch_sens_auto(s_sens_up, s_sens_dn, first);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch auto persist failed (%s)", esp_err_to_name(err));
    } else {
        s_rev = SB_TOUCH_AUTO_REV;
    }
    s_pending_apply = false;
    ESP_LOGI(TAG, "touch auto settled vol+=%.3f vol-=%.3f rev=%u first=%d/%d",
             s_sens_up, s_sens_dn, (unsigned)s_rev,
             (int)s_first_done[0], (int)s_first_done[1]);
}

void volume_buttons_reset_auto(void)
{
    (void)config_store_reset_auto_touch_sens();
    auto_clear_learn();
    if (config_store_load_factory_touch_sens(&s_factory_up, &s_factory_dn)) {
        s_have_factory = true;
        s_sens_up = s_factory_up;
        s_sens_dn = s_factory_dn;
        s_rev = SB_TOUCH_SENS_REV;
    } else {
        s_have_factory = false;
        s_sens_up = SB_TOUCH_SENS;
        s_sens_dn = SB_TOUCH_SENS;
        s_rev = 0;
    }
    ESP_LOGW(TAG, "touch auto reset live=%.3f/%.3f rev=%u", s_sens_up, s_sens_dn,
             (unsigned)s_rev);
}

void volume_buttons_log_status(void)
{
    log_shipper_printf(
        "touch auto live +=%.3f -=%.3f rev=%u factory=%d "
        "f+=%.3f f-=%.3f first=%d/%d %d/%d leak=%d/%d freeze=%d\n",
        s_sens_up, s_sens_dn, (unsigned)s_rev, (int)s_have_factory,
        s_factory_up, s_factory_dn, s_first_n[0], SB_TOUCH_AUTO_FIRST_N,
        s_first_n[1], SB_TOUCH_AUTO_FIRST_N, s_leak_n[0], s_leak_n[1],
        (int)s_learn_freeze);
}

static bool pad_seen(uint32_t idle, uint32_t now, float *rel_out)
{
    if (idle < 50) {
        *rel_out = 0;
        return false;
    }
    uint32_t ad = now > idle ? (now - idle) : (idle - now);
    float rel = (float)ad / (float)idle;
    *rel_out = rel;
    return (rel >= SB_TOUCH_CAL_SEE) || (ad >= SB_TOUCH_CAL_MIN_DELTA);
}

esp_err_t volume_buttons_set_sens(float up, float dn)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = config_store_save_touch_sens(up, dn);
    if (err != ESP_OK) {
        return err;
    }
    if (!config_store_load_touch_sens(&s_sens_up, &s_sens_dn, &s_rev)) {
        return ESP_FAIL;
    }
    s_factory_up = s_sens_up;
    s_factory_dn = s_sens_dn;
    s_have_factory = true;
    s_first_done[0] = s_first_done[1] = true;
    s_need_cal = false;
    return apply_sens();
}

void volume_buttons_dump_raw(int ms)
{
    if (!s_ready || ms <= 0) {
        return;
    }
    touch_pad_t ch_up = gpio_to_touch(board_hw_vol_up_gpio());
    touch_pad_t ch_dn = gpio_to_touch(board_hw_vol_down_gpio());
    uint32_t idle_up = pad_smooth(ch_up);
    uint32_t idle_dn = pad_smooth(ch_dn);
    log_shipper_printf("touch raw idle +=%u -=%u  press each pad\n",
           (unsigned)idle_up, (unsigned)idle_dn);
    int64_t end = esp_timer_get_time() / 1000 + ms;
    while ((esp_timer_get_time() / 1000) < end) {
        uint32_t u = pad_smooth(ch_up);
        uint32_t d = pad_smooth(ch_dn);
        uint32_t du = u > idle_up ? u - idle_up : idle_up - u;
        uint32_t dd = d > idle_dn ? d - idle_dn : idle_dn - d;
        float ru = idle_up ? (float)du / (float)idle_up : 0;
        float rd = idle_dn ? (float)dd / (float)idle_dn : 0;
        log_shipper_printf("touch raw +=%u d=%u rel=%.4f  -=%u d=%u rel=%.4f\n",
               (unsigned)u, (unsigned)du, ru, (unsigned)d, (unsigned)dd, rd);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void drain_touch_msgs(void)
{
    touch_elem_message_t dump;
    while (touch_element_message_receive(&dump, 0) == ESP_OK) {
    }
}

static void beep_n(int n)
{
    for (int i = 0; i < n; i++) {
        radio_player_beep();
        vTaskDelay(pdMS_TO_TICKS(180));
    }
}

static void cal_fail(const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    log_shipper_printf("%s\n", why);
    led_status_set(SB_LED_RED, 0);
    radio_player_beep_limit();
    vTaskDelay(pdMS_TO_TICKS(800));
    led_status_set(SB_LED_OFF, 0);
}

static bool wait_pad(touch_pad_t ch, uint32_t idle, char which,
                    float *peak_out, int timeout_ms)
{
    int64_t give_up = esp_timer_get_time() / 1000 + timeout_ms;
    int64_t last_log = 0;
    int hold_n = 0;
    float peak = 0;
    while ((esp_timer_get_time() / 1000) < give_up) {
        led_status_tick();
        drain_touch_msgs();
        uint32_t now = pad_smooth(ch);
        float rel = 0;
        bool see = pad_seen(idle, now, &rel);
        if (see && rel > peak) {
            peak = rel;
        }
        int64_t t = esp_timer_get_time() / 1000;
        if (t - last_log >= 500) {
            last_log = t;
            uint32_t ad = now > idle ? now - idle : idle - now;
            ESP_LOGI(TAG, "cal %c d=%u rel=%.4f peak=%.4f hold=%d",
                     which, (unsigned)ad, rel, peak, hold_n);
        }
        if (see) {
            hold_n++;
            if (hold_n >= SB_TOUCH_CAL_HOLD_N) {
                *peak_out = peak;
                return true;
            }
        } else {
            hold_n = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    *peak_out = peak;
    return false;
}

static void wait_release(touch_pad_t ch, uint32_t idle, int timeout_ms)
{
    int64_t give_up = esp_timer_get_time() / 1000 + timeout_ms;
    int up_n = 0;
    while ((esp_timer_get_time() / 1000) < give_up) {
        led_status_tick();
        drain_touch_msgs();
        float rel = 0;
        if (!pad_seen(idle, pad_smooth(ch), &rel)) {
            up_n++;
            if (up_n >= 8) {
                return;
            }
        } else {
            up_n = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t volume_buttons_calibrate(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_calibrating = true;
    s_up_down = false;
    s_dn_down = false;
    s_chord = false;
    s_vol_held = 0;
    chord_reset();
    led_status_set(SB_LED_YELLOW, 0);
    radio_player_ungate();
    touch_pad_t ch_up = gpio_to_touch(board_hw_vol_up_gpio());
    touch_pad_t ch_dn = gpio_to_touch(board_hw_vol_down_gpio());

    log_shipper_printf("cal: hands off\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    uint32_t acc_up = 0;
    uint32_t acc_dn = 0;
    int idle_n = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t u = pad_smooth(ch_up);
        uint32_t d = pad_smooth(ch_dn);
        if (u > 0 && d > 0) {
            acc_up += u;
            acc_dn += d;
            idle_n++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint32_t idle_up = idle_n ? (acc_up / (uint32_t)idle_n) : 0;
    uint32_t idle_dn = idle_n ? (acc_dn / (uint32_t)idle_n) : 0;
    ESP_LOGI(TAG, "cal idle +=%u -=%u", (unsigned)idle_up, (unsigned)idle_dn);

    log_shipper_printf("cal: hold +\n");
    beep_n(1);
    float peak_up = 0;
    if (!wait_pad(ch_up, idle_up, '+', &peak_up, SB_TOUCH_CAL_TIMEOUT_MS)) {
        s_calibrating = false;
        cal_fail("cal fail: no +  (hold volume +)");
        return ESP_FAIL;
    }
    if (peak_up < SB_TOUCH_CAL_FAIL) {
        s_calibrating = false;
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "cal fail: + peak=%.3f (need >=%.3f) — pad too weak",
                 peak_up, SB_TOUCH_CAL_FAIL);
        cal_fail(msg);
        return ESP_FAIL;
    }
    log_shipper_printf("cal: + peak=%.3f\n", peak_up);
    radio_player_beep();
    wait_release(ch_up, idle_up, 4000);

    led_status_set(SB_LED_YELLOW, 200);
    log_shipper_printf("cal: hold -\n");
    beep_n(2);
    float peak_dn = 0;
    if (!wait_pad(ch_dn, idle_dn, '-', &peak_dn, SB_TOUCH_CAL_TIMEOUT_MS)) {
        s_calibrating = false;
        cal_fail("cal fail: no -  (hold volume -)");
        return ESP_FAIL;
    }
    if (peak_dn < SB_TOUCH_CAL_FAIL) {
        s_calibrating = false;
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "cal fail: - peak=%.3f (need >=%.3f) — pad too weak",
                 peak_dn, SB_TOUCH_CAL_FAIL);
        cal_fail(msg);
        return ESP_FAIL;
    }
    log_shipper_printf("cal: - peak=%.3f\n", peak_dn);

    s_sens_up = peak_to_sens(peak_up);
    s_sens_dn = peak_to_sens(peak_dn);
    config_store_save_touch_sens(s_sens_up, s_sens_dn);
    s_factory_up = s_sens_up;
    s_factory_dn = s_sens_dn;
    s_have_factory = true;
    s_rev = SB_TOUCH_SENS_REV;
    s_first_done[0] = s_first_done[1] = true;
    esp_err_t err = apply_sens();
    s_need_cal = false;
    s_calibrating = false;
    led_status_set(SB_LED_GREEN, 0);
    beep_n(2);
    vTaskDelay(pdMS_TO_TICKS(300));
    led_status_set(SB_LED_OFF, 0);
    ESP_LOGI(TAG, "cal done up=%.3f dn=%.3f (peak %.3f/%.3f)",
             s_sens_up, s_sens_dn, peak_up, peak_dn);
    log_shipper_printf("cal done vol+=%.3f vol-=%.3f\n", s_sens_up, s_sens_dn);
    return err;
}

void volume_buttons_poll(void)
{
    if (!s_ready) {
        return;
    }
    if (s_calibrating) {
        touch_elem_message_t dump;
        while (touch_element_message_receive(&dump, 0) == ESP_OK) {
        }
        return;
    }

    touch_elem_message_t msg;
    while (touch_element_message_receive(&msg, 0) == ESP_OK) {
        if (msg.element_type != TOUCH_ELEM_TYPE_BUTTON) {
            continue;
        }
        const touch_button_message_t *btn = touch_button_get_message(&msg);
        if (btn) {
            on_button_event((int)(intptr_t)msg.arg, btn->event);
        }
    }

    auto_sample();

    int64_t now = esp_timer_get_time() / 1000;
    if (s_chord_count > 0 && both_up() && !s_chord
        && (now - s_chord_last_ms) >= SB_CHORD_COMMIT_MS) {
        chord_commit();
    }

    auto_flush();

    /* Drain the queue first so a same-cycle overlap becomes a chord, not a
     * volume tick. Stay silent until both pads are up after a chord. */
    if (s_chord || both_down() || s_chord_count > 0) {
        s_vol_held = 0;
        return;
    }

    int delta = 0;
    if (s_up_down && !s_dn_down) {
        delta = +1;
    } else if (s_dn_down && !s_up_down) {
        delta = -1;
    }
    if (delta == 0) {
        s_vol_held = 0;
        return;
    }

    if (s_vol_held != delta) {
        s_vol_held = delta;
        fire(delta);
    } else if ((now - s_last_ms) >= SB_DEBOUNCE_MS) {
        fire(delta);
    }
}
