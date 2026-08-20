#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "log_shipper.h"
#include "sdkconfig.h"

static const char *TAG = "log_shipper";

#define LOG_RING_BYTES 8192
#define LOG_FLUSH_MS 8000
#define LOG_POST_MAX 1536
#define LOG_FAIL_BACKOFF_MS 45000
#define LOG_START_DELAY_MS 25000

static char s_ring[LOG_RING_BYTES];
static size_t s_ring_len;
static SemaphoreHandle_t s_mu;
static vprintf_like_t s_prev_vprintf;
static volatile bool s_paused;
static char s_device_id[18];
static bool s_ready;
static char s_chunk[LOG_POST_MAX];
static char s_payload[LOG_POST_MAX + 192];

static void ring_append(const char *data, size_t n)
{
    if (!s_mu || n == 0) {
        return;
    }
    if (xSemaphoreTake(s_mu, 0) != pdTRUE) {
        return;
    }
    if (n >= LOG_RING_BYTES) {
        data += (n - (LOG_RING_BYTES - 1));
        n = LOG_RING_BYTES - 1;
    }
    while (s_ring_len + n >= LOG_RING_BYTES) {
        char *nl = memchr(s_ring, '\n', s_ring_len);
        if (!nl) {
            s_ring_len = 0;
            break;
        }
        size_t drop = (size_t)(nl - s_ring) + 1;
        memmove(s_ring, s_ring + drop, s_ring_len - drop);
        s_ring_len -= drop;
    }
    memcpy(s_ring + s_ring_len, data, n);
    s_ring_len += n;
    xSemaphoreGive(s_mu);
}

static int shipper_vprintf(const char *fmt, va_list args)
{
    char line[160];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0 && !s_paused && s_ready) {
        size_t use = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        ring_append(line, use);
    }
    if (s_prev_vprintf) {
        return s_prev_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

static bool take_chunk(size_t *out_len)
{
    *out_len = 0;
    if (!s_mu) {
        return false;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (s_ring_len == 0) {
        xSemaphoreGive(s_mu);
        return false;
    }
    size_t n = s_ring_len;
    if (n > sizeof(s_chunk) - 1) {
        n = sizeof(s_chunk) - 1;
        for (size_t i = n; i > 0; --i) {
            if (s_ring[i - 1] == '\n') {
                n = i;
                break;
            }
        }
    }
    memcpy(s_chunk, s_ring, n);
    s_chunk[n] = 0;
    memmove(s_ring, s_ring + n, s_ring_len - n);
    s_ring_len -= n;
    xSemaphoreGive(s_mu);
    *out_len = n;
    return n > 0;
}

static bool ship_chunk(void)
{
    int off = snprintf(s_payload, sizeof(s_payload),
                       "{\"device_id\":\"%s\",\"ts_ms\":%lld,\"text\":",
                       s_device_id, (long long)(esp_timer_get_time() / 1000));
    if (off < 0 || off >= (int)sizeof(s_payload) - 4) {
        return false;
    }
    s_payload[off++] = '"';
    for (const char *p = s_chunk; *p && off < (int)sizeof(s_payload) - 4; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') {
            s_payload[off++] = '\\';
            s_payload[off++] = c;
        } else if (c == '\n') {
            s_payload[off++] = '\\';
            s_payload[off++] = 'n';
        } else if (c == '\r' || (unsigned char)c < 0x20) {
            continue;
        } else {
            s_payload[off++] = c;
        }
    }
    s_payload[off++] = '"';
    s_payload[off++] = '}';
    s_payload[off] = 0;

    char url[192];
    snprintf(url, sizeof(url), "%s/api/logs", CONFIG_SEARABOOM_LOG_URL);

    bool use_tls = (strncmp(url, "https://", 8) == 0);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        .buffer_size = 512,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "SearaBoom-LogShipper/1");
    esp_http_client_set_post_field(client, s_payload, strlen(s_payload));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool ok = (err == ESP_OK && status >= 200 && status < 300);
    if (!ok) {
        printf("log_ship fail err=%s http=%d url=%s\n", esp_err_to_name(err), status, CONFIG_SEARABOOM_LOG_URL);
    }
    esp_http_client_cleanup(client);
    return ok;
}

static void shipper_task(void *arg)
{
    (void)arg;
    size_t n;
    /* Let AAC/stream + TLS settle before competing for another TLS session */
    vTaskDelay(pdMS_TO_TICKS(LOG_START_DELAY_MS));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LOG_FLUSH_MS));
        if (s_paused || !s_ready) {
            continue;
        }
        if (!take_chunk(&n)) {
            continue;
        }
        if (!ship_chunk()) {
            vTaskDelay(pdMS_TO_TICKS(LOG_FAIL_BACKOFF_MS));
        }
    }
}

esp_err_t log_shipper_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("searaboom", ESP_LOG_DEBUG);
    esp_log_level_set("ota_update", ESP_LOG_DEBUG);
    esp_log_level_set("radio_player", ESP_LOG_DEBUG);
    esp_log_level_set("serial_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("log_shipper", ESP_LOG_INFO);
    esp_log_level_set("volume_buttons", ESP_LOG_DEBUG);
    esp_log_level_set("config_store", ESP_LOG_DEBUG);
    esp_log_level_set("captive_portal", ESP_LOG_DEBUG);

    s_prev_vprintf = esp_log_set_vprintf(shipper_vprintf);
    s_ready = true;
    s_paused = false;

    if (xTaskCreatePinnedToCore(shipper_task, "log_ship", 12288, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "log shipper task failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Remote logs -> %s/api/logs as %s", CONFIG_SEARABOOM_LOG_URL, s_device_id);
    {
        char hello[80];
        int n = snprintf(hello, sizeof(hello), "log_shipper online device=%s\n", s_device_id);
        if (n > 0) {
            ring_append(hello, (size_t)n);
        }
    }
    return ESP_OK;
}

void log_shipper_set_paused(bool paused)
{
    s_paused = paused;
}

bool log_shipper_is_paused(void)
{
    return s_paused;
}
