#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "clip_player.h"
#include "radio_player.h"
#include "adts_util.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "i2s_stream.h"
#include "aac_decoder.h"
#include "audio_common.h"
#include "board.h"

static const char *TAG = "clip_player";

extern const uint8_t ota_updating_aac_start[] asm("_binary_ota_updating_aac_start");
extern const uint8_t ota_updating_aac_end[] asm("_binary_ota_updating_aac_end");
extern const uint8_t ota_done_aac_start[] asm("_binary_ota_done_aac_start");
extern const uint8_t ota_done_aac_end[] asm("_binary_ota_done_aac_end");
extern const uint8_t ap_welcome_aac_start[] asm("_binary_ap_welcome_aac_start");
extern const uint8_t ap_welcome_aac_end[] asm("_binary_ap_welcome_aac_end");
extern const uint8_t ap_connected_aac_start[] asm("_binary_ap_connected_aac_start");
extern const uint8_t ap_connected_aac_end[] asm("_binary_ap_connected_aac_end");
extern const uint8_t ap_saved_aac_start[] asm("_binary_ap_saved_aac_start");
extern const uint8_t ap_saved_aac_end[] asm("_binary_ap_saved_aac_end");

#define SB_ALC_MAX_DB (2)
/* Decoder + I2S still hold PCM after the last ADTS byte is queued. */
#define SB_CLIP_TAIL_MS (800)

static const char *clip_name[SB_CLIP_COUNT] = {
    [SB_CLIP_OTA_UPDATING] = "ota_updating",
    [SB_CLIP_OTA_DONE] = "ota_done",
    [SB_CLIP_AP_WELCOME] = "ap_welcome",
    [SB_CLIP_AP_CONNECTED] = "ap_connected",
    [SB_CLIP_AP_SAVED] = "ap_saved",
};

static const uint8_t *clip_start[SB_CLIP_COUNT] = {
    [SB_CLIP_OTA_UPDATING] = ota_updating_aac_start,
    [SB_CLIP_OTA_DONE] = ota_done_aac_start,
    [SB_CLIP_AP_WELCOME] = ap_welcome_aac_start,
    [SB_CLIP_AP_CONNECTED] = ap_connected_aac_start,
    [SB_CLIP_AP_SAVED] = ap_saved_aac_start,
};
static const uint8_t *clip_end[SB_CLIP_COUNT] = {
    [SB_CLIP_OTA_UPDATING] = ota_updating_aac_end,
    [SB_CLIP_OTA_DONE] = ota_done_aac_end,
    [SB_CLIP_AP_WELCOME] = ap_welcome_aac_end,
    [SB_CLIP_AP_CONNECTED] = ap_connected_aac_end,
    [SB_CLIP_AP_SAVED] = ap_saved_aac_end,
};

static audio_pipeline_handle_t s_pipe;
static audio_element_handle_t s_src;
static audio_element_handle_t s_aac;
static audio_element_handle_t s_m2s;
static audio_element_handle_t s_i2s;
static audio_event_iface_handle_t s_evt;
static bool s_own_pipe;

static const uint8_t *s_src_data;
static size_t s_src_len;
static size_t s_src_pos;
static bool s_src_loop;
static bool s_src_playing;
static bool s_src_started;
static int s_src_gap_ms;
static int64_t s_src_gap_until;

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
    /* UI clips are AAC-LC 22050 mono. Always duplicate — if the decoder reports
     * 2ch (HE-AAC PS), treating mono as stereo plays at 2× (Mickey Mouse). */
    int bps = 2;
    int samples = r_size / bps;
    int out_bytes = samples * bps * 2;
    static char out[8192];
    if (out_bytes > (int)sizeof(out)) {
        out_bytes = (int)sizeof(out);
        samples = out_bytes / (bps * 2);
    }
    for (int i = 0; i < samples; i++) {
        memcpy(out + (i * 2) * bps, in_buffer + i * bps, bps);
        memcpy(out + (i * 2 + 1) * bps, in_buffer + i * bps, bps);
    }
    return audio_element_output(self, out, out_bytes);
}

static esp_err_t clip_src_open(audio_element_handle_t self)
{
    audio_element_info_t info = {0};
    info.sample_rates = 22050;
    info.bits = 16;
    info.channels = 1;
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int clip_src_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    if (!s_src_playing || !s_src_data) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return AEL_IO_TIMEOUT;
    }
    if (s_src_gap_until > 0) {
        if (esp_timer_get_time() < s_src_gap_until) {
            vTaskDelay(pdMS_TO_TICKS(20));
            return AEL_IO_TIMEOUT;
        }
        s_src_gap_until = 0;
        s_src_pos = 0;
    }
    if (s_src_pos >= s_src_len) {
        if (s_src_loop) {
            if (s_src_gap_ms > 0) {
                s_src_gap_until = esp_timer_get_time() + (int64_t)s_src_gap_ms * 1000;
                vTaskDelay(pdMS_TO_TICKS(20));
                return AEL_IO_TIMEOUT;
            }
            s_src_pos = 0;
        } else {
            s_src_playing = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            return AEL_IO_TIMEOUT;
        }
    }
    int n = in_len;
    if ((size_t)n > s_src_len - s_src_pos) {
        n = (int)(s_src_len - s_src_pos);
    }
    memcpy(in_buffer, s_src_data + s_src_pos, (size_t)n);
    s_src_pos += (size_t)n;
    s_src_started = true;
    return audio_element_output(self, in_buffer, n);
}

static audio_element_handle_t clip_src_init(void)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = clip_src_open;
    cfg.process = clip_src_process;
    cfg.tag = "clip";
    cfg.out_rb_size = 16 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 6;
    cfg.task_core = 0;
    cfg.stack_in_ext = false;
    cfg.buffer_len = 2048;
    return audio_element_init(&cfg);
}

static void clip_src_play(const uint8_t *data, size_t len, bool loop, int gap_ms)
{
    s_src_data = data;
    s_src_len = len;
    s_src_pos = 0;
    s_src_loop = loop;
    s_src_gap_ms = gap_ms > 0 ? gap_ms : 0;
    s_src_gap_until = 0;
    s_src_started = false;
    s_src_playing = true;
}

static void clip_src_stop(void)
{
    s_src_playing = false;
    s_src_data = NULL;
    s_src_len = 0;
    s_src_pos = 0;
    s_src_gap_until = 0;
}

static void pump_music_info(void)
{
    if (!s_evt || !s_aac || !s_m2s) {
        return;
    }
    audio_event_iface_msg_t msg;
    if (audio_event_iface_listen(s_evt, &msg, 0) != ESP_OK) {
        return;
    }
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_aac
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t music_info = {0};
        audio_element_getinfo(s_aac, &music_info);
        ESP_LOGI(TAG, "clip music info rate=%d bits=%d ch=%d (I2S locked 22050 stereo)",
                 music_info.sample_rates, music_info.bits, music_info.channels);
        music_info.sample_rates = 22050;
        music_info.bits = 16;
        music_info.channels = 1;
        audio_element_setinfo(s_m2s, &music_info);
    }
}

static esp_err_t ensure_pipe(void)
{
    if (s_own_pipe) {
        return ESP_OK;
    }
    if (radio_player_is_running()) {
        ESP_LOGW(TAG, "clip-only pipe requested while radio owns I2S");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "starting clip-only pipe (clip -> aac -> m2s -> i2s)");
    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle && board_handle->audio_hal) {
        audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    }

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipe = audio_pipeline_init(&pipeline_cfg);

    s_src = clip_src_init();

    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    /* Clips are AAC-LC @ 22050. plus_enable reports 44100 and Mickey-Mouses them. */
    aac_cfg.plus_enable = false;
    aac_cfg.out_rb_size = 16 * 1024;
    aac_cfg.task_stack = 8 * 1024;
    aac_cfg.stack_in_ext = false;
    s_aac = aac_decoder_init(&aac_cfg);

    audio_element_cfg_t m2s_cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    m2s_cfg.open = m2s_open;
    m2s_cfg.process = m2s_process;
    m2s_cfg.tag = "m2s";
    m2s_cfg.out_rb_size = 16 * 1024;
    m2s_cfg.task_stack = 3 * 1024;
    m2s_cfg.task_prio = 5;
    m2s_cfg.task_core = 0;
    m2s_cfg.stack_in_ext = false;
    m2s_cfg.buffer_len = 2048;
    s_m2s = audio_element_init(&m2s_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = true;
    i2s_cfg.volume = SB_ALC_MAX_DB;
    i2s_cfg.task_core = 1;
    i2s_cfg.task_prio = 5;
    i2s_cfg.stack_in_ext = false;
    i2s_cfg.uninstall_drv = false;
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_RIGHT_LEFT);
    s_i2s = i2s_stream_init(&i2s_cfg);
    /* Default I2S is 44100. Set once before run — never again while AP is up. */
    i2s_stream_set_clk(s_i2s, 22050, 16, 2);

    audio_pipeline_register(s_pipe, s_src, "clip");
    audio_pipeline_register(s_pipe, s_aac, "aac");
    audio_pipeline_register(s_pipe, s_m2s, "m2s");
    audio_pipeline_register(s_pipe, s_i2s, "i2s");
    const char *link[] = {"clip", "aac", "m2s", "i2s"};
    audio_pipeline_link(s_pipe, &link[0], 4);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_pipe, s_evt);
    audio_pipeline_run(s_pipe);
    s_own_pipe = true;
    return ESP_OK;
}

void clip_player_release_pipe(void)
{
    clip_src_stop();
    if (!s_own_pipe) {
        return;
    }
    audio_pipeline_stop(s_pipe);
    audio_pipeline_wait_for_stop(s_pipe);
    audio_pipeline_terminate(s_pipe);
    audio_pipeline_unregister(s_pipe, s_src);
    audio_pipeline_unregister(s_pipe, s_aac);
    audio_pipeline_unregister(s_pipe, s_m2s);
    audio_pipeline_unregister(s_pipe, s_i2s);
    audio_pipeline_remove_listener(s_pipe);
    audio_event_iface_destroy(s_evt);
    audio_pipeline_deinit(s_pipe);
    audio_element_deinit(s_src);
    audio_element_deinit(s_aac);
    audio_element_deinit(s_m2s);
    audio_element_deinit(s_i2s);
    s_pipe = NULL;
    s_src = NULL;
    s_aac = NULL;
    s_m2s = NULL;
    s_i2s = NULL;
    s_evt = NULL;
    s_own_pipe = false;
}

static void clip_reset_decoder(void)
{
    if (!s_aac) {
        return;
    }
    audio_element_reset_input_ringbuf(s_aac);
    audio_element_reset_state(s_aac);
    audio_element_resume(s_aac, 0, 500);
}

static esp_err_t play_id(sb_clip_id_t id, bool loop, bool wait, int timeout_ms)
{
    if (id < 0 || id >= SB_CLIP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *start = clip_start[id];
    size_t len = (size_t)(clip_end[id] - start);
    int dur = sb_adts_duration_ms(start, len);
    int gap = loop ? 800 : 0;

    if (ensure_pipe() != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CLIP play name=%s loop=%d dur_ms=%d wait=%d prefetch=%d",
             clip_name[id], (int)loop, dur, (int)wait, (int)radio_player_is_prefetching());
    clip_src_play(start, len, loop, gap);
    if (!wait) {
        return ESP_OK;
    }

    int wait_ms = dur + SB_CLIP_TAIL_MS;
    if (timeout_ms > 0 && timeout_ms < wait_ms) {
        wait_ms = timeout_ms;
    }
    /* Wait full speaker time — s_src_playing clears when bytes are queued, not when I2S is done. */
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        pump_music_info();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    clip_src_stop();
    return ESP_OK;
}

esp_err_t clip_player_init(int volume)
{
    (void)volume;
    return ESP_OK;
}

void clip_player_set_volume(int volume)
{
    (void)volume;
}

esp_err_t clip_player_loop(sb_clip_id_t id)
{
    return play_id(id, true, false, 0);
}

esp_err_t clip_player_play_wait(sb_clip_id_t id, int timeout_ms)
{
    return play_id(id, false, true, timeout_ms);
}

esp_err_t clip_player_wait_started(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        pump_music_info();
        if (s_src_started) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_src_started ? ESP_OK : ESP_ERR_TIMEOUT;
}

void clip_player_tick(void)
{
    pump_music_info();
}

void clip_player_stop(void)
{
    clip_src_stop();
}

bool clip_player_is_active(void)
{
    return s_src_playing;
}
