#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include "searaboom.h"
#include "esp_err.h"

esp_err_t config_store_init(void);
esp_err_t config_store_load(sb_config_t *cfg);
esp_err_t config_store_save(const sb_config_t *cfg);
const char *config_store_stream_url(const sb_config_t *cfg);
esp_err_t config_store_set_play_updated(bool on);
bool config_store_take_play_updated(void);
/* True when firmware version changed since last successful boot (OTA or USB). */
bool config_store_consume_fw_change(const char *version);
bool config_store_has_wifi(const sb_config_t *cfg);
esp_err_t config_store_clear_wifi(void);

#endif
