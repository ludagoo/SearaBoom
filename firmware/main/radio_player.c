#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "radio_player.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_mem.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "aac_decoder.h"
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

static const char *TAG = "radio_player";

static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_http;
static audio_element_handle_t s_aac;
static audio_element_handle_t s_m2s;
static audio_element_handle_t s_i2s;
static audio_event_iface_handle_t s_evt;
static int s_volume = SB_DEFAULT_VOLUME;
static bool s_running;
static bool s_got_music_info;
static int s_sample_rate = 22050;
static volatile int s_beep_frames;
static uint32_t s_beep_phase;
static int s_beep_total_frames;

#define SB_BEEP_HZ 2000
#define SB_BEEP_MS 55
#define SB_BEEP_AMP 4200

/* Quarter-wave sine, 0..63 → 0..32767; gentler than a square click. */
static const int16_t s_sin_q[64] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5600, 6389, 7173, 7952, 8724, 9490, 10249, 11000, 11743,
    12476, 13200, 13914, 14617, 15308, 15988, 16655, 17309, 17949, 18575, 19186, 19782, 20362, 20926, 21473, 22003,
    22516, 23011, 23488, 23946, 24385, 24805, 25205, 25585, 25945, 26284, 26602, 26900, 27176, 27431, 27665, 27876,
    28066, 28234, 28379, 28502, 28603, 28681, 28736, 28769, 28779, 28766, 28731, 28673, 28593, 28490, 28365, 28217,
};

static int16_t soft_sine(uint32_t phase)
{
    unsigned idx = (phase >> 10) & 0xFFu;
    unsigned quad = idx >> 6;
    unsigned i = idx & 63;
    int16_t v = s_sin_q[i];
    if (quad == 1) {
        v = s_sin_q[63 - i];
    } else if (quad == 2) {
        v = (int16_t)(-s_sin_q[i]);
    } else if (quad == 3) {
        v = (int16_t)(-s_sin_q[63 - i]);
    }
    return v;
}

bool radio_player_has_music_info(void)
{
    return s_got_music_info;
}

void radio_player_beep(void)
{
    if (!s_running) {
        return;
    }
    int rate = s_sample_rate > 0 ? s_sample_rate : 22050;
    s_beep_total_frames = (rate * SB_BEEP_MS) / 1000;
    if (s_beep_total_frames < 16) {
        s_beep_total_frames = 16;
    }
    s_beep_phase = 0;
    s_beep_frames = s_beep_total_frames;
}

static void mix_beep_s16le(int16_t *samples, int frames, int channels)
{
    if (s_beep_frames <= 0 || channels < 1) {
        return;
    }
    int rate = s_sample_rate > 0 ? s_sample_rate : 22050;
    uint32_t incr = (uint32_t)((SB_BEEP_HZ * 65536u) / (unsigned)rate);
    int total = s_beep_total_frames > 0 ? s_beep_total_frames : 1;
    int edge = total / 3;
    if (edge < 1) {
        edge = 1;
    }

    for (int i = 0; i < frames && s_beep_frames > 0; i++) {
        int left = s_beep_frames;
        int elapsed = total - left;
        int amp = SB_BEEP_AMP;
        if (elapsed < edge) {
            amp = (amp * elapsed) / edge;
        } else if (left < edge) {
            amp = (amp * left) / edge;
        }
        int16_t tone = (int16_t)(((int32_t)soft_sine(s_beep_phase) * amp) / 32767);
        s_beep_phase += incr;
        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            int32_t mixed = (int32_t)samples[idx] + tone;
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            samples[idx] = (int16_t)mixed;
        }
        s_beep_frames--;
    }
}

/*
 * Arduino ESP32-audioI2S tops out near 0 dB full-scale stereo.
 * On this MAX98357 + Seara stream, +9 dB clipped; NVS showed the user's
 * clean ceiling at step 18 on that curve (= +2 dB). Cap the scale there.
 */
#define SB_ALC_MIN_DB (-36)
#define SB_ALC_MAX_DB (2)

static int volume_to_alc(int volume)
{
    if (volume < SB_VOLUME_MIN) {
        volume = SB_VOLUME_MIN;
    }
    if (volume > SB_VOLUME_MAX) {
        volume = SB_VOLUME_MAX;
    }
    return SB_ALC_MIN_DB
        + ((volume - 1) * (SB_ALC_MAX_DB - SB_ALC_MIN_DB)) / (SB_VOLUME_MAX - 1);
}

esp_err_t radio_player_set_volume(int volume)
{
    if (volume < SB_VOLUME_MIN) {
        volume = SB_VOLUME_MIN;
    }
    if (volume > SB_VOLUME_MAX) {
        volume = SB_VOLUME_MAX;
    }
    s_volume = volume;
    if (s_i2s) {
        i2s_alc_volume_set(s_i2s, volume_to_alc(volume));
    }
    ESP_LOGI(TAG, "volume=%d alc=%d dB", s_volume, volume_to_alc(s_volume));
    return ESP_OK;
}

int radio_player_get_volume(void)
{
    return s_volume;
}

/* Duplicate mono PCM to stereo — matches Arduino Audio stereo I2S output. */
static esp_err_t m2s_open(audio_element_handle_t self)
{
    audio_element_info_t info = {0};
    audio_element_getinfo(self, &info);
    if (info.sample_rates == 0) {
        info.sample_rates = 22050;
        info.bits = 16;
        info.channels = 1;
    }
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int m2s_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    if (r_size <= 0) {
        return r_size;
    }

    audio_element_info_t info = {0};
    audio_element_getinfo(self, &info);
    int channels = info.channels > 0 ? info.channels : 1;
    int bps = (info.bits > 0 ? info.bits : 16) / 8;
    if (bps <= 0) {
        bps = 2;
    }

    if (channels >= 2) {
        if (bps == 2 && s_beep_frames > 0) {
            mix_beep_s16le((int16_t *)in_buffer, r_size / (bps * channels), channels);
        }
        return audio_element_output(self, in_buffer, r_size);
    }

    int samples = r_size / bps;
    int out_bytes = samples * bps * 2;
    char *out = audio_malloc(out_bytes);
    if (!out) {
        return AEL_IO_FAIL;
    }
    for (int i = 0; i < samples; i++) {
        memcpy(out + (i * 2) * bps, in_buffer + i * bps, bps);
        memcpy(out + (i * 2 + 1) * bps, in_buffer + i * bps, bps);
    }
    if (bps == 2 && s_beep_frames > 0) {
        mix_beep_s16le((int16_t *)out, samples, 2);
    }
    int w = audio_element_output(self, out, out_bytes);
    audio_free(out);
    return w;
}

static audio_element_handle_t mono_to_stereo_init(void)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = m2s_open;
    cfg.process = m2s_process;
    cfg.tag = "m2s";
    cfg.out_rb_size = 8 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 5;
    cfg.task_core = 0;
    cfg.stack_in_ext = false; /* PSRAM stacks fail RestrictedPinnedToCore on this board */
    cfg.buffer_len = 2048;
    return audio_element_init(&cfg);
}

static int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        esp_http_client_handle_t client = (esp_http_client_handle_t)msg->http_client;
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "User-Agent", "SearaBoom/1.0");
    }
    if (msg->event_id == HTTP_STREAM_POST_REQUEST
        || msg->event_id == HTTP_STREAM_ON_RESPONSE
        || msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        audio_element_set_codec_fmt(msg->el, ESP_CODEC_TYPE_UNKNOW);
    }
    return ESP_OK;
}

esp_err_t radio_player_start(const char *url, int volume)
{
    if (s_running) {
        radio_player_stop();
    }

    ESP_LOGI(TAG, "Start stream %s", url);

    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle && board_handle->audio_hal) {
        audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    }

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipeline_cfg);

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = false;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.event_handle = _http_stream_event_handle;
    http_cfg.user_agent = "SearaBoom/1.0";
    http_cfg.out_rb_size = 16 * 1024;
    http_cfg.task_stack = 5 * 1024;
    http_cfg.stack_in_ext = false;
    s_http = http_stream_init(&http_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = true;
    i2s_cfg.volume = volume_to_alc(volume);
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_RIGHT_LEFT);
    s_i2s = i2s_stream_init(&i2s_cfg);

    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    aac_cfg.plus_enable = false;
    aac_cfg.out_rb_size = 8 * 1024;
    aac_cfg.task_stack = 6 * 1024;
    aac_cfg.stack_in_ext = false;
    s_aac = aac_decoder_init(&aac_cfg);

    s_m2s = mono_to_stereo_init();

    audio_pipeline_register(s_pipeline, s_http, "http");
    audio_pipeline_register(s_pipeline, s_aac, "aac");
    audio_pipeline_register(s_pipeline, s_m2s, "m2s");
    audio_pipeline_register(s_pipeline, s_i2s, "i2s");

    const char *link_tag[4] = {"http", "aac", "m2s", "i2s"};
    audio_pipeline_link(s_pipeline, &link_tag[0], 4);
    audio_element_set_uri(s_http, url);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_pipeline, s_evt);

    audio_pipeline_run(s_pipeline);
    radio_player_set_volume(volume);
    s_got_music_info = false;
    s_running = true;
    return ESP_OK;
}

void radio_player_stop(void)
{
    if (!s_running) {
        return;
    }
    audio_pipeline_stop(s_pipeline);
    audio_pipeline_wait_for_stop(s_pipeline);
    audio_pipeline_terminate(s_pipeline);
    audio_pipeline_unregister(s_pipeline, s_http);
    audio_pipeline_unregister(s_pipeline, s_aac);
    audio_pipeline_unregister(s_pipeline, s_m2s);
    audio_pipeline_unregister(s_pipeline, s_i2s);
    audio_pipeline_remove_listener(s_pipeline);
    audio_event_iface_destroy(s_evt);
    audio_pipeline_deinit(s_pipeline);
    audio_element_deinit(s_http);
    audio_element_deinit(s_aac);
    audio_element_deinit(s_m2s);
    audio_element_deinit(s_i2s);
    s_pipeline = NULL;
    s_http = NULL;
    s_aac = NULL;
    s_m2s = NULL;
    s_i2s = NULL;
    s_evt = NULL;
    s_running = false;
}

static int s_restart_backoff_ms = 500;

void radio_player_loop(void)
{
    if (!s_running || !s_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(s_evt, &msg, 0);
    if (ret != ESP_OK) {
        return;
    }

    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_aac
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t music_info = {0};
        audio_element_getinfo(s_aac, &music_info);
        ESP_LOGI(TAG, "music info rate=%d bits=%d ch=%d -> stereo I2S",
                 music_info.sample_rates, music_info.bits, music_info.channels);
        if (music_info.sample_rates > 0) {
            s_sample_rate = music_info.sample_rates;
        }
        audio_element_setinfo(s_m2s, &music_info);
        i2s_stream_set_clk(s_i2s, music_info.sample_rates, music_info.bits, 2);
        s_got_music_info = true;
        s_restart_backoff_ms = 500;
        return;
    }

    bool bad = msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
        && ((int)msg.data == AEL_STATUS_ERROR_OPEN || (int)msg.data == AEL_STATUS_ERROR_INPUT
            || (int)msg.data == AEL_STATUS_ERROR_PROCESS)
        && (msg.source == (void *)s_http || msg.source == (void *)s_aac);
    if (!bad) {
        return;
    }

    ESP_LOGW(TAG, "stream/decoder error, restarting in %d ms", s_restart_backoff_ms);
    audio_pipeline_stop(s_pipeline);
    audio_pipeline_wait_for_stop(s_pipeline);
    vTaskDelay(pdMS_TO_TICKS(s_restart_backoff_ms));
    if (s_restart_backoff_ms < 8000) {
        s_restart_backoff_ms *= 2;
    }
    audio_element_reset_state(s_aac);
    audio_element_reset_state(s_m2s);
    audio_element_reset_state(s_i2s);
    audio_pipeline_reset_ringbuffer(s_pipeline);
    audio_pipeline_reset_items_state(s_pipeline);
    audio_pipeline_run(s_pipeline);
}
