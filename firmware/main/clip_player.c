#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "clip_player.h"
#include "radio_player.h"
#include "volume_buttons.h"
#include "config_store.h"
#include "blob_stream.h"
#include "pcm_upmix.h"
#include "adts_util.h"
#include "searaboom.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "aac_decoder.h"
#include "raw_stream.h"

static const char *TAG = "clip_player";

#define SB_CLIP_LOOP_PAUSE_MS 2500
/* net_slow: 3 plays + 2 gaps = 25 s, matching HTTP-slow reconnect. Then stop. */
#define SB_NET_SLOW_WINDOW_MS 25000
#define SB_NET_SLOW_PLAYS 3
#define SB_WIFI_WEAK_PLAYS 4

#define SB_PROBE_SR 44100
#define SB_PROBE_MS 1000
#define SB_PROBE_START_MS 80
#define SB_PROBE_END_MS 80
#define SB_PROBE_AMP 12000
#define SB_PROBE_START_HZ 8000
#define SB_PROBE_END_HZ 3000
#define SB_PROBE_MID_HZ 440

extern const uint8_t ap_welcome_aac_start[] asm("_binary_ap_welcome_aac_start");
extern const uint8_t ap_welcome_aac_end[] asm("_binary_ap_welcome_aac_end");
extern const uint8_t ap_connected_aac_start[] asm("_binary_ap_connected_aac_start");
extern const uint8_t ap_connected_aac_end[] asm("_binary_ap_connected_aac_end");
extern const uint8_t ap_page_aac_start[] asm("_binary_ap_page_aac_start");
extern const uint8_t ap_page_aac_end[] asm("_binary_ap_page_aac_end");
extern const uint8_t ap_form_station_aac_start[] asm("_binary_ap_form_station_aac_start");
extern const uint8_t ap_form_station_aac_end[] asm("_binary_ap_form_station_aac_end");
extern const uint8_t ap_form_wifi_aac_start[] asm("_binary_ap_form_wifi_aac_start");
extern const uint8_t ap_form_wifi_aac_end[] asm("_binary_ap_form_wifi_aac_end");
extern const uint8_t ap_form_password_aac_start[] asm("_binary_ap_form_password_aac_start");
extern const uint8_t ap_form_password_aac_end[] asm("_binary_ap_form_password_aac_end");
extern const uint8_t ap_form_save_aac_start[] asm("_binary_ap_form_save_aac_start");
extern const uint8_t ap_form_save_aac_end[] asm("_binary_ap_form_save_aac_end");
extern const uint8_t ap_saved_aac_start[] asm("_binary_ap_saved_aac_start");
extern const uint8_t ap_saved_aac_end[] asm("_binary_ap_saved_aac_end");
extern const uint8_t tune_102_aac_start[] asm("_binary_tune_102_aac_start");
extern const uint8_t tune_102_aac_end[] asm("_binary_tune_102_aac_end");
extern const uint8_t tune_104_aac_start[] asm("_binary_tune_104_aac_start");
extern const uint8_t tune_104_aac_end[] asm("_binary_tune_104_aac_end");
extern const uint8_t wifi_weak_aac_start[] asm("_binary_wifi_weak_aac_start");
extern const uint8_t wifi_weak_aac_end[] asm("_binary_wifi_weak_aac_end");
extern const uint8_t net_slow_aac_start[] asm("_binary_net_slow_aac_start");
extern const uint8_t net_slow_aac_end[] asm("_binary_net_slow_aac_end");
extern const uint8_t ota_available_aac_start[] asm("_binary_ota_available_aac_start");
extern const uint8_t ota_available_aac_end[] asm("_binary_ota_available_aac_end");
extern const uint8_t ota_rebooting_aac_start[] asm("_binary_ota_rebooting_aac_start");
extern const uint8_t ota_rebooting_aac_end[] asm("_binary_ota_rebooting_aac_end");
extern const uint8_t ota_done_aac_start[] asm("_binary_ota_done_aac_start");
extern const uint8_t ota_done_aac_end[] asm("_binary_ota_done_aac_end");

static const char *clip_name[SB_CLIP_COUNT] = {
    [SB_CLIP_AP_WELCOME] = "ap_welcome",
    [SB_CLIP_AP_CONNECTED] = "ap_connected",
    [SB_CLIP_AP_PAGE] = "ap_page",
    [SB_CLIP_AP_STATION] = "ap_form_station",
    [SB_CLIP_AP_WIFI] = "ap_form_wifi",
    [SB_CLIP_AP_PASSWORD] = "ap_form_password",
    [SB_CLIP_AP_SAVEBTN] = "ap_form_save",
    [SB_CLIP_AP_SAVED] = "ap_saved",
    [SB_CLIP_TUNE_102] = "tune_102",
    [SB_CLIP_TUNE_104] = "tune_104",
    [SB_CLIP_WIFI_WEAK] = "wifi_weak",
    [SB_CLIP_NET_SLOW] = "net_slow",
    [SB_CLIP_OTA_AVAILABLE] = "ota_available",
    [SB_CLIP_OTA_REBOOTING] = "ota_rebooting",
    [SB_CLIP_OTA_DONE] = "ota_done",
};

static const uint8_t *clip_start[SB_CLIP_COUNT] = {
    [SB_CLIP_AP_WELCOME] = ap_welcome_aac_start,
    [SB_CLIP_AP_CONNECTED] = ap_connected_aac_start,
    [SB_CLIP_AP_PAGE] = ap_page_aac_start,
    [SB_CLIP_AP_STATION] = ap_form_station_aac_start,
    [SB_CLIP_AP_WIFI] = ap_form_wifi_aac_start,
    [SB_CLIP_AP_PASSWORD] = ap_form_password_aac_start,
    [SB_CLIP_AP_SAVEBTN] = ap_form_save_aac_start,
    [SB_CLIP_AP_SAVED] = ap_saved_aac_start,
    [SB_CLIP_TUNE_102] = tune_102_aac_start,
    [SB_CLIP_TUNE_104] = tune_104_aac_start,
    [SB_CLIP_WIFI_WEAK] = wifi_weak_aac_start,
    [SB_CLIP_NET_SLOW] = net_slow_aac_start,
    [SB_CLIP_OTA_AVAILABLE] = ota_available_aac_start,
    [SB_CLIP_OTA_REBOOTING] = ota_rebooting_aac_start,
    [SB_CLIP_OTA_DONE] = ota_done_aac_start,
};
static const uint8_t *clip_end[SB_CLIP_COUNT] = {
    [SB_CLIP_AP_WELCOME] = ap_welcome_aac_end,
    [SB_CLIP_AP_CONNECTED] = ap_connected_aac_end,
    [SB_CLIP_AP_PAGE] = ap_page_aac_end,
    [SB_CLIP_AP_STATION] = ap_form_station_aac_end,
    [SB_CLIP_AP_WIFI] = ap_form_wifi_aac_end,
    [SB_CLIP_AP_PASSWORD] = ap_form_password_aac_end,
    [SB_CLIP_AP_SAVEBTN] = ap_form_save_aac_end,
    [SB_CLIP_AP_SAVED] = ap_saved_aac_end,
    [SB_CLIP_TUNE_102] = tune_102_aac_end,
    [SB_CLIP_TUNE_104] = tune_104_aac_end,
    [SB_CLIP_WIFI_WEAK] = wifi_weak_aac_end,
    [SB_CLIP_NET_SLOW] = net_slow_aac_end,
    [SB_CLIP_OTA_AVAILABLE] = ota_available_aac_end,
    [SB_CLIP_OTA_REBOOTING] = ota_rebooting_aac_end,
    [SB_CLIP_OTA_DONE] = ota_done_aac_end,
};

static int s_volume = SB_VOLUME_MAX;
static SemaphoreHandle_t s_mu;
static audio_pipeline_handle_t s_pipe;
static audio_element_handle_t s_blob;
static audio_element_handle_t s_aac;
static audio_element_handle_t s_m2s;
static audio_element_handle_t s_raw;
static audio_event_iface_handle_t s_evt;
static volatile bool s_active;
static volatile bool s_finished;
static volatile bool s_loop;
static sb_clip_id_t s_playing_id = SB_CLIP_COUNT;
static sb_clip_id_t s_pending_page = SB_CLIP_COUNT;
static sb_clip_id_t s_pending_field = SB_CLIP_COUNT;
static int64_t s_loop_at;
static int64_t s_play_started_us;
static int s_play_dur_ms;
static int s_loop_plays;

static audio_pipeline_handle_t s_probe_pipe;
static audio_element_handle_t s_probe;
static audio_element_handle_t s_probe_raw;
static volatile int s_probe_frame;
static volatile int s_probe_total;

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

static void lock(void)
{
    if (s_mu) {
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mu) {
        xSemaphoreGiveRecursive(s_mu);
    }
}

static bool clip_blob(sb_clip_id_t id, const uint8_t **data, size_t *len, int *dur)
{
    if (id < 0 || id >= SB_CLIP_COUNT) {
        return false;
    }
    *data = clip_start[id];
    *len = (size_t)(clip_end[id] - clip_start[id]);
    *dur = sb_adts_duration_ms(*data, *len);
    return true;
}

const char *clip_player_name(sb_clip_id_t id)
{
    if (id < 0 || id >= SB_CLIP_COUNT) {
        return "?";
    }
    return clip_name[id];
}

int clip_player_duration_ms(sb_clip_id_t id)
{
    const uint8_t *data;
    size_t len;
    int dur;
    if (!clip_blob(id, &data, &len, &dur)) {
        return 0;
    }
    return dur;
}

static int clip_loop_pause_ms(sb_clip_id_t id)
{
    if (id != SB_CLIP_NET_SLOW) {
        return SB_CLIP_LOOP_PAUSE_MS;
    }
    int dur = s_play_dur_ms > 0 ? s_play_dur_ms : clip_player_duration_ms(id);
    if (dur <= 0 || SB_NET_SLOW_PLAYS < 2) {
        return SB_CLIP_LOOP_PAUSE_MS;
    }
    int pause = (SB_NET_SLOW_WINDOW_MS - SB_NET_SLOW_PLAYS * dur)
                / (SB_NET_SLOW_PLAYS - 1);
    if (pause < 500) {
        pause = 500;
    }
    return pause;
}

esp_err_t clip_player_init(int volume)
{
    s_volume = volume;
    if (!s_mu) {
        s_mu = xSemaphoreCreateRecursiveMutex();
    }
    return ESP_OK;
}

void clip_player_set_volume(int volume)
{
    s_volume = volume;
    radio_player_set_volume(volume);
}

static void drain_evt(void)
{
    if (!s_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    while (audio_event_iface_listen(s_evt, &msg, 0) == ESP_OK) {
    }
}

static void clip_pipe_halt(void)
{
    if (!s_pipe) {
        return;
    }
    /* After natural EOS the elements are FINISHED but the pipeline is still
     * RUNNING. audio_pipeline_run is a no-op unless state is INIT, so always
     * stop/wait/reset even when nothing looks "busy". */
    audio_pipeline_stop(s_pipe);
    audio_pipeline_wait_for_stop(s_pipe);
    audio_pipeline_reset_ringbuffer(s_pipe);
    audio_pipeline_reset_elements(s_pipe);
    audio_pipeline_reset_items_state(s_pipe);
    audio_pipeline_change_state(s_pipe, AEL_STATE_INIT);
    drain_evt();
}

static void clip_evt_release(void)
{
    if (s_pipe || s_probe_pipe || !s_evt) {
        return;
    }
    audio_event_iface_destroy(s_evt);
    s_evt = NULL;
}

static void clip_pipe_deinit(void)
{
    if (!s_pipe) {
        return;
    }
    clip_pipe_halt();
    /* Listener first, while element queues still exist. Unregister next so
     * deinit's terminate cannot abort a mix-linked rb. Destroy s_evt before
     * element_deinit or a later set_listener spins "Error remove listener". */
    audio_pipeline_remove_listener(s_pipe);
    if (s_blob) {
        audio_pipeline_unregister(s_pipe, s_blob);
    }
    if (s_aac) {
        audio_pipeline_unregister(s_pipe, s_aac);
    }
    if (s_m2s) {
        audio_pipeline_unregister(s_pipe, s_m2s);
    }
    if (s_raw) {
        audio_pipeline_unregister(s_pipe, s_raw);
    }
    audio_pipeline_deinit(s_pipe);
    s_pipe = NULL;
    clip_evt_release();
    if (s_blob) {
        audio_element_deinit(s_blob);
        s_blob = NULL;
    }
    if (s_aac) {
        audio_element_deinit(s_aac);
        s_aac = NULL;
    }
    if (s_m2s) {
        audio_element_deinit(s_m2s);
        s_m2s = NULL;
    }
    if (s_raw) {
        audio_element_deinit(s_raw);
        s_raw = NULL;
    }
    ESP_LOGI(TAG, "clip pipe released internal=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void probe_pipe_halt(void)
{
    if (!s_probe_pipe) {
        return;
    }
    audio_pipeline_stop(s_probe_pipe);
    audio_pipeline_wait_for_stop(s_probe_pipe);
    audio_pipeline_reset_ringbuffer(s_probe_pipe);
    audio_pipeline_reset_elements(s_probe_pipe);
    audio_pipeline_reset_items_state(s_probe_pipe);
    audio_pipeline_change_state(s_probe_pipe, AEL_STATE_INIT);
}

static void probe_pipe_deinit(void)
{
    if (!s_probe_pipe) {
        return;
    }
    probe_pipe_halt();
    audio_pipeline_remove_listener(s_probe_pipe);
    if (s_probe) {
        audio_pipeline_unregister(s_probe_pipe, s_probe);
    }
    if (s_probe_raw) {
        audio_pipeline_unregister(s_probe_pipe, s_probe_raw);
    }
    audio_pipeline_deinit(s_probe_pipe);
    s_probe_pipe = NULL;
    clip_evt_release();
    if (s_probe) {
        audio_element_deinit(s_probe);
        s_probe = NULL;
    }
    if (s_probe_raw) {
        audio_element_deinit(s_probe_raw);
        s_probe_raw = NULL;
    }
}

static esp_err_t ensure_clip_pipe(void)
{
    if (s_pipe) {
        ringbuf_handle_t rb = audio_element_get_input_ringbuf(s_raw);
        return radio_player_attach_clip_pcm(rb);
    }
    s_blob = blob_stream_init();
    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    aac_cfg.plus_enable = false;
    aac_cfg.out_rb_size = 8 * 1024;
    aac_cfg.task_stack = 6 * 1024;
    aac_cfg.task_core = 1;
    aac_cfg.stack_in_ext = true;
    s_aac = aac_decoder_init(&aac_cfg);
    s_m2s = pcm_upmix_init_core("cm2s", 1);
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 16 * 1024;
    s_raw = raw_stream_init(&raw_cfg);
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipe = audio_pipeline_init(&pipeline_cfg);
    if (!s_pipe || !s_blob || !s_aac || !s_m2s || !s_raw) {
        ESP_LOGE(TAG, "clip pipe init failed");
        return ESP_FAIL;
    }
    audio_pipeline_register(s_pipe, s_blob, "blob");
    audio_pipeline_register(s_pipe, s_aac, "caac");
    audio_pipeline_register(s_pipe, s_m2s, "cm2s");
    audio_pipeline_register(s_pipe, s_raw, "craw");
    const char *link[4] = {"blob", "caac", "cm2s", "craw"};
    audio_pipeline_link(s_pipe, &link[0], 4);
    if (!s_evt) {
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        s_evt = audio_event_iface_init(&evt_cfg);
    }
    audio_pipeline_set_listener(s_pipe, s_evt);
    ringbuf_handle_t rb = audio_element_get_input_ringbuf(s_raw);
    if (radio_player_attach_clip_pcm(rb) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t probe_open(audio_element_handle_t self)
{
    (void)self;
    s_probe_frame = 0;
    s_probe_total = (SB_PROBE_SR * SB_PROBE_MS) / 1000;
    audio_element_info_t info = {0};
    info.sample_rates = SB_PROBE_SR;
    info.bits = 16;
    info.channels = 2;
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int probe_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int frames_room = in_len / 4;
    int remain = s_probe_total - s_probe_frame;
    if (remain <= 0 || frames_room <= 0) {
        return AEL_IO_OK;
    }
    int n = frames_room < remain ? frames_room : remain;
    int16_t *out = (int16_t *)in_buffer;
    for (int i = 0; i < n; i++) {
        int frame = s_probe_frame + i;
        int ms = (int)((frame * 1000LL) / SB_PROBE_SR);
        int hz = SB_PROBE_MID_HZ;
        if (ms < SB_PROBE_START_MS) {
            hz = SB_PROBE_START_HZ;
        } else if (ms >= SB_PROBE_MS - SB_PROBE_END_MS) {
            hz = SB_PROBE_END_HZ;
        }
        uint32_t phase = (uint32_t)(((uint64_t)frame * (uint32_t)hz * 65536u) / SB_PROBE_SR);
        int16_t s = (int16_t)(((int32_t)soft_sine(phase) * SB_PROBE_AMP) / 32767);
        out[i * 2] = s;
        out[i * 2 + 1] = s;
    }
    s_probe_frame += n;
    int w = audio_element_output(self, in_buffer, n * 4);
    if (s_probe_frame >= s_probe_total) {
        return w > 0 ? w : AEL_IO_OK;
    }
    return w;
}

static esp_err_t ensure_probe_pipe(void)
{
    if (s_probe_pipe) {
        return ESP_OK;
    }
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = probe_open;
    cfg.process = probe_process;
    cfg.tag = "probe";
    cfg.out_rb_size = 8 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 6;
    cfg.task_core = 0;
    cfg.stack_in_ext = true;
    cfg.buffer_len = 2048;
    s_probe = audio_element_init(&cfg);
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 16 * 1024;
    s_probe_raw = raw_stream_init(&raw_cfg);
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_probe_pipe = audio_pipeline_init(&pipeline_cfg);
    if (!s_probe_pipe || !s_probe || !s_probe_raw) {
        return ESP_FAIL;
    }
    audio_pipeline_register(s_probe_pipe, s_probe, "probe");
    audio_pipeline_register(s_probe_pipe, s_probe_raw, "praw");
    const char *link[2] = {"probe", "praw"};
    audio_pipeline_link(s_probe_pipe, &link[0], 2);
    if (!s_evt) {
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        s_evt = audio_event_iface_init(&evt_cfg);
    }
    audio_pipeline_set_listener(s_probe_pipe, s_evt);
    return ESP_OK;
}

static int clip_elapsed_ms(void)
{
    if (s_play_started_us <= 0) {
        return 0;
    }
    return (int)((esp_timer_get_time() - s_play_started_us) / 1000);
}

static bool clip_pcm_drained(void)
{
    ringbuf_handle_t rb = s_raw ? audio_element_get_input_ringbuf(s_raw) : NULL;
    int filled = rb ? rb_bytes_filled(rb) : 0;
    if (filled > 256) {
        return false;
    }
    if (radio_player_pcm_finished(120)) {
        return true;
    }
    int elapsed = clip_elapsed_ms();
    if (s_play_dur_ms > 0 && elapsed >= s_play_dur_ms + 1200) {
        ESP_LOGW(TAG, "clip drain timeout elapsed_ms=%d dur_ms=%d rb=%d",
                 elapsed, s_play_dur_ms, filled);
        return true;
    }
    return false;
}

static void handle_clip_events(void)
{
    if (!s_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    while (audio_event_iface_listen(s_evt, &msg, 0) == ESP_OK) {
        if (msg.source_type != AUDIO_ELEMENT_TYPE_ELEMENT
            || msg.cmd != AEL_MSG_CMD_REPORT_STATUS) {
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_aac
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                audio_element_info_t info = {0};
                audio_element_getinfo(s_aac, &info);
                ESP_LOGI(TAG, "clip music info rate=%d bits=%d ch=%d",
                         info.sample_rates, info.bits, info.channels);
                if (s_m2s) {
                    audio_element_set_music_info(s_m2s, 44100, 1, 16);
                }
            }
            continue;
        }
        int st = (int)msg.data;
        if (st != AEL_STATUS_STATE_FINISHED) {
            continue;
        }
        const char *who = NULL;
        if (msg.source == (void *)s_blob) {
            who = "blob";
        } else if (msg.source == (void *)s_aac) {
            who = "caac";
        } else if (msg.source == (void *)s_m2s) {
            who = "cm2s";
        } else if (msg.source == (void *)s_probe) {
            who = "probe";
        } else {
            continue;
        }
        ESP_LOGI(TAG, "clip el %s finished elapsed_ms=%d dur_ms=%d",
                 who, clip_elapsed_ms(), s_play_dur_ms);
        /* Blob/AAC EOS only means compressed bytes are queued. Halt there
         * and the last words never reach I2S. PCM is done when upmix finishes. */
        if (msg.source == (void *)s_m2s || msg.source == (void *)s_probe) {
            s_finished = true;
        }
    }
}

static void abort_pcm_rb(audio_element_handle_t raw)
{
    if (!raw) {
        return;
    }
    ringbuf_handle_t rb = audio_element_get_input_ringbuf(raw);
    if (rb) {
        rb_abort(rb);
    }
}

static void detach_clip_from_mix(void)
{
    /* Mute mix before aborting the clip rb. Clip slot timeout is 40 ticks;
     * 20 ms was short enough that mix still waited on the clip rb, took
     * ABORT, and finished I2S (silent after station-switch ident). */
    radio_player_set_clip_active(false);
    vTaskDelay(pdMS_TO_TICKS(80));
    abort_pcm_rb(s_raw);
    abort_pcm_rb(s_probe_raw);
    radio_player_attach_clip_pcm(NULL);
}

static void halt_playback(void);

void clip_player_tick(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
    volume_buttons_poll();
    config_store_flush_deferred();
    radio_player_loop();
    lock();
    handle_clip_events();
    sb_clip_id_t start_id = SB_CLIP_COUNT;
    bool start_loop = false;
    if (s_finished && s_loop && s_playing_id < SB_CLIP_COUNT && s_loop_at == 0) {
        if (!clip_pcm_drained()) {
            unlock();
            return;
        }
        int max_plays = 0;
        if (s_playing_id == SB_CLIP_NET_SLOW) {
            max_plays = SB_NET_SLOW_PLAYS;
        } else if (s_playing_id == SB_CLIP_WIFI_WEAK) {
            max_plays = SB_WIFI_WEAK_PLAYS;
        }
        if (max_plays && s_loop_plays >= max_plays) {
            ESP_LOGI(TAG, "clip loop stop after %d plays (%s)", s_loop_plays,
                     clip_name[s_playing_id]);
            halt_playback();
            unlock();
            return;
        }
        int pause_ms = clip_loop_pause_ms(s_playing_id);
        s_loop_at = esp_timer_get_time() + (int64_t)pause_ms * 1000LL;
        s_active = false;
        radio_player_set_clip_active(false);
        clip_pipe_halt();
        ESP_LOGI(TAG, "clip loop pause %d ms after %d ms play %d", pause_ms,
                 clip_elapsed_ms(), s_loop_plays);
    }
    if (s_loop_at && s_loop && s_playing_id < SB_CLIP_COUNT
        && esp_timer_get_time() >= s_loop_at) {
        start_id = s_playing_id;
        start_loop = true;
        s_loop_plays++;
        s_loop_at = 0;
        s_finished = false;
        s_active = false;
    } else if (s_finished && !s_loop) {
        /* One-shot clips used to stay s_active forever. The portal then
         * thought CONNECTED/PAGE was still playing and never started fields. */
        if (!clip_pcm_drained()) {
            unlock();
            return;
        }
        if (s_pending_page < SB_CLIP_COUNT) {
            start_id = s_pending_page;
            start_loop = false;
            s_pending_page = SB_CLIP_COUNT;
            s_finished = false;
            s_loop_at = 0;
            s_active = false;
        } else if (s_pending_field < SB_CLIP_COUNT) {
            start_id = s_pending_field;
            start_loop = false;
            s_pending_field = SB_CLIP_COUNT;
            s_finished = false;
            s_loop_at = 0;
            s_active = false;
        } else {
            ESP_LOGI(TAG, "clip done after %d ms", clip_elapsed_ms());
            halt_playback();
            /* Keep Helix until clip_player_release_idle() after go_live.
             * Deinit here (play_wait ident) aborts mix before the station
             * is routed — double-tap then stays silent. */
        }
    }
    unlock();
    if (start_id < SB_CLIP_COUNT) {
        clip_player_play(start_id, start_loop);
    }
}

static bool clip_is_field(sb_clip_id_t id)
{
    return id == SB_CLIP_AP_STATION || id == SB_CLIP_AP_WIFI
        || id == SB_CLIP_AP_PASSWORD || id == SB_CLIP_AP_SAVEBTN;
}

static void halt_playback(void)
{
    s_loop = false;
    s_loop_at = 0;
    s_loop_plays = 0;
    s_active = false;
    s_finished = false;
    s_playing_id = SB_CLIP_COUNT;
    detach_clip_from_mix();
    clip_pipe_halt();
    probe_pipe_halt();
}

static void stop_locked(void)
{
    s_pending_page = SB_CLIP_COUNT;
    s_pending_field = SB_CLIP_COUNT;
    halt_playback();
}

void clip_player_stop(void)
{
    lock();
    stop_locked();
    unlock();
}

void clip_player_release_idle(void)
{
    lock();
    if (!s_active && !s_loop && s_loop_at == 0) {
        clip_pipe_deinit();
        probe_pipe_deinit();
    }
    unlock();
}

bool clip_player_is_active(void)
{
    return s_active || (s_loop && s_playing_id < SB_CLIP_COUNT);
}

sb_clip_id_t clip_player_playing(void)
{
    if (s_active || (s_loop && s_playing_id < SB_CLIP_COUNT)) {
        return s_playing_id;
    }
    return SB_CLIP_COUNT;
}

esp_err_t clip_player_play(sb_clip_id_t id, bool loop)
{
    const uint8_t *data;
    size_t len;
    int dur;
    if (!clip_blob(id, &data, &len, &dur)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_active && !s_finished && s_loop_at == 0 && s_playing_id == id) {
        ESP_LOGI(TAG, "CLIP already playing %s", clip_name[id]);
        unlock();
        return ESP_OK;
    }
    if (radio_player_start_idle(s_volume) != ESP_OK) {
        unlock();
        return ESP_FAIL;
    }
    /* tick() increments s_loop_plays then calls play() to restart the
     * same loop. halt_playback() used to zero that count and play() set
     * it back to 1, so net_slow/wifi_weak never reached max_plays. */
    int keep_plays = 0;
    if (loop && s_playing_id == id && s_loop_plays > 0) {
        keep_plays = s_loop_plays;
    }
    halt_playback();
    if (ensure_clip_pipe() != ESP_OK) {
        unlock();
        return ESP_FAIL;
    }
    blob_stream_set_data(s_blob, data, len);
    audio_element_set_music_info(s_m2s, 44100, 1, 16);
    s_loop = loop;
    s_loop_at = 0;
    s_loop_plays = keep_plays > 0 ? keep_plays : 1;
    s_playing_id = id;
    if (id == SB_CLIP_AP_PAGE) {
        s_pending_page = SB_CLIP_COUNT;
    }
    if (clip_is_field(id) && s_pending_field == id) {
        s_pending_field = SB_CLIP_COUNT;
    }
    s_finished = false;
    s_active = true;
    s_play_dur_ms = dur;
    s_play_started_us = esp_timer_get_time();
    radio_player_pcm_arm();
    radio_player_set_clip_active(true);
    ESP_LOGI(TAG, "CLIP play name=%s loop=%d dur_ms=%d prefetch=%d",
             clip_name[id], (int)loop, dur, (int)radio_player_is_prefetching());
    esp_err_t err = audio_pipeline_run(s_pipe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clip pipeline_run failed");
        s_active = false;
        radio_player_set_clip_active(false);
    } else {
        ESP_LOGI(TAG, "clip el state blob=%d aac=%d m2s=%d",
                 (int)audio_element_get_state(s_blob),
                 (int)audio_element_get_state(s_aac),
                 (int)audio_element_get_state(s_m2s));
    }
    unlock();
    return err;
}

esp_err_t clip_player_loop(sb_clip_id_t id)
{
    return clip_player_play(id, true);
}

static esp_err_t wait_finished(int timeout_ms)
{
    int64_t give_up = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < give_up) {
        clip_player_tick();
        if (s_finished && !s_loop) {
            int64_t pad_until = esp_timer_get_time() + 250 * 1000;
            while (esp_timer_get_time() < pad_until) {
                volume_buttons_poll();
                config_store_flush_deferred();
                radio_player_loop();
                if (radio_player_pcm_finished(40)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            lock();
            halt_playback();
            unlock();
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "clip wait timed out");
    clip_player_stop();
    return ESP_ERR_TIMEOUT;
}

esp_err_t clip_player_play_wait(sb_clip_id_t id, int timeout_ms)
{
    clip_player_stop();
    int dur = clip_player_duration_ms(id);
    esp_err_t err = clip_player_play(id, false);
    if (err != ESP_OK) {
        return err;
    }
    int budget = timeout_ms > 0 ? timeout_ms : (dur + 4000);
    if (budget < dur + 1500) {
        budget = dur + 1500;
    }
    return wait_finished(budget);
}

esp_err_t clip_player_wait_started(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        clip_player_tick();
        if (radio_player_pcm_heard()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return radio_player_pcm_heard() ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t clip_player_run_pcm_probe(radio_probe_result_t *out, int timeout_ms)
{
    lock();
    if (radio_player_start_idle(s_volume) != ESP_OK) {
        unlock();
        return ESP_FAIL;
    }
    stop_locked();
    if (ensure_probe_pipe() != ESP_OK) {
        unlock();
        return ESP_FAIL;
    }
    ringbuf_handle_t rb = audio_element_get_input_ringbuf(s_probe_raw);
    if (radio_player_attach_clip_pcm(rb) != ESP_OK) {
        unlock();
        return ESP_FAIL;
    }
    s_probe_frame = 0;
    s_probe_total = (SB_PROBE_SR * SB_PROBE_MS) / 1000;
    s_loop = false;
    s_finished = false;
    s_active = true;
    radio_player_probe_arm();
    radio_player_set_clip_active(true);
    ESP_LOGI(TAG, "PCM probe %d ms (8 kHz / 440 Hz / 3 kHz)", SB_PROBE_MS);
    esp_err_t err = audio_pipeline_run(s_probe_pipe);
    unlock();
    if (err != ESP_OK) {
        return err;
    }
    int budget = timeout_ms > 0 ? timeout_ms : 4000;
    err = wait_finished(budget);
    if (out) {
        radio_player_probe_result(out);
    }
    return err;
}

esp_err_t clip_player_run_clip_probe(sb_clip_id_t id, int *heard_ms, int timeout_ms)
{
    radio_player_probe_arm();
    esp_err_t err = clip_player_play_wait(id, timeout_ms);
    radio_probe_result_t r;
    radio_player_probe_result(&r);
    if (heard_ms) {
        *heard_ms = r.dur_ms;
    }
    return err;
}
