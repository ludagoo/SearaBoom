#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"

/* Check server for newer firmware. Shows LED feedback while running. */
esp_err_t ota_update_check_on_boot(void);

#endif
