#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include "esp_err.h"
#include "searaboom.h"

esp_err_t radio_player_start(const char *url, int volume);
/* Start HTTP only (HOLD inject) so the 256 KB ringbuffer fills. Clip-only I2S can run. */
esp_err_t radio_player_prefetch(const char *url, int volume);
bool radio_player_is_prefetching(void);
void radio_player_stop(void);
void radio_player_request_stop(void);
void radio_player_resume(void);
bool radio_player_is_running(void);
void radio_player_loop(void);
esp_err_t radio_player_set_volume(int volume);
int radio_player_get_volume(void);
bool radio_player_has_music_info(void);
/* Short confirmation tone mixed into the live stream (touch feedback). */
void radio_player_beep(void);

#endif
