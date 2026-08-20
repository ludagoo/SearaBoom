#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include "searaboom.h"
#include "esp_err.h"

esp_err_t config_store_init(void);
esp_err_t config_store_load(sb_config_t *cfg);
esp_err_t config_store_save(const sb_config_t *cfg);
/* Apply ALC now; NVS volume is written ~1s after the last change. */
esp_err_t config_store_save_volume_deferred(int volume);
void config_store_flush_deferred(void);
void config_store_flush_deferred_now(void);
/* Copy a pending pad volume into cfg so a full save does not revert it. */
void config_store_absorb_deferred_volume(sb_config_t *cfg);
const char *config_store_stream_url(const sb_config_t *cfg);
esp_err_t config_store_set_play_updated(bool on);
bool config_store_take_play_updated(void);
/* True when firmware version changed since last successful boot (OTA or USB). */
bool config_store_consume_fw_change(const char *version);
bool config_store_has_wifi(const sb_config_t *cfg);
esp_err_t config_store_clear_wifi(void);

#endif
