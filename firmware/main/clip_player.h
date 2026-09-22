#ifndef CLIP_PLAYER_H
#define CLIP_PLAYER_H

#include <stdbool.h>
#include "esp_err.h"
#include "radio_player.h"

typedef enum {
    SB_CLIP_AP_WELCOME = 0,
    SB_CLIP_AP_CONNECTED,
    SB_CLIP_AP_PAGE,
    SB_CLIP_AP_STATION,
    SB_CLIP_AP_WIFI,
    SB_CLIP_AP_PASSWORD,
    SB_CLIP_AP_SAVEBTN,
    SB_CLIP_AP_SAVED,
    SB_CLIP_TUNE_102,
    SB_CLIP_TUNE_104,
    SB_CLIP_WIFI_WEAK,
    SB_CLIP_NET_SLOW,
    SB_CLIP_OTA_AVAILABLE,
    SB_CLIP_OTA_REBOOTING,
    SB_CLIP_OTA_DONE,
    SB_CLIP_PREBUF,
    SB_CLIP_COUNT
} sb_clip_id_t;

esp_err_t clip_player_init(int volume);
void clip_player_set_volume(int volume);

esp_err_t clip_player_play(sb_clip_id_t id, bool loop);
esp_err_t clip_player_loop(sb_clip_id_t id);
esp_err_t clip_player_play_wait(sb_clip_id_t id, int timeout_ms);
esp_err_t clip_player_wait_started(int timeout_ms);

void clip_player_stop(void);
/* Destroy clip/probe pipes if nothing is playing. Call after go_live —
 * never during play_wait ident, or mix/I2S stay silent. */
void clip_player_release_idle(void);
bool clip_player_is_active(void);
sb_clip_id_t clip_player_playing(void);
const char *clip_player_name(sb_clip_id_t id);
int clip_player_duration_ms(sb_clip_id_t id);
void clip_player_tick(void);

esp_err_t clip_player_run_pcm_probe(radio_probe_result_t *out, int timeout_ms);
esp_err_t clip_player_run_clip_probe(sb_clip_id_t id, int *heard_ms, int timeout_ms);

#endif
