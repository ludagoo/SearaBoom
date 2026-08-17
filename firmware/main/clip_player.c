#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "clip_player.h"
#include "radio_player.h"
#include "aac_inject.h"
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
#define SB_CLIP_TAIL_MS (500)

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
static audio_element_handle_t s_aac;
static audio_element_handle_t s_m2s;
static audio_element_handle_t s_i2s;
static audio_event_iface_handle_t s_evt;
static bool s_own_pipe;

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
    int w = audio_element_output(self, out, out_bytes);
    return w;
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

static void clip_on_clip(bool active)
{
    if (!s_i2s || !active) {
        return;
    }
    /* Do not call i2s_stream_set_clk here — it pauses I2S and drops SoftAP STAs. */
    i2s_alc_volume_set(s_i2s, SB_ALC_MAX_DB);
}

static void pump_music_info(void)
{
    if (!s_evt || !s_aac || !s_i2s) {
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
    if (radio_player_is_running() || s_own_pipe) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "starting clip-only pipe (inj -> aac -> m2s -> i2s)");
    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle && board_handle->audio_hal) {
        audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    }

    audio_element_handle_t inj = aac_inject_init();
    aac_inject_set_passthrough(false);
    aac_inject_hooks_t hooks = {
        .reset_decoder = clip_reset_decoder,
        .on_clip = clip_on_clip,
    };
    aac_inject_set_hooks(&hooks);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipe = audio_pipeline_init(&pipeline_cfg);

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

    audio_pipeline_register(s_pipe, inj, "inj");
    audio_pipeline_register(s_pipe, s_aac, "aac");
    audio_pipeline_register(s_pipe, s_m2s, "m2s");
    audio_pipeline_register(s_pipe, s_i2s, "i2s");
    const char *link[] = {"inj", "aac", "m2s", "i2s"};
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
    aac_inject_stop();
    if (!s_own_pipe) {
        return;
    }
    audio_pipeline_stop(s_pipe);
    audio_pipeline_wait_for_stop(s_pipe);
    audio_pipeline_terminate(s_pipe);
    audio_pipeline_unregister(s_pipe, aac_inject_element());
    audio_pipeline_unregister(s_pipe, s_aac);
    audio_pipeline_unregister(s_pipe, s_m2s);
    audio_pipeline_unregister(s_pipe, s_i2s);
    audio_pipeline_remove_listener(s_pipe);
    audio_event_iface_destroy(s_evt);
    audio_pipeline_deinit(s_pipe);
    audio_element_deinit(s_aac);
    audio_element_deinit(s_m2s);
    audio_element_deinit(s_i2s);
    aac_inject_set_hooks(NULL);
    aac_inject_deinit();
    s_pipe = NULL;
    s_aac = NULL;
    s_m2s = NULL;
    s_i2s = NULL;
    s_evt = NULL;
    s_own_pipe = false;
}

static esp_err_t play_id(sb_clip_id_t id, bool loop, bool wait, int timeout_ms)
{
    if (id < 0 || id >= SB_CLIP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *start = clip_start[id];
    size_t len = (size_t)(clip_end[id] - start);
    int dur = sb_adts_duration_ms(start, len);
    /* Short pause only — a clip-length TIMEOUT gap stalls AAC and the next play dies. */
    int gap = loop ? 800 : 0;
    if (!radio_player_is_running()) {
        if (ensure_pipe() != ESP_OK) {
            return ESP_FAIL;
        }
    }
    pump_music_info();
    ESP_LOGI(TAG, "CLIP play name=%s loop=%d dur_ms=%d wait=%d",
             clip_name[id], (int)loop, dur, (int)wait);
    esp_err_t err = aac_inject_play(start, len, loop, gap);
    if (err != ESP_OK) {
        return err;
    }
    if (!wait) {
        return ESP_OK;
    }
    /* Inject queues the whole file in milliseconds. Wait out speaker time. */
    int wait_ms = dur + SB_CLIP_TAIL_MS;
    if (timeout_ms > 0 && timeout_ms < wait_ms) {
        wait_ms = timeout_ms;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        pump_music_info();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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
        if (aac_inject_has_started()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return aac_inject_has_started() ? ESP_OK : ESP_ERR_TIMEOUT;
}

void clip_player_tick(void)
{
    pump_music_info();
}

void clip_player_stop(void)
{
    aac_inject_stop();
}

bool clip_player_is_active(void)
{
    return aac_inject_is_playing();
}
