#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "searaboom.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "ota_update.h"
#include "led_status.h"
#include "log_shipper.h"
#include "clip_player.h"
#include "config_store.h"
#include "radio_player.h"
#include "sdkconfig.h"

static const char *TAG = "ota_update";

static void stop_radio_for_ota(void)
{
    if (!radio_player_is_running()) {
        return;
    }
    radio_player_request_stop();
    for (int i = 0; i < 100; i++) {
        if (!radio_player_is_running()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "forcing radio stop before OTA");
    radio_player_stop();
}

typedef struct {
    char *buf;
    int len;
    int size;
} resp_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && !esp_http_client_is_chunked_response(evt->client)) {
        if (rb->len + evt->data_len + 1 > rb->size) {
            return ESP_OK;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->buf[rb->len] = 0;
    }
    return ESP_OK;
}

static bool parse_version(const char *v, int *a, int *b, int *c)
{
    *a = *b = *c = 0;
    if (!v || !*v) {
        return false;
    }
    return sscanf(v, "%d.%d.%d", a, b, c) >= 1;
}

/* Full semver: latest > current */
static bool is_newer_full(const char *latest, const char *current)
{
    int l1, l2, l3, c1, c2, c3;
    parse_version(latest, &l1, &l2, &l3);
    parse_version(current, &c1, &c2, &c3);
    if (l1 != c1) {
        return l1 > c1;
    }
    if (l2 != c2) {
        return l2 > c2;
    }
    return l3 > c3;
}

/* Boot policy: only major.minor (ignore patch / 3rd digit) */
static bool is_newer_stable(const char *latest, const char *current)
{
    int l1, l2, l3, c1, c2, c3;
    parse_version(latest, &l1, &l2, &l3);
    parse_version(current, &c1, &c2, &c3);
    (void)l3;
    (void)c3;
    if (l1 != c1) {
        return l1 > c1;
    }
    return l2 > c2;
}

esp_err_t ota_update_check(ota_policy_t policy)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *policy_name = (policy == OTA_POLICY_DEV) ? "dev" : "stable";
    ESP_LOGI(TAG, "OTA check policy=%s current=%s", policy_name, app->version);

    /* Radio AAC + TLS on the same core trips the task WDT. Stop first. */
    stop_radio_for_ota();

    led_status_set(SB_LED_YELLOW, 250);
    log_shipper_set_paused(true);

    char check_url[256];
    snprintf(check_url, sizeof(check_url), "%s/api/firmware/check?current=%s",
             CONFIG_SEARABOOM_OTA_URL, app->version);

    char body[1024];
    resp_buf_t rb = {.buf = body, .len = 0, .size = sizeof(body)};
    body[0] = 0;

    esp_http_client_config_t cfg = {
        .url = check_url,
        .event_handler = http_event,
        .user_data = &rb,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "OTA check failed err=%s status=%d (continuing)", esp_err_to_name(err), status);
        led_status_set(SB_LED_MAGENTA, 200);
        vTaskDelay(pdMS_TO_TICKS(1500));
        log_shipper_set_paused(false);
        radio_player_resume();
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Bad OTA JSON");
        log_shipper_set_paused(false);
        radio_player_resume();
        return ESP_FAIL;
    }
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    const char *latest = cJSON_IsString(version) ? version->valuestring : NULL;
    const char *fw_url_in = cJSON_IsString(url) ? url->valuestring : NULL;

    bool need = false;
    if (latest && fw_url_in) {
        if (policy == OTA_POLICY_DEV) {
            need = is_newer_full(latest, app->version);
        } else {
            need = is_newer_stable(latest, app->version);
        }
    }

    if (!need) {
        if (latest && is_newer_full(latest, app->version) && policy == OTA_POLICY_STABLE) {
            ESP_LOGI(TAG, "Patch-only update %s -> %s ignored on boot (use serial 'ota')",
                     app->version, latest);
        } else {
            ESP_LOGI(TAG, "Firmware up to date (%s)", app->version);
        }
        cJSON_Delete(root);
        led_status_set(SB_LED_GREEN, 500);
        log_shipper_set_paused(false);
        radio_player_resume();
        return ESP_OK;
    }

    char fw_url[256];
    strncpy(fw_url, fw_url_in, sizeof(fw_url) - 1);
    fw_url[sizeof(fw_url) - 1] = 0;
    char new_ver[32] = {0};
    strncpy(new_ver, latest, sizeof(new_ver) - 1);
    cJSON_Delete(root);

    ESP_LOGW(TAG, "OTA update available -> %s (%s) policy=%s", new_ver, fw_url, policy_name);
    led_status_set(SB_LED_YELLOW, 100);
    clip_player_loop(SB_CLIP_OTA_UPDATING);
    if (clip_player_wait_started(12000) != ESP_OK) {
        ESP_LOGW(TAG, "updating clip did not start; continuing OTA anyway");
    }

    esp_http_client_config_t ota_http = {
        .url = fw_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &ota_http,
    };

    esp_err_t ota_err = esp_https_ota(&ota_config);
    clip_player_stop();
    if (ota_err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting into %s", new_ver);
        config_store_set_play_updated(true);
        led_status_set(SB_LED_GREEN, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ota_err));
    led_status_set(SB_LED_MAGENTA, 150);
    vTaskDelay(pdMS_TO_TICKS(2000));
    log_shipper_set_paused(false);
    radio_player_resume();
    return ota_err;
}

esp_err_t ota_update_check_on_boot(void)
{
    return ota_update_check(OTA_POLICY_STABLE);
}
