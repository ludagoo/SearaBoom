#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_element.h"
#include "audio_common.h"
#include "aac_inject.h"

static const char *TAG = "aac_inject";

typedef enum {
    INJ_PASS = 0,
    INJ_HOLD,
    INJ_CLIP,
    INJ_GAP,
} inj_mode_t;

static audio_element_handle_t s_el;
static aac_inject_hooks_t s_hooks;
static volatile inj_mode_t s_mode = INJ_HOLD;
static volatile bool s_passthrough;
static volatile bool s_playing;
static volatile bool s_started;
static volatile bool s_loop;
static const uint8_t *s_data;
static size_t s_len;
static size_t s_pos;
static int s_gap_ms;
static int64_t s_gap_until;
static TaskHandle_t s_waiter;
static SemaphoreHandle_t s_mu;

static void lock(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

static void notify_done(void)
{
    s_playing = false;
    s_started = false;
    if (s_hooks.on_clip) {
        s_hooks.on_clip(false);
    }
    if (s_waiter) {
        xTaskNotifyGive(s_waiter);
        s_waiter = NULL;
    }
}

static void clip_ended_locked(void)
{
    if (s_loop) {
        if (s_gap_ms > 0) {
            s_mode = INJ_GAP;
            s_gap_until = esp_timer_get_time() + (int64_t)s_gap_ms * 1000;
        } else {
            s_pos = 0;
            s_mode = INJ_CLIP;
        }
        return;
    }
    s_data = NULL;
    s_len = 0;
    s_pos = 0;
    s_mode = s_passthrough ? INJ_PASS : INJ_HOLD;
    notify_done();
}

static esp_err_t inject_open(audio_element_handle_t self)
{
    audio_element_info_t info = {0};
    info.sample_rates = 22050;
    info.bits = 16;
    info.channels = 1;
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int inject_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    inj_mode_t mode = s_mode;
    if (mode == INJ_HOLD) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return AEL_IO_TIMEOUT;
    }
    if (mode == INJ_GAP) {
        if (esp_timer_get_time() >= s_gap_until) {
            lock();
            s_pos = 0;
            s_mode = INJ_CLIP;
            unlock();
            mode = INJ_CLIP;
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
            return AEL_IO_TIMEOUT;
        }
    }
    if (mode == INJ_CLIP) {
        lock();
        const uint8_t *data = s_data;
        size_t len = s_len;
        size_t pos = s_pos;
        unlock();
        if (!data || pos >= len) {
            lock();
            clip_ended_locked();
            unlock();
            return AEL_IO_TIMEOUT;
        }
        int n = in_len;
        if ((size_t)n > len - pos) {
            n = (int)(len - pos);
        }
        memcpy(in_buffer, data + pos, (size_t)n);
        lock();
        s_pos += (size_t)n;
        bool done = (s_pos >= s_len);
        s_started = true;
        unlock();
        int w = audio_element_output(self, in_buffer, n);
        if (done) {
            lock();
            clip_ended_locked();
            unlock();
        }
        return w;
    }

    int r = audio_element_input(self, in_buffer, in_len);
    if (r == AEL_IO_TIMEOUT) {
        return r;
    }
    if (r <= 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return AEL_IO_TIMEOUT;
    }
    return audio_element_output(self, in_buffer, r);
}

audio_element_handle_t aac_inject_init(void)
{
    if (s_el) {
        return s_el;
    }
    s_mu = xSemaphoreCreateMutex();
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = inject_open;
    cfg.process = inject_process;
    cfg.tag = "inj";
    cfg.out_rb_size = 16 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 6;
    cfg.task_core = 0;
    cfg.stack_in_ext = false;
    cfg.buffer_len = 2048;
    s_el = audio_element_init(&cfg);
    s_mode = INJ_HOLD;
    return s_el;
}

void aac_inject_deinit(void)
{
    aac_inject_stop();
    if (s_el) {
        audio_element_deinit(s_el);
        s_el = NULL;
    }
}

audio_element_handle_t aac_inject_element(void)
{
    return s_el;
}

void aac_inject_set_hooks(const aac_inject_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
}

void aac_inject_set_passthrough(bool on)
{
    s_passthrough = on;
    if (!s_playing) {
        s_mode = on ? INJ_PASS : INJ_HOLD;
    }
}

esp_err_t aac_inject_play(const uint8_t *data, size_t len, bool loop, int gap_ms)
{
    if (!s_el || !data || len < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool live = s_playing || s_mode == INJ_CLIP || s_mode == INJ_GAP;
    s_playing = true;
    s_data = data;
    s_len = len;
    s_pos = 0;
    s_loop = loop;
    s_gap_ms = gap_ms > 0 ? gap_ms : 0;
    if (live) {
        /* Same-rate UI clips: swap ADTS in place. Decoder reset + i2s_set_clk
         * pauses I2S and drops SoftAP clients. */
        s_mode = INJ_CLIP;
        unlock();
        ESP_LOGI(TAG, "swap clip %u bytes loop=%d gap=%d", (unsigned)len, (int)loop, gap_ms);
        return ESP_OK;
    }
    s_started = false;
    s_mode = INJ_CLIP;
    unlock();
    if (s_hooks.on_clip) {
        s_hooks.on_clip(true);
    }
    ESP_LOGI(TAG, "inject clip %u bytes loop=%d gap=%d", (unsigned)len, (int)loop, gap_ms);
    return ESP_OK;
}

void aac_inject_stop(void)
{
    lock();
    s_data = NULL;
    s_len = 0;
    s_pos = 0;
    s_loop = false;
    s_mode = s_passthrough ? INJ_PASS : INJ_HOLD;
    bool was = s_playing;
    unlock();
    if (was) {
        notify_done();
    }
}

bool aac_inject_is_playing(void)
{
    return s_playing;
}

bool aac_inject_has_started(void)
{
    return s_started;
}

esp_err_t aac_inject_wait_done(int timeout_ms)
{
    if (!s_playing) {
        return ESP_OK;
    }
    s_waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, 0);
    uint32_t ticks = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    if (ulTaskNotifyTake(pdTRUE, ticks) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
