#include "volume_buttons.h"
#include "searaboom.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "touch_element/touch_button.h"
#include <stdint.h>

static const char *TAG = "volume_buttons";

/* Fraction of idle benchmark. Official S3 button example uses 0.1. */
#define SB_TOUCH_SENS 0.1f
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

static esp_err_t make_button(int gpio, int delta, touch_button_handle_t *out)
{
    touch_button_config_t cfg = {
        .channel_num = gpio_to_touch(gpio),
        .channel_sens = SB_TOUCH_SENS,
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

    err = make_button(CONFIG_SEARABOOM_BTN_VOL_UP, +1, &s_btn[0]);
    if (err == ESP_OK) {
        err = make_button(CONFIG_SEARABOOM_BTN_VOL_DOWN, -1, &s_btn[1]);
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
    ESP_LOGI(TAG, "Touch vol-up=T%d vol-down=T%d (touch_element)",
             (int)gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_UP),
             (int)gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_DOWN));
    return ESP_OK;
}

void volume_buttons_poll(void)
{
    if (!s_ready) {
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
