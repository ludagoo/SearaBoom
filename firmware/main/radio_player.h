#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include "esp_err.h"
#include "searaboom.h"

esp_err_t radio_player_start(const char *url, int volume);
void radio_player_stop(void);
void radio_player_loop(void);
esp_err_t radio_player_set_volume(int volume);
int radio_player_get_volume(void);
bool radio_player_has_music_info(void);

#endif
