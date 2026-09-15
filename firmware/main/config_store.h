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
bool config_store_has_identity(const sb_config_t *cfg);
/* True only when the box has never been set up (empty ssid + no name/city + URL1). */
bool config_store_is_first_setup(const sb_config_t *cfg);
/* 4-tap / serial wifi wipe: empty ssid but still open SoftAP (no welcome). */
bool config_store_force_setup(void);
esp_err_t config_store_save_wifi(const sb_config_t *cfg);
esp_err_t config_store_clear_wifi(void);
/* Per-pad touch sensitivity (fraction of idle). False if never calibrated.
 * tsens_rev: 1 = 40% of peak (0.5.16), 2 = 80% of peak (0.5.18).
 * 3 = factory-flasher `touch cal` on this firmware. Older revs are ignored
 * so field OTA keeps the fixed SB_TOUCH_SENS graze fix from 0.5.20. */
#define SB_TOUCH_SENS_REV 3
bool config_store_load_touch_sens(float *up, float *dn, uint8_t *rev);
esp_err_t config_store_save_touch_sens(float up, float dn);
/* USB factory rewrites SPIFFS, not NVS. First boot after that flash sees
 * /spiffs/usb_factory, drops tsens_* (Wi-Fi stays), then unlinks the marker. */
esp_err_t config_store_wipe_touch_if_usb_factory(void);

#endif
