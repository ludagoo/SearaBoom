#ifndef CLIP_PLAYER_H
#define CLIP_PLAYER_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    SB_CLIP_OTA_UPDATING = 0,
    SB_CLIP_OTA_DONE,
    SB_CLIP_AP_WELCOME,
    SB_CLIP_AP_CONNECTED,
    SB_CLIP_AP_SAVED,
    SB_CLIP_COUNT
} sb_clip_id_t;

esp_err_t clip_player_init(int volume);
void clip_player_set_volume(int volume); /* ignored; clips always play at full volume */

/* Loop clip in the background. Silence between repeats equals the clip length. */
esp_err_t clip_player_loop(sb_clip_id_t id);

/* Play once and block until finished (or timeout). */
esp_err_t clip_player_play_wait(sb_clip_id_t id, int timeout_ms);

/* Wait until the clip has started feeding the decoder. */
esp_err_t clip_player_wait_started(int timeout_ms);

void clip_player_stop(void);
bool clip_player_is_active(void);
/* Drain decoder events; does not change I2S clock. */
void clip_player_tick(void);
/* Tear down the clip-only output pipe so radio can own I2S. */
void clip_player_release_pipe(void);

#endif
