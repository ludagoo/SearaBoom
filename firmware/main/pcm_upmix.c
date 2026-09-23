#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcm_upmix.h"
#include "audio_element.h"
#include "audio_mem.h"
#include "esp_log.h"

static const char *TAG = "pcm_upmix";
#define SB_OUT_SR 44100

static esp_err_t upmix_open(audio_element_handle_t self)
{
    audio_element_info_t info = {0};
    audio_element_getinfo(self, &info);
    if (info.sample_rates == 0) {
        info.sample_rates = SB_OUT_SR;
        info.bits = 16;
    }
    /* channels == 0 means "not yet": do not guess mono. Stereo-as-mono
     * plays an octave low (anti-Mickey-Mouse) until music_info arrives. */
    if (info.bits == 0) {
        info.bits = 16;
    }
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int upmix_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    audio_element_info_t info = {0};
    audio_element_getinfo(self, &info);
    if (info.channels < 1) {
        /* Must yield: ADF treats TIMEOUT as "try again now" and this
         * task would otherwise busy-spin, starve IDLE0/I2S/Wi-Fi. */
        vTaskDelay(pdMS_TO_TICKS(10));
        return AEL_IO_TIMEOUT;
    }
    int r = audio_element_input(self, in_buffer, in_len);
    if (r <= 0) {
        return r;
    }
    int in_ch = info.channels >= 2 ? 2 : 1;
    int in_rate = info.sample_rates > 0 ? info.sample_rates : SB_OUT_SR;
    int in_frames = r / (2 * in_ch);
    if (in_frames <= 0) {
        return r;
    }
    const int16_t *in = (const int16_t *)in_buffer;
    if (in_ch >= 2 && in_rate == SB_OUT_SR) {
        return audio_element_output(self, in_buffer, r);
    }

    int16_t *stereo = (int16_t *)in;
    int16_t *owned = NULL;
    if (in_ch < 2) {
        owned = audio_malloc((size_t)in_frames * 2 * sizeof(int16_t));
        if (!owned) {
            return AEL_IO_FAIL;
        }
        for (int i = 0; i < in_frames; i++) {
            owned[i * 2] = in[i];
            owned[i * 2 + 1] = in[i];
        }
        stereo = owned;
    }
    if (in_rate == SB_OUT_SR) {
        int w = audio_element_output(self, (char *)stereo, in_frames * 4);
        audio_free(owned);
        return w;
    }

    int out_frames = (int)(((int64_t)in_frames * SB_OUT_SR) / in_rate);
    if (out_frames < 1) {
        out_frames = 1;
    }
    int16_t *out = audio_malloc((size_t)out_frames * 2 * sizeof(int16_t));
    if (!out) {
        audio_free(owned);
        return AEL_IO_FAIL;
    }
    for (int i = 0; i < out_frames; i++) {
        int src = (int)(((int64_t)i * in_rate) / SB_OUT_SR);
        if (src >= in_frames) {
            src = in_frames - 1;
        }
        out[i * 2] = stereo[src * 2];
        out[i * 2 + 1] = stereo[src * 2 + 1];
    }
    int w = audio_element_output(self, (char *)out, out_frames * 4);
    audio_free(out);
    audio_free(owned);
    return w;
}

audio_element_handle_t pcm_upmix_init_core(const char *tag, int core)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = upmix_open;
    cfg.process = upmix_process;
    cfg.tag = tag ? tag : "m2s";
    cfg.out_rb_size = PCM_UPMIX_OUT_RB_SIZE;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 5;
    cfg.task_core = (core == 1) ? 1 : 0;
    cfg.stack_in_ext = true;
    cfg.buffer_len = 2048;
    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        ESP_LOGE(TAG, "init failed");
    }
    return el;
}

audio_element_handle_t pcm_upmix_init(const char *tag)
{
    return pcm_upmix_init_core(tag, 0);
}
