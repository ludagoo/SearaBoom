#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "searaboom.h"
#include "esp_err.h"

esp_err_t led_status_init(void);
void led_status_set(sb_led_color_t color, int blink_ms);
void led_status_tick(void);

#endif
