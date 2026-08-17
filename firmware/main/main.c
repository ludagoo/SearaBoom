#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "searaboom.h"
#include "config_store.h"
#include "led_status.h"
#include "volume_buttons.h"
#include "captive_portal.h"
#include "radio_player.h"
#include "clip_player.h"
#include "ota_update.h"
#include "log_shipper.h"
#include "serial_cmd.h"

static const char *TAG = "searaboom";
static sb_config_t s_cfg;
static EventGroupHandle_t s_wifi_events;
static volatile bool s_sta_retry = true;
#define WIFI_OK_BIT BIT0

static void volume_cb(int delta, void *ctx)
{
    (void)ctx;
    int v = radio_player_get_volume() + delta;
    if (v < SB_VOLUME_MIN) {
        v = SB_VOLUME_MIN;
    }
    if (v > SB_VOLUME_MAX) {
        v = SB_VOLUME_MAX;
    }
    radio_player_set_volume(v);
    s_cfg.volume = v;
    config_store_save(&s_cfg);
    radio_player_beep();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (!s_sta_retry) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_OK_BIT);
    }
}

void wifi_set_sta_retry(bool on)
{
    s_sta_retry = on;
    if (!on) {
        esp_wifi_disconnect();
    }
}

static bool wifi_connect_or_setup(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* SoftAP unicast (ARP/HTTP) dies with AMPDU + I2S; radio bitrate does not need it. */
    cfg.ampdu_tx_enable = 0;
    cfg.ampdu_rx_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    if (!config_store_has_wifi(&s_cfg)) {
        ESP_LOGW(TAG, "No WiFi SSID saved -> setup AP");
        s_sta_retry = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        return false;
    }

    wifi_config_t wifi = {0};
    strncpy((char *)wifi.sta.ssid, s_cfg.ssid, sizeof(wifi.sta.ssid));
    strncpy((char *)wifi.sta.password, s_cfg.password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());

    led_status_set(SB_LED_RED, 500);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_OK_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(CONFIG_SEARABOOM_WIFI_TIMEOUT_MS));
    return (bits & WIFI_OK_BIT) != 0;
}

static void ota_task_fn(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    ota_update_check_on_boot();
    if (waiter) {
        xTaskNotifyGive(waiter);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "SearaBoom ADF starting (fw %s)", app->version);
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(led_status_init());
    led_status_set(SB_LED_WHITE, 0);

    config_store_load(&s_cfg);
    serial_cmd_init();
    clip_player_init(s_cfg.volume);

    bool play_updated = config_store_take_play_updated();
    play_updated = config_store_consume_fw_change(app->version) || play_updated;
    if (play_updated && !config_store_has_wifi(&s_cfg)) {
        ESP_LOGI(TAG, "First boot after update, no Wi-Fi — atualizado then setup AP");
        clip_player_play_wait(SB_CLIP_OTA_DONE, 20000);
        /* Drop I2S/AAC before WiFi AP — leftover HOLD + decoder reset brownouts the 5V rail. */
        clip_player_release_pipe();
    }

    if (!wifi_connect_or_setup()) {
        ESP_LOGW(TAG, "Starting captive portal");
        captive_portal_run(); /* never returns */
    }

    log_shipper_init();

    const char *url = config_store_stream_url(&s_cfg);
    if (play_updated) {
        /* Prefetch HTTP while atualizado plays on the LC clip-only pipe. */
        ESP_LOGI(TAG, "Prefetch stream %s (%s) vol=%d while playing atualizado",
                 s_cfg.url_key, url, s_cfg.volume);
        radio_player_prefetch(url, s_cfg.volume);
        ESP_LOGI(TAG, "First boot after update — playing atualizado (LC clip pipe)");
        clip_player_play_wait(SB_CLIP_OTA_DONE, 20000);
        clip_player_release_pipe();
        if (radio_player_start(url, s_cfg.volume) != ESP_OK) {
            ESP_LOGE(TAG, "Radio start failed");
        }
    } else {
        ESP_LOGI(TAG, "WiFi OK, checking OTA (stable policy: major.minor only)");
        if (xTaskCreatePinnedToCore(ota_task_fn, "ota", 12288, xTaskGetCurrentTaskHandle(), 5, NULL, 0) == pdPASS) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(120000));
        } else {
            ota_update_check_on_boot();
        }
        ESP_LOGI(TAG, "Connecting to stream %s (%s) vol=%d", s_cfg.url_key, url, s_cfg.volume);
        if (radio_player_start(url, s_cfg.volume) != ESP_OK) {
            ESP_LOGE(TAG, "Radio start failed");
        }
    }
    led_status_set(SB_LED_GREEN, 500);

    /* Touch pads need RAM; wait until AAC has initialized. */
    bool touch_ready = false;
    int64_t started_ms = esp_timer_get_time() / 1000;

    while (true) {
        led_status_tick();
        radio_player_loop();
        if (!touch_ready) {
            int64_t now = esp_timer_get_time() / 1000;
            if (radio_player_has_music_info() || (now - started_ms) > 20000) {
                volume_buttons_init(volume_cb, NULL);
                touch_ready = true;
                ESP_LOGI(TAG, "Touch volume controls ready");
            }
        } else {
            volume_buttons_poll();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
