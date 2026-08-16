#ifndef VOLUME_BUTTONS_H
#define VOLUME_BUTTONS_H

#include <stdbool.h>
#include "esp_err.h"

typedef void (*volume_btn_cb_t)(int delta, void *ctx);

esp_err_t volume_buttons_init(volume_btn_cb_t cb, void *ctx);
void volume_buttons_poll(void);

#endif
