#ifndef SEARABOOM_COMMON_H
#define SEARABOOM_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#define SB_URL1 "https://searaboom.goossen.dev/stream/102"
#define SB_URL2 "https://searaboom.goossen.dev/stream/104"
#define SB_AP_SSID "SearaBoom"
#define SB_VOLUME_MIN 1
#define SB_VOLUME_MAX 21
#define SB_DEFAULT_VOLUME SB_VOLUME_MAX
#define SB_DEBOUNCE_MS 250

void wifi_set_sta_retry(bool on);

typedef enum {
    SB_LED_OFF = 0,
    SB_LED_WHITE,
    SB_LED_RED,      /* WiFi connecting */
    SB_LED_GREEN,    /* Playing */
    SB_LED_BLUE,     /* Setup AP */
    SB_LED_YELLOW,   /* OTA in progress */
    SB_LED_MAGENTA,  /* OTA failed */
} sb_led_color_t;

typedef struct {
    char ssid[33];
    char password[65];
    char url_key[8]; /* "URL1" or "URL2" */
    int volume;
} sb_config_t;

#endif
