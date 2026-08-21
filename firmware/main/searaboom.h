#ifndef SEARABOOM_COMMON_H
#define SEARABOOM_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Brasilstream is ADTS; they label it audio/aac. http_stream_event
 * forces codec UNKNOWN and Icy-MetaData: 0 so ADF will decode it.
 * Canonical URLs stay https://; radio_player tries http:// first. */
#define SB_URL1 "https://8396.brasilstream.com.br/stream"
#define SB_URL2 "https://8404.brasilstream.com.br/stream"
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
    char name[41];   /* owner name from setup form */
    char city[41];   /* owner city from setup form */
    int volume;
} sb_config_t;

bool wifi_sta_got_ip(void);
esp_err_t wifi_sta_join(const sb_config_t *cfg);

#endif
