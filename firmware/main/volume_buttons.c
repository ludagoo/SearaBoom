#include "volume_buttons.h"
#include "searaboom.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/touch_pad.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "volume_buttons";
static volume_btn_cb_t s_cb;
static void *s_ctx;
static int64_t s_last_ms;
static uint32_t s_base_up;
static uint32_t s_base_down;
static uint32_t s_thresh_up;
static uint32_t s_thresh_down;

/* Original BoomV5S3Zero.ino uses capacitive T6/T7 (GPIO6/7), not digital GPIO. */
static touch_pad_t gpio_to_touch(int gpio)
{
    /* ESP32-S3: TOUCH_PAD_NUMn <-> GPIOn for pads 1..14 */
    if (gpio >= 1 && gpio <= 14) {
        return (touch_pad_t)gpio;
    }
    return TOUCH_PAD_NUM7;
}

static uint32_t read_pad(touch_pad_t pad)
{
    uint32_t raw = 0;
    touch_pad_read_raw_data(pad, &raw);
    return raw;
}

static uint32_t sample_baseline(touch_pad_t pad)
{
    uint64_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += read_pad(pad);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (uint32_t)(sum / 16);
}

esp_err_t volume_buttons_init(volume_btn_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;

    touch_pad_t up = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_UP);
    touch_pad_t down = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_DOWN);

    ESP_ERROR_CHECK(touch_pad_init());
    ESP_ERROR_CHECK(touch_pad_config(up));
    ESP_ERROR_CHECK(touch_pad_config(down));

    /* Denoise uses extra RAM; skip it so AAC has room to init. */
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();
    vTaskDelay(pdMS_TO_TICKS(120));

    s_base_up = sample_baseline(up);
    s_base_down = sample_baseline(down);
    /* Arduino used absolute threshold 1500; on S3 use delta from idle baseline. */
    s_thresh_up = s_base_up / 8;
    s_thresh_down = s_base_down / 8;
    if (s_thresh_up < 800) {
        s_thresh_up = 800;
    }
    if (s_thresh_down < 800) {
        s_thresh_down = 800;
    }

    ESP_LOGI(TAG, "Touch vol-up=T%d base=%" PRIu32 " thr=%" PRIu32
                  " vol-down=T%d base=%" PRIu32 " thr=%" PRIu32,
             (int)up, s_base_up, s_thresh_up, (int)down, s_base_down, s_thresh_down);
    return ESP_OK;
}

void volume_buttons_poll(void)
{
    if (!s_cb) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if ((now - s_last_ms) < SB_DEBOUNCE_MS) {
        return;
    }

    touch_pad_t up = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_UP);
    touch_pad_t down = gpio_to_touch(CONFIG_SEARABOOM_BTN_VOL_DOWN);
    uint32_t v_up = read_pad(up);
    uint32_t v_down = read_pad(down);

    /* S3 touch raw usually rises when touched; also accept drop for pad variance. */
    bool up_hit = (v_up > s_base_up + s_thresh_up) || (v_up + s_thresh_up < s_base_up);
    bool down_hit = (v_down > s_base_down + s_thresh_down) || (v_down + s_thresh_down < s_base_down);

    if (up_hit && !down_hit) {
        s_last_ms = now;
        ESP_LOGI(TAG, "vol+ touch raw=%" PRIu32, v_up);
        s_cb(+1, s_ctx);
    } else if (down_hit && !up_hit) {
        s_last_ms = now;
        ESP_LOGI(TAG, "vol- touch raw=%" PRIu32, v_down);
        s_cb(-1, s_ctx);
    }
}
