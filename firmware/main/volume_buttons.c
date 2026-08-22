#include "volume_buttons.h"
#include "searaboom.h"
#include "config_store.h"
#include "radio_player.h"
#include "led_status.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "touch_element/touch_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "volume_buttons";

/* Fraction of idle benchmark. Official S3 button example uses 0.1. */
#define SB_TOUCH_SENS 0.1f
/* Cal: abs(smooth-idle)/idle vs frozen idle (not live benchmark). */
#define SB_TOUCH_CAL_SEE 0.008f
#define SB_TOUCH_CAL_MIN_DELTA 40u
#define SB_TOUCH_CAL_FRAC 0.40f
#define SB_TOUCH_CAL_MIN 0.012f
#define SB_TOUCH_CAL_MAX 0.35f
/* Below this peak a dedicated finger is a dead pad, not a sensitivity tweak. */
#define SB_TOUCH_CAL_FAIL 0.025f
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
        return;
    }
    if (ev == TOUCH_BUTTON_EVT_ON_RELEASE) {
        *pad = false;
        ESP_LOGI(TAG, "vol%c release", delta > 0 ? '+' : '-');
        if (s_vol_held == delta) {
            s_vol_held = 0;
        }
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

    if (!config_store_load_touch_sens(&s_sens_up, &s_sens_dn)) {
        s_sens_up = SB_TOUCH_SENS;
        s_sens_dn = SB_TOUCH_SENS;
        s_need_cal = true;
    }

    err = make_button(CONFIG_SEARABOOM_BTN_VOL_UP, +1, s_sens_up, &s_btn[0]);
    if (err == ESP_OK) {
        err = make_button(CONFIG_SEARABOOM_BTN_VOL_DOWN, -1, s_sens_dn, &s_btn[1]);
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
    ESP_LOGI(TAG, "Touch vol-up=T%d vol-down=T%d sens=%.3f/%.3f cal=%d",
             (int)gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_UP),
             (int)gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_DOWN),
             s_sens_up, s_sens_dn, (int)s_need_cal);
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

static esp_err_t apply_sens(void)
{
    touch_element_stop();
    if (s_btn[0]) {
        touch_button_delete(s_btn[0]);
        s_btn[0] = NULL;
    }
    if (s_btn[1]) {
        touch_button_delete(s_btn[1]);
        s_btn[1] = NULL;
    }
    esp_err_t err = make_button(CONFIG_SEARABOOM_BTN_VOL_UP, +1, s_sens_up, &s_btn[0]);
    if (err == ESP_OK) {
        err = make_button(CONFIG_SEARABOOM_BTN_VOL_DOWN, -1, s_sens_dn, &s_btn[1]);
    }
    if (err == ESP_OK) {
        err = touch_element_start();
    }
    return err;
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
    if (!config_store_load_touch_sens(&s_sens_up, &s_sens_dn)) {
        return ESP_FAIL;
    }
    s_need_cal = false;
    return apply_sens();
}

void volume_buttons_dump_raw(int ms)
{
    if (!s_ready || ms <= 0) {
        return;
    }
    touch_pad_t ch_up = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_UP);
    touch_pad_t ch_dn = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_DOWN);
    uint32_t idle_up = pad_smooth(ch_up);
    uint32_t idle_dn = pad_smooth(ch_dn);
    printf("touch raw idle +=%u -=%u  press each pad\n",
           (unsigned)idle_up, (unsigned)idle_dn);
    int64_t end = esp_timer_get_time() / 1000 + ms;
    while ((esp_timer_get_time() / 1000) < end) {
        uint32_t u = pad_smooth(ch_up);
        uint32_t d = pad_smooth(ch_dn);
        uint32_t du = u > idle_up ? u - idle_up : idle_up - u;
        uint32_t dd = d > idle_dn ? d - idle_dn : idle_dn - d;
        float ru = idle_up ? (float)du / (float)idle_up : 0;
        float rd = idle_dn ? (float)dd / (float)idle_dn : 0;
        printf("touch raw +=%u d=%u rel=%.4f  -=%u d=%u rel=%.4f\n",
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
    printf("%s\n", why);
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
    touch_pad_t ch_up = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_UP);
    touch_pad_t ch_dn = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_DOWN);

    printf("cal: hands off\n");
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

    printf("cal: hold +\n");
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
    printf("cal: + peak=%.3f\n", peak_up);
    radio_player_beep();
    wait_release(ch_up, idle_up, 4000);

    led_status_set(SB_LED_YELLOW, 200);
    printf("cal: hold -\n");
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
    printf("cal: - peak=%.3f\n", peak_dn);

    s_sens_up = peak_to_sens(peak_up);
    s_sens_dn = peak_to_sens(peak_dn);
    config_store_save_touch_sens(s_sens_up, s_sens_dn);
    esp_err_t err = apply_sens();
    s_need_cal = false;
    s_calibrating = false;
    led_status_set(SB_LED_GREEN, 0);
    beep_n(2);
    vTaskDelay(pdMS_TO_TICKS(300));
    led_status_set(SB_LED_OFF, 0);
    ESP_LOGI(TAG, "cal done up=%.3f dn=%.3f (peak %.3f/%.3f)",
             s_sens_up, s_sens_dn, peak_up, peak_dn);
    printf("cal done vol+=%.3f vol-=%.3f\n", s_sens_up, s_sens_dn);
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

    int64_t now = esp_timer_get_time() / 1000;
    if (s_chord_count > 0 && both_up() && !s_chord
        && (now - s_chord_last_ms) >= SB_CHORD_COMMIT_MS) {
        chord_commit();
    }

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
