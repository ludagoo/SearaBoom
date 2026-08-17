#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "serial_cmd.h"
#include "ota_update.h"
#include "config_store.h"
#include "sdkconfig.h"

static const char *TAG = "serial_cmd";

static void trim_line(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = 0;
    }
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static void handle_line(char *line)
{
    trim_line(line);
    if (line[0] == 0) {
        return;
    }

    if (strcasecmp(line, "logtest") == 0) {
        printf("logtest GET %s/healthz ...\n", CONFIG_SEARABOOM_LOG_URL);
        char url[192];
        snprintf(url, sizeof(url), "%s/healthz", CONFIG_SEARABOOM_LOG_URL);
        bool use_tls = (strncmp(url, "https://", 8) == 0);
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 5000,
            .crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        printf("logtest err=%s http=%d heap=%u\n", esp_err_to_name(err), status,
               (unsigned)esp_get_free_heap_size());
        esp_http_client_cleanup(client);

        printf("logtest GET https OTA host /healthz ...\n");
        snprintf(url, sizeof(url), "%s/healthz", CONFIG_SEARABOOM_OTA_URL);
        esp_http_client_config_t cfg2 = {
            .url = url,
            .timeout_ms = 8000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        client = esp_http_client_init(&cfg2);
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        printf("logtest-ota err=%s http=%d heap=%u\n", esp_err_to_name(err), status,
               (unsigned)esp_get_free_heap_size());
        esp_http_client_cleanup(client);
        return;
    }
    if (strcasecmp(line, "heap") == 0) {
        printf("heap free=%u min_free=%u\n",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size());
        printf("  internal free=%u largest=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        printf("  spiram   free=%u largest=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return;
    }
    if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        printf("commands: help | ver | ota | heap | logtest | reboot | wifi wipe\n");
        printf("  ota        - force OTA now (applies any newer X.Y.Z including patch)\n");
        printf("  heap       - free internal / PSRAM (buffer headroom)\n");
        printf("  logtest    - probe log URL + OTA host connectivity\n");
        printf("  ver        - print firmware version\n");
        printf("  reboot     - restart\n");
        printf("  wifi wipe  - clear saved SSID/pass and reboot into setup AP\n");
        return;
    }
    if (strcasecmp(line, "ver") == 0 || strcasecmp(line, "version") == 0) {
        const esp_app_desc_t *app = esp_app_get_description();
        printf("version=%s kconfig=%s\n", app->version, CONFIG_SEARABOOM_FW_VERSION);
        return;
    }
    if (strcasecmp(line, "reboot") == 0 || strcasecmp(line, "reset") == 0) {
        printf("rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }
    if (strcasecmp(line, "wifi wipe") == 0 || strcasecmp(line, "wifiwipe") == 0) {
        printf("wiping WiFi, rebooting into setup AP...\n");
        config_store_clear_wifi();
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        return;
    }
    if (strcasecmp(line, "ota") == 0 || strcasecmp(line, "ota force") == 0) {
        printf("OTA force check...\n");
        ESP_LOGW(TAG, "Serial-triggered OTA (dev policy)");
        /* Run in this task — stack is large enough */
        ota_update_check(OTA_POLICY_DEV);
        return;
    }

    printf("unknown cmd '%s' (try help)\n", line);
}

static void serial_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t len = 0;

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    /* May already be installed by console secondary — ignore ALREADY_DONE-style errors */
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install: %s (continuing)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Serial commands ready (type 'help')");
    printf("\nSearaBoom console — type 'help'\n");

    while (true) {
        uint8_t ch;
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            line[len] = 0;
            handle_line(line);
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        } else {
            len = 0; /* overflow — reset */
        }
    }
}

esp_err_t serial_cmd_init(void)
{
    if (xTaskCreatePinnedToCore(serial_task, "serial_cmd", 12288, NULL, 5, NULL, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
