#include "led_status.h"
#include "board.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "led_strip.h"
#include "esp_timer.h"

static const char *TAG = "led_status";
static led_strip_handle_t s_strip[2];
static sb_led_color_t s_color = SB_LED_OFF;
static int s_blink_ms = 0;
static bool s_on = false;
static int64_t s_last_toggle_us = 0;

static void apply_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 2; i++) {
        if (!s_strip[i]) {
            continue;
        }
        led_strip_set_pixel(s_strip[i], 0, r, g, b);
        led_strip_refresh(s_strip[i]);
    }
}

static esp_err_t add_strip(int gpio, int slot)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip[slot]);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED GPIO %d init failed: %s", gpio, esp_err_to_name(err));
        s_strip[slot] = NULL;
    }
    return err;
}

esp_err_t led_status_init(void)
{
    /* Zero WS2812 is GPIO 21, SuperMini is 48. The other pad is unused on
     * each carrier; driving both as WS2812 is safe and lights whichever
     * board this binary is on without a perfect load-sense. */
    (void)add_strip(21, 0);
    (void)add_strip(48, 1);
    if (!s_strip[0] && !s_strip[1]) {
        ESP_LOGE(TAG, "led_strip init failed on 21 and 48");
        return ESP_FAIL;
    }
    apply_rgb(0, 0, 0);
    ESP_LOGI(TAG, "LED GPIO 21%s + 48%s (%s, prefer %d)",
             s_strip[0] ? "" : "(off)", s_strip[1] ? "" : "(off)",
             board_hw_name(), board_hw_led_gpio());
    return ESP_OK;
}

void led_status_set(sb_led_color_t color, int blink_ms)
{
    s_color = color;
    s_blink_ms = blink_ms;
    s_on = true;
    s_last_toggle_us = esp_timer_get_time();

    if (color == SB_LED_OFF || blink_ms == 0) {
        /* solid */
    }

    uint8_t r = 0, g = 0, b = 0;
    switch (color) {
    case SB_LED_WHITE: r = 128; g = 128; b = 128; break;
    case SB_LED_RED: r = 255; break;
    case SB_LED_GREEN: g = 255; break;
    case SB_LED_BLUE: b = 255; break;
    case SB_LED_YELLOW: r = 255; g = 180; break;
    case SB_LED_MAGENTA: r = 255; b = 255; break;
    default: break;
    }
    if (color == SB_LED_OFF) {
        apply_rgb(0, 0, 0);
    } else {
        apply_rgb(r, g, b);
    }
}

static void color_to_rgb(sb_led_color_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = *g = *b = 0;
    switch (color) {
    case SB_LED_WHITE: *r = 128; *g = 128; *b = 128; break;
    case SB_LED_RED: *r = 255; break;
    case SB_LED_GREEN: *g = 255; break;
    case SB_LED_BLUE: *b = 255; break;
    case SB_LED_YELLOW: *r = 255; *g = 180; break;
    case SB_LED_MAGENTA: *r = 255; *b = 255; break;
    default: break;
    }
}

void led_status_tick(void)
{
    if (s_blink_ms <= 0 || s_color == SB_LED_OFF || (!s_strip[0] && !s_strip[1])) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if ((now - s_last_toggle_us) < ((int64_t)s_blink_ms * 1000)) {
        return;
    }
    s_last_toggle_us = now;
    s_on = !s_on;
    if (!s_on) {
        apply_rgb(0, 0, 0);
        return;
    }
    uint8_t r, g, b;
    color_to_rgb(s_color, &r, &g, &b);
    apply_rgb(r, g, b);
}