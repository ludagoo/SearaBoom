#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include <stdbool.h>
#include "esp_err.h"
#include "searaboom.h"
#include "ringbuf.h"

esp_err_t radio_player_start_idle(int volume);
esp_err_t radio_player_prefetch(const char *url, int volume);
bool radio_player_is_prefetching(void);
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
bool radio_player_has_music_info(void);
void radio_player_beep(void);

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

#endif
