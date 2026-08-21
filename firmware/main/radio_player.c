#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "radio_player.h"
#include "listen_stats.h"
#include "pcm_upmix.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_mem.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "aac_decoder.h"
#include "downmix.h"
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "ringbuf.h"

static const char *TAG = "radio_player";

#define SB_STREAM_STALL_MS 15000
#define SB_STREAM_STALL_GRACE_MS 35000
#define SB_STREAM_HARD_RESTART_AFTER 3
#define SB_HTTP_RB_SIZE (256 * 1024)
#define SB_WIFI_RSSI_WEAK_DBM (-78)
#define SB_WIFI_HTTP_LOW_BYTES (128 * 1024)
#define SB_WIFI_HTTP_RESUME_BYTES (200 * 1024)
/* Live 64 kbps: 64 KB is ~8 s left. Lower than wifi-weak (128 KB) so a
 * healthy post-prefetch fill (~100 KB) does not loop the prompt. */
#define SB_HTTP_SLOW_LOW_BYTES (64 * 1024)
#define SB_HTTP_SLOW_RESUME_BYTES (128 * 1024)
#define SB_HTTP_SLOW_RECONNECT_MS 25000
#define SB_MIX_SR 44100
#define SB_SLOT_RADIO 0
#define SB_SLOT_CLIP 1
#define SB_PCM_VOICE_ABS 2000
#define SB_PCM_HOLD_ABS 400

#define SB_ALC_MIN_DB (-36)
#define SB_ALC_MAX_DB (2)
/* Downmix reads slots in series. A mute-slot wait of 20 ticks (20 ms at
 * 1 kHz) on an empty rb adds 20 ms to every 256-sample block (~6 ms) and
 * I2S underruns — choppy welcome. Unused slots must be timeout 0: ADF
 * treats TIMEOUT as silence and continues. Mix then blocks on I2S/clip. */
/* 0 busy-spins mix (current + rb-swap races at 240 MHz). 1 tick yields. */
#define SB_MIX_MUTE_TIMEOUT 1
#define SB_MIX_CLIP_TIMEOUT 40
#define SB_MIX_RADIO_TIMEOUT 50

#define SB_BEEP_HZ 2000
#define SB_BEEP_MS 55
#define SB_BEEP_AMP 4200
#define SB_BEEP_FRAMES ((SB_MIX_SR * SB_BEEP_MS) / 1000)
#define SB_LIMIT_HZ 880
#define SB_LIMIT_MS 120
#define SB_LIMIT_AMP 5200
#define SB_LIMIT_FRAMES ((SB_MIX_SR * SB_LIMIT_MS) / 1000)

#define SB_PROBE_WIN 512
#define SB_PROBE_START_LO 5500
#define SB_PROBE_START_HI 10500
#define SB_PROBE_END_LO 2000
#define SB_PROBE_END_HI 4500

/* Brasilstream stats group by exact User-Agent. Product + OS comment +
 * STA MAC (no colons) so they can count SearaBoom and still split boxes. */
static char s_stream_ua[64];

static const char *stream_user_agent(void)
{
    if (s_stream_ua[0]) {
        return s_stream_ua;
    }
    uint8_t mac[6];
    char ver[16] = "0";
    const esp_app_desc_t *app = esp_app_get_description();
    if (app && app->version[0]) {
        strncpy(ver, app->version, sizeof(ver) - 1);
        ver[sizeof(ver) - 1] = '\0';
    }
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_stream_ua, sizeof(s_stream_ua),
             "SearaBoom/%s (SearaBoom; %02x%02x%02x%02x%02x%02x)",
             ver, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s_stream_ua;
}

static audio_pipeline_handle_t s_mix_pipe;
static audio_pipeline_handle_t s_radio_pipe;
static audio_element_handle_t s_downmix;
static audio_element_handle_t s_tap;
static audio_element_handle_t s_i2s;
static audio_element_handle_t s_http;
static audio_element_handle_t s_aac;
static audio_element_handle_t s_radio_m2s;
static audio_element_handle_t s_radio_raw;
static audio_event_iface_handle_t s_mix_evt;
static audio_event_iface_handle_t s_radio_evt;
static ringbuf_handle_t s_mute_radio;
static ringbuf_handle_t s_mute_clip;
static ringbuf_handle_t s_radio_pcm;
static ringbuf_handle_t s_clip_pcm;

static int s_volume = SB_DEFAULT_VOLUME;
static bool s_running;
static bool s_prefetching;
static bool s_got_music_info;
static bool s_have_out;
static bool s_clip_active;

static volatile bool s_pcm_heard;
static volatile bool s_pcm_flowing;
static volatile int s_pcm_peak_max;
static volatile int64_t s_pcm_last_voice_us;
static volatile bool s_amp_gated;
static volatile int64_t s_last_pcm_ms;
static int64_t s_stall_grace_until_ms;
static int s_restart_backoff_ms = 500;
static int s_stall_strikes;
static char s_url[256];
static volatile bool s_want_stop;
static volatile bool s_wifi_weak_latched;
static bool s_hold_radio;
static int64_t s_hold_started_ms;

static int16_t *s_beep_pcm;
static int16_t *s_limit_pcm;
static const int16_t *s_beep_src;
static int s_beep_len;
static volatile int s_beep_pos = -1;
static bool s_beep_ready;

static volatile bool s_probe_on;
static int s_probe_prev;
static int s_probe_zc;
static int s_probe_n;
static int s_probe_win_peak;
static int s_probe_first_hz;
static int s_probe_last_hz;
static int64_t s_probe_first_us;
static int64_t s_probe_last_us;

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
    if (s_i2s && !s_amp_gated) {
        i2s_alc_volume_set(s_i2s, volume_to_alc(volume));
    }
    ESP_LOGI(TAG, "volume=%d alc=%d dB%s", s_volume, volume_to_alc(s_volume),
             s_amp_gated ? " (amp gated)" : "");
    return ESP_OK;
}

int radio_player_get_volume(void)
{
    return s_volume;
}

bool radio_player_has_music_info(void)
{
    return s_got_music_info;
}

static void fill_tone(int16_t *dst, int frames, int hz, int amp)
{
    int edge = frames / 3;
    if (edge < 1) {
        edge = 1;
    }
    const float w = 6.28318530718f * (float)hz / (float)SB_MIX_SR;
    for (int i = 0; i < frames; i++) {
        int a = amp;
        if (i < edge) {
            a = (a * i) / edge;
        } else if (i > frames - 1 - edge) {
            a = (a * (frames - 1 - i)) / edge;
        }
        dst[i] = (int16_t)((float)a * sinf(w * (float)i));
    }
}

static void beep_prepare(void)
{
    if (s_beep_ready) {
        return;
    }
    if (!s_beep_pcm) {
        s_beep_pcm = audio_calloc(SB_BEEP_FRAMES, sizeof(int16_t));
    }
    if (!s_limit_pcm) {
        s_limit_pcm = audio_calloc(SB_LIMIT_FRAMES, sizeof(int16_t));
    }
    if (!s_beep_pcm || !s_limit_pcm) {
        ESP_LOGW(TAG, "beep pcm alloc failed");
        return;
    }
    fill_tone(s_beep_pcm, SB_BEEP_FRAMES, SB_BEEP_HZ, SB_BEEP_AMP);
    fill_tone(s_limit_pcm, SB_LIMIT_FRAMES, SB_LIMIT_HZ, SB_LIMIT_AMP);
    s_beep_ready = true;
}

static void beep_play(const int16_t *src, int frames)
{
    if (!s_have_out || !src || frames <= 0) {
        return;
    }
    beep_prepare();
    if (!s_beep_ready) {
        return;
    }
    s_beep_src = src;
    s_beep_len = frames;
    s_beep_pos = 0;
}

void radio_player_beep(void)
{
    beep_play(s_beep_pcm, SB_BEEP_FRAMES);
}

void radio_player_beep_limit(void)
{
    beep_play(s_limit_pcm, SB_LIMIT_FRAMES);
}

/* Overlay mix output. ADF tone_stream is a flash-MP3 reader and needs a mix
 * slot; radio-only BYPASS ignores extra slots, so a pipeline tone would duck
 * the station or stay silent. */
static void mix_beep_s16le(int16_t *samples, int frames, int channels)
{
    int pos = s_beep_pos;
    const int16_t *src = s_beep_src;
    int len = s_beep_len;
    if (pos < 0 || !src || len <= 0 || pos >= len || channels < 1) {
        return;
    }
    for (int i = 0; i < frames && pos < len; i++, pos++) {
        int16_t tone = src[pos];
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
    }
    s_beep_pos = (pos >= len) ? -1 : pos;
}

static void probe_window_done(int zc, int n, int peak, int64_t now)
{
    if (n < 8 || peak < SB_PCM_VOICE_ABS) {
        s_probe_prev = 0;
        return;
    }
    int hz = (int)((zc * (int64_t)SB_MIX_SR) / (2 * n));
    if (s_probe_first_us == 0) {
        s_probe_first_us = now;
        s_probe_first_hz = hz;
    }
    s_probe_last_us = now;
    s_probe_last_hz = hz;
}

static void pcm_note_s16(const int16_t *s, int n, int channels)
{
    int peak = 0;
    int step = channels > 0 ? channels : 1;
    for (int i = 0; i < n; i++) {
        int a = s[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak) {
            peak = a;
        }
    }
    if (peak > s_pcm_peak_max) {
        s_pcm_peak_max = peak;
    }
    int64_t now = esp_timer_get_time();
    if (peak >= SB_PCM_HOLD_ABS) {
        s_pcm_last_voice_us = now;
    }
    if (s_probe_on) {
        for (int i = 0; i < n; i += step) {
            int v = s[i];
            int a = v < 0 ? -v : v;
            if (a > s_probe_win_peak) {
                s_probe_win_peak = a;
            }
            if (s_probe_n > 0 && ((s_probe_prev < 0) != (v < 0))) {
                s_probe_zc++;
            }
            s_probe_prev = v;
            s_probe_n++;
            if (s_probe_n >= SB_PROBE_WIN) {
                probe_window_done(s_probe_zc, s_probe_n, s_probe_win_peak, now);
                s_probe_zc = 0;
                s_probe_n = 0;
                s_probe_win_peak = 0;
            }
        }
    }
    if (peak < SB_PCM_VOICE_ABS) {
        return;
    }
    if (!s_pcm_heard) {
        s_pcm_heard = true;
        ESP_LOGI(TAG, "PCM reached amp peak=%d", peak);
        if (s_amp_gated && s_i2s) {
            s_amp_gated = false;
            i2s_alc_volume_set(s_i2s, volume_to_alc(s_volume));
            ESP_LOGI(TAG, "amp ungated alc=%d dB", volume_to_alc(s_volume));
        }
    }
}

void radio_player_pcm_arm(void)
{
    s_pcm_heard = false;
    s_pcm_last_voice_us = 0;
    s_pcm_peak_max = 0;
}

bool radio_player_pcm_heard(void)
{
    return s_pcm_heard;
}

bool radio_player_pcm_flowing(void)
{
    if (!s_pcm_flowing) {
        return false;
    }
    /* Mixer tap stamps this; treat as stalled if PCM has not moved recently. */
    return (esp_timer_get_time() / 1000 - s_last_pcm_ms) < 500;
}

int radio_player_pcm_peak(void)
{
    return s_pcm_peak_max;
}

bool radio_player_pcm_finished(int silence_ms)
{
    if (!s_pcm_heard || silence_ms <= 0) {
        return false;
    }
    int64_t last = s_pcm_last_voice_us;
    if (last <= 0) {
        return false;
    }
    return (esp_timer_get_time() - last) >= (int64_t)silence_ms * 1000;
}

void radio_player_probe_arm(void)
{
    s_probe_on = true;
    s_probe_prev = 0;
    s_probe_zc = 0;
    s_probe_n = 0;
    s_probe_win_peak = 0;
    s_probe_first_hz = 0;
    s_probe_last_hz = 0;
    s_probe_first_us = 0;
    s_probe_last_us = 0;
    radio_player_pcm_arm();
}

void radio_player_probe_result(radio_probe_result_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->peak = s_pcm_peak_max;
    out->start_hz = s_probe_first_hz;
    out->end_hz = s_probe_last_hz;
    if (s_probe_first_us > 0 && s_probe_last_us > s_probe_first_us) {
        out->dur_ms = (int)((s_probe_last_us - s_probe_first_us) / 1000);
    }
    out->start_ok = (s_probe_first_hz >= SB_PROBE_START_LO && s_probe_first_hz <= SB_PROBE_START_HI);
    out->end_ok = (s_probe_last_hz >= SB_PROBE_END_LO && s_probe_last_hz <= SB_PROBE_END_HI);
    s_probe_on = false;
}

static esp_err_t tap_open(audio_element_handle_t self)
{
    audio_element_info_t info = {0};
    info.sample_rates = SB_MIX_SR;
    info.bits = 16;
    info.channels = 2;
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int tap_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r = audio_element_input(self, in_buffer, in_len);
    if (r <= 0) {
        return r;
    }
    s_last_pcm_ms = esp_timer_get_time() / 1000;
    s_pcm_flowing = true;
    if ((r & 1) == 0) {
        pcm_note_s16((int16_t *)in_buffer, r / 2, 2);
        if (s_beep_pos >= 0) {
            mix_beep_s16le((int16_t *)in_buffer, r / 4, 2);
        }
    }
    return audio_element_output(self, in_buffer, r);
}

static audio_element_handle_t tap_init(void)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = tap_open;
    cfg.process = tap_process;
    cfg.tag = "tap";
    cfg.out_rb_size = 8 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 5;
    cfg.task_core = 0;
    cfg.stack_in_ext = false; /* mix path: keep next to I2S */
    cfg.buffer_len = 2048;
    return audio_element_init(&cfg);
}

static int http_stream_event(http_stream_event_msg_t *msg)
{
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        esp_http_client_handle_t client = (esp_http_client_handle_t)msg->http_client;
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "User-Agent", stream_user_agent());
    }
    if (msg->event_id == HTTP_STREAM_POST_REQUEST
        || msg->event_id == HTTP_STREAM_ON_RESPONSE
        || msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        audio_element_set_codec_fmt(msg->el, ESP_CODEC_TYPE_UNKNOW);
    }
    return ESP_OK;
}

static void board_codec_start(void)
{
    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle && board_handle->audio_hal) {
        audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    }
}

static void store_url_volume(const char *url, int volume)
{
    s_volume = volume;
    strncpy(s_url, url ? url : "", sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = 0;
}

static void radio_flush_pcm(void)
{
    if (s_radio_pcm) {
        rb_reset(s_radio_pcm);
    }
}

static void radio_mark_started(void)
{
    s_running = true;
    s_prefetching = false;
    int64_t now = esp_timer_get_time() / 1000;
    s_last_pcm_ms = now;
    s_stall_grace_until_ms = now + SB_STREAM_STALL_GRACE_MS;
    s_stall_strikes = 0;
    s_restart_backoff_ms = 500;
}

static audio_event_iface_handle_t make_evt(void)
{
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    return audio_event_iface_init(&evt_cfg);
}

static void mix_use_rb(int slot, ringbuf_handle_t rb, int timeout)
{
    if (!s_downmix || !rb) {
        return;
    }
    downmix_set_input_rb_timeout(s_downmix, 0, slot);
    downmix_set_input_rb(s_downmix, rb, slot);
    downmix_set_input_rb_timeout(s_downmix, timeout, slot);
}

static void mix_mute_slot(int slot)
{
    mix_use_rb(slot, slot == SB_SLOT_RADIO ? s_mute_radio : s_mute_clip,
               SB_MIX_MUTE_TIMEOUT);
}

static void mix_route_clip_and_radio(void);

static void mix_rewind(void)
{
    if (!s_mix_pipe) {
        return;
    }
    audio_pipeline_stop(s_mix_pipe);
    audio_pipeline_wait_for_stop(s_mix_pipe);
    audio_pipeline_reset_ringbuffer(s_mix_pipe);
    audio_pipeline_reset_elements(s_mix_pipe);
    audio_pipeline_reset_items_state(s_mix_pipe);
    audio_pipeline_change_state(s_mix_pipe, AEL_STATE_INIT);
}

static void mix_restart_if_needed(void)
{
    if (!s_mix_pipe || !s_downmix) {
        return;
    }
    audio_element_state_t st = audio_element_get_state(s_downmix);
    audio_element_state_t i2s_st = s_i2s ? audio_element_get_state(s_i2s) : AEL_STATE_RUNNING;
    if (st != AEL_STATE_FINISHED && st != AEL_STATE_STOPPED && st != AEL_STATE_ERROR
        && i2s_st != AEL_STATE_FINISHED && i2s_st != AEL_STATE_STOPPED && i2s_st != AEL_STATE_ERROR) {
        return;
    }
    ESP_LOGW(TAG, "mix dead mix=%d i2s=%d, restarting", (int)st, (int)i2s_st);
    mix_rewind();
    mix_mute_slot(SB_SLOT_RADIO);
    mix_mute_slot(SB_SLOT_CLIP);
    if (audio_pipeline_run(s_mix_pipe) != ESP_OK) {
        ESP_LOGE(TAG, "mix restart run failed");
        return;
    }
    if (s_i2s) {
        i2s_stream_set_clk(s_i2s, SB_MIX_SR, 16, 2);
        if (!s_amp_gated) {
            i2s_alc_volume_set(s_i2s, volume_to_alc(s_volume));
        }
    }
    mix_route_clip_and_radio();
}

static void teardown_radio(void)
{
    s_radio_pcm = NULL;
    s_running = false;
    s_prefetching = false;
    s_hold_radio = false;
    s_hold_started_ms = 0;
    mix_route_clip_and_radio();
    vTaskDelay(pdMS_TO_TICKS(30));
    if (s_radio_raw) {
        ringbuf_handle_t old = audio_element_get_input_ringbuf(s_radio_raw);
        if (old) {
            rb_abort(old);
        }
    }
    if (s_radio_pipe) {
        audio_pipeline_stop(s_radio_pipe);
        audio_pipeline_wait_for_stop(s_radio_pipe);
        audio_pipeline_terminate(s_radio_pipe);
        if (s_http) {
            audio_pipeline_unregister(s_radio_pipe, s_http);
        }
        if (s_aac) {
            audio_pipeline_unregister(s_radio_pipe, s_aac);
        }
        if (s_radio_m2s) {
            audio_pipeline_unregister(s_radio_pipe, s_radio_m2s);
        }
        if (s_radio_raw) {
            audio_pipeline_unregister(s_radio_pipe, s_radio_raw);
        }
        audio_pipeline_remove_listener(s_radio_pipe);
        audio_pipeline_deinit(s_radio_pipe);
        s_radio_pipe = NULL;
    }
    if (s_http) {
        audio_element_deinit(s_http);
        s_http = NULL;
    }
    if (s_aac) {
        audio_element_deinit(s_aac);
        s_aac = NULL;
    }
    if (s_radio_m2s) {
        audio_element_deinit(s_radio_m2s);
        s_radio_m2s = NULL;
    }
    if (s_radio_raw) {
        audio_element_deinit(s_radio_raw);
        s_radio_raw = NULL;
    }
    if (s_radio_evt) {
        audio_event_iface_destroy(s_radio_evt);
        s_radio_evt = NULL;
    }
    s_running = false;
    s_prefetching = false;
    s_got_music_info = false;
}

static void teardown_mix(void)
{
    teardown_radio();
    if (s_mix_pipe) {
        audio_pipeline_stop(s_mix_pipe);
        audio_pipeline_wait_for_stop(s_mix_pipe);
        audio_pipeline_terminate(s_mix_pipe);
        if (s_downmix) {
            audio_pipeline_unregister(s_mix_pipe, s_downmix);
        }
        if (s_tap) {
            audio_pipeline_unregister(s_mix_pipe, s_tap);
        }
        if (s_i2s) {
            audio_pipeline_unregister(s_mix_pipe, s_i2s);
        }
        audio_pipeline_remove_listener(s_mix_pipe);
        audio_pipeline_deinit(s_mix_pipe);
        s_mix_pipe = NULL;
    }
    if (s_downmix) {
        audio_element_deinit(s_downmix);
        s_downmix = NULL;
    }
    if (s_tap) {
        audio_element_deinit(s_tap);
        s_tap = NULL;
    }
    if (s_i2s) {
        audio_element_deinit(s_i2s);
        s_i2s = NULL;
    }
    if (s_mix_evt) {
        audio_event_iface_destroy(s_mix_evt);
        s_mix_evt = NULL;
    }
    if (s_mute_radio) {
        rb_destroy(s_mute_radio);
        s_mute_radio = NULL;
    }
    if (s_mute_clip) {
        rb_destroy(s_mute_clip);
        s_mute_clip = NULL;
    }
    s_have_out = false;
    s_clip_active = false;
}

static esp_err_t ensure_mix(int volume)
{
    if (s_have_out) {
        radio_player_set_volume(volume);
        return ESP_OK;
    }
    board_codec_start();
    if (!s_mix_evt) {
        s_mix_evt = make_evt();
    }

    downmix_cfg_t mix_cfg = DEFAULT_DOWNMIX_CONFIG();
    mix_cfg.downmix_info.source_num = 2;
    mix_cfg.downmix_info.mode = ESP_DOWNMIX_WORK_MODE_BYPASS;
    mix_cfg.downmix_info.output_type = ESP_DOWNMIX_OUTPUT_TYPE_TWO_CHANNEL;
    mix_cfg.downmix_info.out_ctx = ESP_DOWNMIX_OUT_CTX_NORMAL;
    mix_cfg.task_stack = 4 * 1024;
    mix_cfg.task_prio = 6;
    mix_cfg.stack_in_ext = false; /* I2S feeder: internal stack */
    mix_cfg.out_rb_size = 8 * 1024;
    s_downmix = downmix_init(&mix_cfg);
    esp_downmix_input_info_t src[2] = {
        {.samplerate = SB_MIX_SR, .channel = 2, .bits_num = 16, .gain = {0, -12}, .transit_time = 150},
        {.samplerate = SB_MIX_SR, .channel = 2, .bits_num = 16, .gain = {-60, 0}, .transit_time = 150},
    };
    source_info_init(s_downmix, src);
    s_mute_radio = rb_create(256, 4);
    s_mute_clip = rb_create(256, 4);
    mix_mute_slot(SB_SLOT_RADIO);
    mix_mute_slot(SB_SLOT_CLIP);

    s_tap = tap_init();

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = true;
    i2s_cfg.volume = volume_to_alc(volume);
    i2s_cfg.uninstall_drv = true;
    i2s_cfg.stack_in_ext = false;
    i2s_cfg.std_cfg.clk_cfg.sample_rate_hz = SB_MIX_SR;
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_RIGHT_LEFT);
    s_i2s = i2s_stream_init(&i2s_cfg);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_mix_pipe = audio_pipeline_init(&pipeline_cfg);
    if (!s_mix_pipe || !s_downmix || !s_tap || !s_i2s) {
        ESP_LOGE(TAG, "mix init failed");
        teardown_mix();
        return ESP_FAIL;
    }
    audio_pipeline_register(s_mix_pipe, s_downmix, "mix");
    audio_pipeline_register(s_mix_pipe, s_tap, "tap");
    audio_pipeline_register(s_mix_pipe, s_i2s, "i2s");
    const char *link[3] = {"mix", "tap", "i2s"};
    audio_pipeline_link(s_mix_pipe, &link[0], 3);
    audio_pipeline_set_listener(s_mix_pipe, s_mix_evt);
    if (audio_pipeline_run(s_mix_pipe) != ESP_OK) {
        ESP_LOGE(TAG, "mix pipeline_run failed");
        teardown_mix();
        return ESP_FAIL;
    }
    i2s_stream_set_clk(s_i2s, SB_MIX_SR, 16, 2);
    beep_prepare();
    s_amp_gated = true;
    s_have_out = true;
    s_volume = volume;
    if (s_i2s) {
        i2s_alc_volume_set(s_i2s, SB_ALC_MIN_DB);
    }
    radio_player_pcm_arm();
    ESP_LOGI(TAG, "mix+I2S up (44100 stereo)");
    return ESP_OK;
}

static esp_err_t ensure_radio(const char *url, int volume, bool hold)
{
    if (s_radio_pipe) {
        return ESP_OK;
    }
    if (ensure_mix(volume) != ESP_OK) {
        return ESP_FAIL;
    }
    mix_restart_if_needed();
    store_url_volume(url, volume);

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = false;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.event_handle = http_stream_event;
    http_cfg.user_agent = stream_user_agent();
    ESP_LOGI(TAG, "stream UA %s", http_cfg.user_agent);
    http_cfg.out_rb_size = SB_HTTP_RB_SIZE;
    http_cfg.task_stack = 5 * 1024;
    http_cfg.task_core = 1;
    /* Decode path: PSRAM stack. Mix/I2S/tap stay internal (cache-off DMA). */
    http_cfg.stack_in_ext = true;
    s_http = http_stream_init(&http_cfg);

    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    aac_cfg.plus_enable = true;
    aac_cfg.out_rb_size = 16 * 1024;
    aac_cfg.task_stack = 8 * 1024;
    aac_cfg.task_core = 1;
    aac_cfg.task_prio = 6;
    aac_cfg.stack_in_ext = true;
    s_aac = aac_decoder_init(&aac_cfg);

    s_radio_m2s = pcm_upmix_init_core("rm2s", 1);
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 32 * 1024;
    s_radio_raw = raw_stream_init(&raw_cfg);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_radio_pipe = audio_pipeline_init(&pipeline_cfg);
    if (!s_radio_pipe || !s_http || !s_aac || !s_radio_m2s || !s_radio_raw) {
        ESP_LOGE(TAG, "radio init failed");
        teardown_radio();
        return ESP_FAIL;
    }
    audio_pipeline_register(s_radio_pipe, s_http, "http");
    audio_pipeline_register(s_radio_pipe, s_aac, "aac");
    audio_pipeline_register(s_radio_pipe, s_radio_m2s, "rm2s");
    audio_pipeline_register(s_radio_pipe, s_radio_raw, "rraw");
    const char *link[4] = {"http", "aac", "rm2s", "rraw"};
    audio_pipeline_link(s_radio_pipe, &link[0], 4);
    audio_element_set_uri(s_http, url);
    /* channels=0: upmix holds until decoder music_info (do not assume mono). */
    audio_element_set_music_info(s_radio_m2s, SB_MIX_SR, 0, 16);
    s_got_music_info = false;
    s_radio_pcm = audio_element_get_input_ringbuf(s_radio_raw);
    if (!s_radio_evt) {
        s_radio_evt = make_evt();
    }
    audio_pipeline_set_listener(s_radio_pipe, s_radio_evt);
    if (audio_pipeline_run(s_radio_pipe) != ESP_OK) {
        ESP_LOGE(TAG, "radio pipeline_run failed");
        teardown_radio();
        return ESP_FAIL;
    }
    if (hold) {
        s_prefetching = true;
        s_running = false;
        ESP_LOGI(TAG, "radio prefetch (mixer not reading yet)");
    } else {
        radio_mark_started();
        ESP_LOGI(TAG, "radio live");
    }
    mix_route_clip_and_radio();
    return ESP_OK;
}

static void mix_route_clip_and_radio(void)
{
    if (!s_downmix) {
        return;
    }
    /* Prefetch fills the radio pipe but must not feed the mixer — otherwise
     * go_live finds an empty PCM rb and the station underruns. */
    /* Hold keeps HTTP/AAC running but mix must not eat radio PCM, so the
     * 256 KB HTTP rb can refill while the weak-Wi-Fi clip speaks. */
    bool radio = s_running && s_radio_pcm && s_got_music_info && !s_hold_radio;
    bool clip = s_clip_active && s_clip_pcm;
    if (clip && radio) {
        mix_use_rb(SB_SLOT_RADIO, s_radio_pcm, SB_MIX_MUTE_TIMEOUT);
        mix_use_rb(SB_SLOT_CLIP, s_clip_pcm, SB_MIX_CLIP_TIMEOUT);
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_SWITCH_ON);
        return;
    }
    if (clip) {
        /* BYPASS dies on any non-TIMEOUT on slot 0. Clip EOS/ABORT would
         * finish mix+I2S, so clip-only always uses SWITCH_ON: mute slot 0
         * (timeout 0 → silence) plus clip on slot 1. */
        mix_mute_slot(SB_SLOT_RADIO);
        mix_use_rb(SB_SLOT_CLIP, s_clip_pcm, SB_MIX_CLIP_TIMEOUT);
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_SWITCH_ON);
        return;
    }
    mix_mute_slot(SB_SLOT_CLIP);
    if (radio) {
        mix_use_rb(SB_SLOT_RADIO, s_radio_pcm, SB_MIX_RADIO_TIMEOUT);
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_BYPASS);
    } else {
        mix_mute_slot(SB_SLOT_RADIO);
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_BYPASS);
    }
}

esp_err_t radio_player_attach_clip_pcm(ringbuf_handle_t rb)
{
    if (ensure_mix(s_volume) != ESP_OK) {
        return ESP_FAIL;
    }
    mix_restart_if_needed();
    s_clip_pcm = rb;
    mix_route_clip_and_radio();
    return ESP_OK;
}

void radio_player_set_clip_active(bool on)
{
    s_clip_active = on;
    mix_route_clip_and_radio();
}

esp_err_t radio_player_start_idle(int volume)
{
    ESP_LOGI(TAG, "Start idle audio out (clips)");
    if (ensure_mix(volume) != ESP_OK) {
        return ESP_FAIL;
    }
    mix_restart_if_needed();
    return ESP_OK;
}

esp_err_t radio_player_prefetch(const char *url, int volume)
{
    if (s_running || s_prefetching) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Prefetch stream %s", url);
    return ensure_radio(url, volume, true);
}

bool radio_player_is_prefetching(void)
{
    return s_prefetching;
}

esp_err_t radio_player_go_live(void)
{
    if (!s_have_out) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_radio_pipe) {
        if (!s_url[0]) {
            return ESP_ERR_INVALID_STATE;
        }
        return ensure_radio(s_url, s_volume, false);
    }
    ESP_LOGI(TAG, "Go live — mixer reads radio");
    if (s_amp_gated && s_i2s) {
        s_amp_gated = false;
        i2s_alc_volume_set(s_i2s, volume_to_alc(s_volume));
    }
    if (!s_running) {
        radio_mark_started();
    }
    s_prefetching = false;
    mix_route_clip_and_radio();
    return ESP_OK;
}

esp_err_t radio_player_start(const char *url, int volume)
{
    if (s_prefetching && s_radio_pipe) {
        store_url_volume(url, volume);
        return radio_player_go_live();
    }
    if (s_running) {
        teardown_radio();
    }
    ESP_LOGI(TAG, "Start stream %s", url);
    return ensure_radio(url, volume, false);
}

bool radio_player_has_output(void)
{
    return s_have_out;
}

bool radio_player_is_running(void)
{
    return s_running;
}

void radio_player_request_stop(void)
{
    if (!s_running) {
        return;
    }
    s_want_stop = true;
}

void radio_player_resume(void)
{
    if (s_running || !s_url[0]) {
        return;
    }
    ESP_LOGI(TAG, "Resuming stream after pause");
    radio_player_start(s_url, s_volume);
}

void radio_player_stop(void)
{
    s_want_stop = false;
    teardown_radio();
}

static void radio_soft_restart(const char *reason)
{
    if (!s_radio_pipe) {
        return;
    }
    ESP_LOGW(TAG, "%s — soft restart in %d ms (strike %d)", reason, s_restart_backoff_ms,
             s_stall_strikes + 1);
    audio_pipeline_stop(s_radio_pipe);
    audio_pipeline_wait_for_stop(s_radio_pipe);
    vTaskDelay(pdMS_TO_TICKS(s_restart_backoff_ms));
    if (s_restart_backoff_ms < 8000) {
        s_restart_backoff_ms *= 2;
    }
    audio_pipeline_reset_ringbuffer(s_radio_pipe);
    audio_pipeline_reset_elements(s_radio_pipe);
    audio_pipeline_reset_items_state(s_radio_pipe);
    if (s_radio_m2s) {
        audio_element_set_music_info(s_radio_m2s, SB_MIX_SR, 0, 16);
    }
    audio_pipeline_run(s_radio_pipe);
    int64_t now = esp_timer_get_time() / 1000;
    s_last_pcm_ms = now;
    s_stall_grace_until_ms = now + SB_STREAM_STALL_GRACE_MS;
    s_got_music_info = false;
    mix_route_clip_and_radio();
}

static void check_stream_stall(void)
{
    if (!s_running || !s_got_music_info || s_clip_active) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_stall_grace_until_ms) {
        return;
    }
    int64_t idle = now - s_last_pcm_ms;
    if (idle < SB_STREAM_STALL_MS) {
        return;
    }
    s_stall_strikes++;
    ESP_LOGW(TAG, "stream stall: no PCM for %lld ms", (long long)idle);
    if (s_stall_strikes >= SB_STREAM_HARD_RESTART_AFTER && s_url[0]) {
        ESP_LOGW(TAG, "stream stall: hard restart of radio");
        int vol = s_volume;
        char url[sizeof(s_url)];
        memcpy(url, s_url, sizeof(url));
        teardown_radio();
        vTaskDelay(pdMS_TO_TICKS(500));
        radio_player_start(url, vol);
        return;
    }
    radio_soft_restart("stream stall");
}

static void drain_mix_evt(void)
{
    if (!s_mix_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    while (audio_event_iface_listen(s_mix_evt, &msg, 0) == ESP_OK) {
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && msg.source == (void *)s_downmix) {
            int st = (int)msg.data;
            if (st == AEL_STATUS_STATE_FINISHED || st == AEL_STATUS_STATE_STOPPED
                || st == AEL_STATUS_ERROR_PROCESS || st == AEL_STATUS_ERROR_INPUT) {
                ESP_LOGW(TAG, "mix status=%d", st);
            }
        }
    }
}

void radio_player_arm_rssi_threshold(void)
{
    esp_err_t err = esp_wifi_set_rssi_threshold(SB_WIFI_RSSI_WEAK_DBM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rssi threshold %d dBm: %s", SB_WIFI_RSSI_WEAK_DBM,
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "rssi threshold %d dBm", SB_WIFI_RSSI_WEAK_DBM);
}

static int http_rb_filled(void)
{
    if (!s_http) {
        return -1;
    }
    ringbuf_handle_t rb = audio_element_get_output_ringbuf(s_http);
    return rb ? rb_bytes_filled(rb) : -1;
}

int radio_player_http_buffered(void)
{
    if (!s_running) {
        return -1;
    }
    return http_rb_filled();
}

static bool sta_rssi_is_weak(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    return ap.rssi <= SB_WIFI_RSSI_WEAK_DBM;
}

static bool http_buf_hungry(void)
{
    int filled = http_rb_filled();
    return filled < SB_WIFI_HTTP_LOW_BYTES;
}

static bool http_buf_critical(void)
{
    int filled = http_rb_filled();
    return filled >= 0 && filled < SB_HTTP_SLOW_LOW_BYTES;
}

void radio_player_on_rssi_low(int rssi_dbm)
{
    s_wifi_weak_latched = true;
    ESP_LOGW(TAG, "RSSI low (%d dBm) HTTP rb=%d/%d", rssi_dbm, http_rb_filled(),
             SB_HTTP_RB_SIZE);
}

void radio_player_on_sta_lost(void)
{
    s_wifi_weak_latched = false;
    if (s_hold_radio) {
        s_hold_radio = false;
        mix_route_clip_and_radio();
    }
}

void radio_player_hold_stream(bool on)
{
    if (s_hold_radio == on) {
        return;
    }
    s_hold_radio = on;
    if (on) {
        s_hold_started_ms = esp_timer_get_time() / 1000;
        ESP_LOGW(TAG, "hold radio (HTTP refill) rb=%d", http_rb_filled());
    } else {
        radio_flush_pcm();
        s_last_pcm_ms = esp_timer_get_time() / 1000;
        s_hold_started_ms = 0;
        s_wifi_weak_latched = false;
        radio_player_arm_rssi_threshold();
        ESP_LOGI(TAG, "resume radio HTTP rb=%d", http_rb_filled());
    }
    mix_route_clip_and_radio();
}

bool radio_player_wifi_weak_holding(void)
{
    return s_hold_radio;
}

bool radio_player_wifi_weak_resume_ready(void)
{
    if (!s_hold_radio) {
        return false;
    }
    if (!s_got_music_info) {
        return false;
    }
    int filled = http_rb_filled();
    int need = s_wifi_weak_latched ? SB_WIFI_HTTP_RESUME_BYTES
                                   : SB_HTTP_SLOW_RESUME_BYTES;
    if (filled >= need) {
        ESP_LOGI(TAG, "HTTP refill ready rb=%d/%d need=%d", filled, SB_HTTP_RB_SIZE,
                 need);
        return true;
    }
    return false;
}

bool radio_player_wifi_weak_should_speak(void)
{
    if (s_hold_radio || s_clip_active || s_prefetching) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_stall_grace_until_ms) {
        return false;
    }
    if (!http_buf_hungry()) {
        return false;
    }
    if (!s_wifi_weak_latched) {
        /* Event can miss if RSSI was already below the threshold. Confirm
         * only when the HTTP rb is actually hurting. */
        if (!sta_rssi_is_weak()) {
            return false;
        }
        s_wifi_weak_latched = true;
    }
    ESP_LOGW(TAG, "wifi weak and HTTP rb=%d/%d — prompt then hold",
             http_rb_filled(), SB_HTTP_RB_SIZE);
    return true;
}

bool radio_player_http_slow_should_speak(void)
{
    if (s_hold_radio || s_clip_active || s_prefetching) {
        return false;
    }
    if (!s_running || !s_got_music_info) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_stall_grace_until_ms) {
        return false;
    }
    if (s_wifi_weak_latched || sta_rssi_is_weak()) {
        return false;
    }
    int filled = http_rb_filled();
    if (filled < 0 || filled >= SB_HTTP_SLOW_LOW_BYTES) {
        return false;
    }
    ESP_LOGW(TAG, "HTTP slow rb=%d/%d — hold then prompt", filled, SB_HTTP_RB_SIZE);
    return true;
}

void radio_player_loop(void)
{
    listen_stats_poll();
    if (s_want_stop) {
        radio_player_stop();
        return;
    }
    if (!s_have_out) {
        return;
    }
    drain_mix_evt();
    mix_restart_if_needed();
    if (s_hold_radio && !s_wifi_weak_latched && s_running && s_http
        && s_hold_started_ms > 0) {
        int64_t now = esp_timer_get_time() / 1000;
        int filled = http_rb_filled();
        if (now - s_hold_started_ms >= SB_HTTP_SLOW_RECONNECT_MS
            && filled >= 0 && filled < SB_HTTP_SLOW_RESUME_BYTES) {
            radio_soft_restart("HTTP slow hold — reconnect");
            s_hold_started_ms = now;
        }
    }
    if (s_running && !s_clip_active && !s_hold_radio && !s_wifi_weak_latched
        && !http_buf_critical()) {
        check_stream_stall();
    }
    if (!s_radio_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(s_radio_evt, &msg, 0);
    if (ret != ESP_OK) {
        return;
    }
    /* Fall through to handle this event (loop may be called often). */
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_aac
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t music_info = {0};
        audio_element_getinfo(s_aac, &music_info);
        ESP_LOGI(TAG, "stream music info rate=%d bits=%d ch=%d (mix locked 44100 stereo)",
                 music_info.sample_rates, music_info.bits, music_info.channels);
        int ch = music_info.channels > 0 ? music_info.channels : 2;
        int rate = music_info.sample_rates > 0 ? music_info.sample_rates : SB_MIX_SR;
        if (s_radio_m2s) {
            audio_element_set_music_info(s_radio_m2s, rate, ch, 16);
        }
        /* Flush only while prefetching. Doing it after go-live dumps the
         * PCM that was already playing (clean first slice, then underrun). */
        if (!s_running) {
            radio_flush_pcm();
        }
        if (rate >= 16000) {
            s_got_music_info = true;
        }
        s_restart_backoff_ms = 500;
        s_stall_strikes = 0;
        s_last_pcm_ms = esp_timer_get_time() / 1000;
        mix_route_clip_and_radio();
        ESP_LOGI(TAG, "heap after music: free=%u spiram=%u internal=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    bool bad = msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
        && ((int)msg.data == AEL_STATUS_ERROR_OPEN || (int)msg.data == AEL_STATUS_ERROR_INPUT
            || (int)msg.data == AEL_STATUS_ERROR_PROCESS)
        && (msg.source == (void *)s_http || msg.source == (void *)s_aac);
    if (!bad || s_clip_active || s_hold_radio || s_wifi_weak_latched
        || http_buf_critical()) {
        return;
    }
    s_stall_strikes++;
    if (s_stall_strikes >= SB_STREAM_HARD_RESTART_AFTER && s_url[0]) {
        ESP_LOGW(TAG, "stream/decoder error — hard restart");
        int vol = s_volume;
        char url[sizeof(s_url)];
        memcpy(url, s_url, sizeof(url));
        teardown_radio();
        vTaskDelay(pdMS_TO_TICKS(s_restart_backoff_ms));
        if (s_restart_backoff_ms < 8000) {
            s_restart_backoff_ms *= 2;
        }
        radio_player_start(url, vol);
        return;
    }
    radio_soft_restart("stream/decoder error");
}
