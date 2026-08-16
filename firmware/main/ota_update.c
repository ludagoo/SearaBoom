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
#include "sdkconfig.h"

static const char *TAG = "ota_update";

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

static int version_cmp(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) {
        return a1 - b1;
    }
    if (a2 != b2) {
        return a2 - b2;
    }
    return a3 - b3;
}

esp_err_t ota_update_check_on_boot(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Current firmware %s", app->version);

    led_status_set(SB_LED_YELLOW, 250);

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
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Bad OTA JSON");
        return ESP_FAIL;
    }
    cJSON *update = cJSON_GetObjectItem(root, "update");
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    bool need = cJSON_IsTrue(update);
    if (!need && cJSON_IsString(version)) {
        need = version_cmp(version->valuestring, app->version) > 0;
    }
    if (!need || !cJSON_IsString(url)) {
        ESP_LOGI(TAG, "Firmware up to date");
        cJSON_Delete(root);
        return ESP_OK;
    }

    char fw_url[256];
    strncpy(fw_url, url->valuestring, sizeof(fw_url) - 1);
    fw_url[sizeof(fw_url) - 1] = 0;
    char new_ver[32] = {0};
    if (cJSON_IsString(version)) {
        strncpy(new_ver, version->valuestring, sizeof(new_ver) - 1);
    }
    cJSON_Delete(root);

    ESP_LOGW(TAG, "OTA update available -> %s (%s)", new_ver, fw_url);
    led_status_set(SB_LED_YELLOW, 100);

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
    if (ota_err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting");
        led_status_set(SB_LED_GREEN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ota_err));
    led_status_set(SB_LED_MAGENTA, 150);
    vTaskDelay(pdMS_TO_TICKS(2000));
    return ota_err;
}