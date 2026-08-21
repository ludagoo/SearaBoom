#ifndef VOLUME_BUTTONS_H
#define VOLUME_BUTTONS_H

#include <stdbool.h>
#include "esp_err.h"

typedef void (*volume_btn_cb_t)(int delta, void *ctx);
/* Dual-pad taps: 2 = station, 3 = log flag, 4 = wipe Wi-Fi. */
typedef void (*volume_gesture_cb_t)(int taps, void *ctx);

esp_err_t volume_buttons_init(volume_btn_cb_t cb, volume_gesture_cb_t gesture, void *ctx);
void volume_buttons_poll(void);

#endif
