#include "led_status.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "led_strip.h"
#include "esp_timer.h"

static const char *TAG = "led_status";
static led_strip_handle_t s_strip;
static sb_led_color_t s_color = SB_LED_OFF;
static int s_blink_ms = 0;
static bool s_on = false;
static int64_t s_last_toggle_us = 0;

static void apply_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

esp_err_t led_status_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_SEARABOOM_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "LED on GPIO %d", CONFIG_SEARABOOM_LED_GPIO);
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
    if (s_blink_ms <= 0 || s_color == SB_LED_OFF || !s_strip) {
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