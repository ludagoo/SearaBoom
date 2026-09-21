#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include <stdbool.h>
#include "esp_err.h"
#include "searaboom.h"
#include "ringbuf.h"

esp_err_t radio_player_start_idle(int volume);
esp_err_t radio_player_prefetch(const char *url, int volume);
bool radio_player_is_prefetching(void);
/* Mixer is muted until HTTP rb reaches start/recover (112 KB). */
bool radio_player_is_prebuffering(void);
esp_err_t radio_player_go_live(void);
esp_err_t radio_player_start(const char *url, int volume);
void radio_player_stop(void);
void radio_player_request_stop(void);
void radio_player_resume(void);
bool radio_player_is_running(void);
bool radio_player_has_output(void);
void radio_player_loop(void);
esp_err_t radio_player_set_volume(int volume);
int radio_player_get_volume(void);
int radio_player_alc_db(int volume);
/* Knob +/−. Uses the live player step (not NVS). Returns the new step. */
int radio_player_nudge_volume(int delta);
bool radio_player_has_music_info(void);
void radio_player_beep(void);
void radio_player_beep_limit(void);
void radio_player_ungate(void);

void radio_player_pcm_arm(void);
bool radio_player_pcm_heard(void);
bool radio_player_pcm_flowing(void);
int radio_player_pcm_peak(void);
bool radio_player_pcm_finished(int silence_ms);

/* Clip pipeline attaches its PCM ringbuf as mixer slot 1. */
esp_err_t radio_player_attach_clip_pcm(ringbuf_handle_t rb);
void radio_player_set_clip_active(bool on);

typedef struct {
    int start_ok;
    int end_ok;
    int dur_ms;
    int peak;
    int start_hz;
    int end_hz;
} radio_probe_result_t;

void radio_player_probe_arm(void);
void radio_player_probe_result(radio_probe_result_t *out);

/* WIFI_EVENT_STA_BSS_RSSI_LOW handler: latch only. Play from the app loop. */
void radio_player_on_rssi_low(int rssi_dbm);
void radio_player_on_sta_lost(void);
void radio_player_on_sta_got_ip(void);
/* True once after STA lost while streaming — stop the net_slow clip in app loop. */
bool radio_player_take_stop_clip(void);
void radio_player_arm_rssi_threshold(void);
void radio_player_hold_stream(bool on);
bool radio_player_wifi_weak_holding(void);
bool radio_player_wifi_weak_should_speak(void);
bool radio_player_wifi_weak_resume_ready(void);
/* HTTP rb draining with RSSI still OK: hold + "internet lenta" prompt. */
bool radio_player_http_slow_should_speak(void);

/* Station HTTP ringbuf fill in bytes, or -1 if not streaming. */
int radio_player_http_buffered(void);
/* One-line stall snapshot (HTTP rb, PCM rb, RSSI, element state). */
void radio_player_log_health(const char *why);

#endif
