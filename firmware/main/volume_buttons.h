#ifndef VOLUME_BUTTONS_H
#define VOLUME_BUTTONS_H

#include <stdbool.h>
#include "esp_err.h"

typedef void (*volume_btn_cb_t)(int delta, void *ctx);
/* Dual-pad taps: 2 = station, 3 = log flag, 4 = wipe Wi-Fi. */
typedef void (*volume_gesture_cb_t)(int taps, void *ctx);

esp_err_t volume_buttons_init(volume_btn_cb_t cb, volume_gesture_cb_t gesture, void *ctx);
void volume_buttons_poll(void);
bool volume_buttons_needs_cal(void);
/* First-boot / factory: + then −, one finger each. Mix must be up. */
esp_err_t volume_buttons_calibrate(void);
void volume_buttons_get_sens(float *up, float *dn);
esp_err_t volume_buttons_set_sens(float up, float dn);
void volume_buttons_dump_raw(int ms);
/* 4-chord: drop rev 4. Restore factory snapshot or the 0.25 boot default. */
void volume_buttons_reset_auto(void);
void volume_buttons_log_status(void);

#endif
