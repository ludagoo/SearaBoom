#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include "searaboom.h"
#include "esp_err.h"

esp_err_t config_store_init(void);
esp_err_t config_store_load(sb_config_t *cfg);
esp_err_t config_store_save(const sb_config_t *cfg);
const char *config_store_stream_url(const sb_config_t *cfg);

#endif
