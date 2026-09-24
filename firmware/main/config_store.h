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
 * 3 = factory-flasher `touch cal` / serial `touch sens` on this firmware.
 * 4 = field auto-cal from real presses. Older revs are ignored so field
 * OTA keeps the fixed SB_TOUCH_SENS graze fix from 0.5.20. Factory rev 3
 * is snapshotted in tsens_f_* so a 4-chord reset can drop rev 4.
 * Rev 4 also stores tsens_first (per-pad first-N done). One pad committing
 * must not retire the other pad on the next boot. */
#define SB_TOUCH_SENS_REV 3
#define SB_TOUCH_AUTO_REV 4
/* Per-pad first-N done bits stored with rev 4 (`tsens_first`). */
#define SB_TOUCH_AUTO_FIRST_UP 0x01u
#define SB_TOUCH_AUTO_FIRST_DN 0x02u
bool config_store_load_touch_sens(float *up, float *dn, uint8_t *rev);
bool config_store_load_touch_auto_first(uint8_t *mask);
esp_err_t config_store_save_touch_sens(float up, float dn);
esp_err_t config_store_save_touch_sens_auto(float up, float dn, uint8_t first_mask);
/* Running-image id (ELF SHA-256). False if this image has not been stored. */
bool config_store_load_touch_image(uint8_t *id, int len);
esp_err_t config_store_save_touch_image(const uint8_t *id, int len);
/* Per-pad seed samples for the stored image. n_* is 0..3. */
bool config_store_load_touch_seed(float *up, int *n_up, float *dn, int *n_dn);
esp_err_t config_store_save_touch_seed(const float *up, int n_up,
                                      const float *dn, int n_dn);
bool config_store_load_factory_touch_sens(float *up, float *dn);
esp_err_t config_store_ensure_factory_touch_snapshot(float up, float dn);
/* Restore factory snapshot as live rev 3, or clear live so boot uses SB_TOUCH_SENS. */
esp_err_t config_store_reset_auto_touch_sens(void);
/* USB factory rewrites SPIFFS, not NVS. First boot after that flash sees
 * /spiffs/usb_factory, drops tsens_* (Wi-Fi stays), then unlinks the marker. */
esp_err_t config_store_wipe_touch_if_usb_factory(void);

#endif
