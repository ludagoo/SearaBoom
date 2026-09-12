#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"

typedef enum {
    /* Boot / periodic: any newer X.Y.Z. USB factory stays one publish behind. */
    OTA_POLICY_STABLE = 0,
    /* Serial `ota`: same rule, any newer X.Y.Z. */
    OTA_POLICY_DEV = 1,
} ota_policy_t;

esp_err_t ota_update_check(ota_policy_t policy);
esp_err_t ota_update_check_on_boot(void);

/* 12 KB internal-RAM worker. Flash writes abort if this stack is PSRAM. */
esp_err_t ota_update_start_task(void);
/* Wake the OTA worker (boot check or serial `ota`). */
void ota_update_kick(ota_policy_t policy);

/* True while an update is in progress so UI clips do not start. */
bool ota_update_is_busy(void);

/* Skip boot/periodic OTA after repeated crashes *during* a download.
 * Serial `ota` (DEV) still runs. Cleared after 120 s healthy. */
bool ota_update_skip_boot(void);
void ota_update_mark_healthy(void);

#endif
