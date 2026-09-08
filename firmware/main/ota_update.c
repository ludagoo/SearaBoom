#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
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

static volatile bool s_busy;
static TaskHandle_t s_task;
static volatile ota_policy_t s_kick_policy = OTA_POLICY_STABLE;

bool ota_update_is_busy(void)
{
    return s_busy;
}

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

static void announce_update_found(void)
{
    stop_radio_for_ota();
    radio_player_start_idle(radio_player_get_volume());
    if (clip_player_play_wait(SB_CLIP_OTA_AVAILABLE, 15000) != ESP_OK) {
        ESP_LOGW(TAG, "available clip did not finish; downloading anyway");
    }
    clip_player_stop();
    clip_player_release_idle();
}

static esp_err_t download_firmware(const char *fw_url)
{
    ESP_LOGI(TAG, "OTA download (audio off, full speed)");
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
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
    esp_err_t err = esp_https_ota(&ota_config);
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
    return err;
}

static void announce_reboot(const char *new_ver)
{
    ESP_LOGI(TAG, "OTA success, announcing reboot into %s", new_ver);
    config_store_flush_deferred_now();
    config_store_set_play_updated(true);
    led_status_set(SB_LED_GREEN, 0);
    radio_player_start_idle(radio_player_get_volume());
    if (clip_player_play_wait(SB_CLIP_OTA_REBOOTING, 10000) != ESP_OK) {
        ESP_LOGW(TAG, "rebooting clip did not finish; restarting anyway");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
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

/* Boot policy used to ignore patch. USB factory is one publish behind OTA,
 * so first Wi-Fi after a flash must install that update — including 0.0.63 -> 0.1.0
 * (minor) and later patch bumps once factory firmware includes this check. */
static bool is_newer_stable(const char *latest, const char *current)
{
    return is_newer_full(latest, current);
}

esp_err_t ota_update_check(ota_policy_t policy)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *policy_name = (policy == OTA_POLICY_DEV) ? "dev" : "stable";
    ESP_LOGI(TAG, "OTA check policy=%s current=%s", policy_name, app->version);

    /* Version JSON is a small GET. Stopping the radio here made boot go
     * live, then silent, then start the stream again. Only pause the
     * station if we are actually going to download firmware. Mark busy so
     * the log shipper does not open a second TLS session during this GET. */
    s_busy = true;
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
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
    esp_err_t err = esp_http_client_perform(client);
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "OTA check failed err=%s status=%d (continuing)", esp_err_to_name(err), status);
        led_status_set(SB_LED_MAGENTA, 200);
        vTaskDelay(pdMS_TO_TICKS(1500));
        led_status_set(SB_LED_OFF, 0);
        s_busy = false;
        log_shipper_flush();
        radio_player_resume();
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Bad OTA JSON");
        led_status_set(SB_LED_OFF, 0);
        s_busy = false;
        log_shipper_flush();
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
        ESP_LOGI(TAG, "Firmware up to date (%s)", app->version);
        cJSON_Delete(root);
        led_status_set(SB_LED_OFF, 0);
        s_busy = false;
        log_shipper_flush();
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
    s_busy = false;
    log_shipper_flush();
    announce_update_found();
    log_shipper_set_paused(true);
    s_busy = true;
    esp_err_t ota_err = download_firmware(fw_url);
    log_shipper_set_paused(false);
    s_busy = false;
    if (ota_err == ESP_OK) {
        announce_reboot(new_ver);
    }
    clip_player_stop();

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ota_err));
    led_status_set(SB_LED_MAGENTA, 150);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_status_set(SB_LED_OFF, 0);
    log_shipper_flush();
    radio_player_resume();
    return ota_err;
}

esp_err_t ota_update_check_on_boot(void)
{
    return ota_update_check(OTA_POLICY_STABLE);
}

static void ota_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ota_policy_t pol = s_kick_policy;
        if (pol == OTA_POLICY_DEV) {
            ota_update_check(OTA_POLICY_DEV);
        } else {
            ota_update_check_on_boot();
        }
    }
}

esp_err_t ota_update_start_task(void)
{
    if (s_task) {
        return ESP_OK;
    }
    /* Allocate at boot while internal heap still has a 12 KB hole. */
    if (xTaskCreatePinnedToCore(ota_task_fn, "ota", 12288, NULL, 5, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "OTA task create failed at boot");
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ota_update_kick(ota_policy_t policy)
{
    s_kick_policy = policy;
    if (!s_task) {
        ESP_LOGW(TAG, "OTA task missing — cannot kick");
        return;
    }
    xTaskNotifyGive(s_task);
}
