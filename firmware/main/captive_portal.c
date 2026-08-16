#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "lwip/sockets.h"
#include "captive_portal.h"
#include "config_store.h"
#include "led_status.h"

static const char *TAG = "captive_portal";
static httpd_handle_t s_server;
static char s_ssid_options[2048];

static void build_ssid_options(void)
{
    s_ssid_options[0] = '\0';
    wifi_scan_config_t scan = {0};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_start();
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        snprintf(s_ssid_options, sizeof(s_ssid_options),
                 "<option value=\"Nenhum Rede Encontrado\">Nenhum Rede Encontrado</option>");
        return;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        snprintf(s_ssid_options, sizeof(s_ssid_options),
                 "<option value=\"Nenhum Rede Encontrado\">Nenhum Rede Encontrado</option>");
        return;
    }
    if (ap_count > 20) {
        ap_count = 20;
    }
    wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!aps) {
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, aps));
    size_t used = 0;
    for (int i = 0; i < ap_count; ++i) {
        char opt[160];
        if (aps[i].rssi <= -85) {
            snprintf(opt, sizeof(opt), "<option value=\"%s\">%s (sinal fraco)</option>",
                     (char *)aps[i].ssid, (char *)aps[i].ssid);
        } else {
            snprintf(opt, sizeof(opt), "<option value=\"%s\">%s</option>",
                     (char *)aps[i].ssid, (char *)aps[i].ssid);
        }
        size_t n = strlen(opt);
        if (used + n + 1 >= sizeof(s_ssid_options)) {
            break;
        }
        memcpy(s_ssid_options + used, opt, n + 1);
        used += n;
    }
    free(aps);
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *type)
{
    char full[64];
    snprintf(full, sizeof(full), "/spiffs%s", path);
    FILE *f = fopen(full, "rb");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, type);
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    const char *html1 =
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"X-UA-Compatible\" content=\"ie=edge\">"
        "<title>SearaBoom Configuração</title>"
        "<link rel=\"stylesheet\" href=\"styles.css\"></head><body><article>"
        "<form action=\"/\" method=\"POST\"><h1><img src=\"background.png\"></h1>"
        "<fieldset><fieldset><legend>Estação</legend>"
        "<select name=\"url\" id=\"URL\">"
        "<option value=\"URL1\">Nova Russas (FM 102.7)</option>"
        "<option value=\"URL2\">Ibiapina (FM 104.7)</option>"
        "</select></fieldset>"
        "<fieldset><legend>Configurações do WiFi</legend>"
        "<label for=\"SSID\">Nome:</label>"
        "<select name=\"ssid\" id=\"SSID\">";
    const char *html2 =
        "</select><label for=\"PASS\">Senha:</label>"
        "<input name=\"pass\" id=\"PASS\" type=\"Text\" placeholder=\"Senha Do WiFi\">"
        "</fieldset><button type=\"submit\">Salvar</button></form></article></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, html1);
    httpd_resp_sendstr_chunk(req, s_ssid_options);
    httpd_resp_sendstr_chunk(req, html2);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void url_decode_inplace(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}

static esp_err_t root_post(httpd_req_t *req)
{
    char buf[512];
    int total = req->content_len;
    if (total >= (int)sizeof(buf)) {
        total = sizeof(buf) - 1;
    }
    int received = httpd_req_recv(req, buf, total);
    if (received <= 0) {
        return ESP_FAIL;
    }
    buf[received] = 0;

    sb_config_t cfg;
    config_store_load(&cfg);

    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, "&", &saveptr); tok; tok = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char *val = eq + 1;
        url_decode_inplace(val);
        if (strcmp(tok, "ssid") == 0) {
            strncpy(cfg.ssid, val, sizeof(cfg.ssid) - 1);
        } else if (strcmp(tok, "pass") == 0) {
            strncpy(cfg.password, val, sizeof(cfg.password) - 1);
        } else if (strcmp(tok, "url") == 0) {
            strncpy(cfg.url_key, val, sizeof(cfg.url_key) - 1);
        }
    }

    config_store_save(&cfg);
    httpd_resp_send(req, "Done. Will restart", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t styles_get(httpd_req_t *req) { return send_file(req, "/styles.css", "text/css"); }
static esp_err_t bg_get(httpd_req_t *req) { return send_file(req, "/background.png", "image/png"); }

static esp_err_t captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://4.3.2.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    uint8_t packet[512];
    while (true) {
        struct sockaddr_in source;
        socklen_t socklen = sizeof(source);
        int len = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *)&source, &socklen);
        if (len < 12) {
            continue;
        }
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        int i = 12;
        while (i < len && packet[i] != 0) {
            i += packet[i] + 1;
        }
        i += 5;
        if (i > len) {
            i = len;
        }
        uint8_t answer[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
            4, 3, 2, 1
        };
        if (i + (int)sizeof(answer) < (int)sizeof(packet)) {
            memcpy(packet + i, answer, sizeof(answer));
            sendto(sock, packet, i + sizeof(answer), 0, (struct sockaddr *)&source, socklen);
        }
    }
}

void captive_portal_run(void)
{
    ESP_LOGI(TAG, "Entering setup AP mode");
    led_status_set(SB_LED_WHITE, 0);

    /* WiFi stack already initialized by app_main */
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == NULL) {
        esp_netif_create_default_wifi_ap();
    }

    build_ssid_options();
    esp_wifi_stop();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 4, 3, 2, 1);
    IP4_ADDR(&ip_info.gw, 4, 3, 2, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, SB_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(SB_AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_vfs_spiffs_conf_t spiffs = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t spiffs_err = esp_vfs_spiffs_register(&spiffs);
    if (spiffs_err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(spiffs_err));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/", .method = HTTP_POST, .handler = root_post},
        {.uri = "/styles.css", .method = HTTP_GET, .handler = styles_get},
        {.uri = "/background.png", .method = HTTP_GET, .handler = bg_get},
        {.uri = "/*", .method = HTTP_GET, .handler = captive_redirect},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    xTaskCreate(dns_server_task, "dns", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "AP %s up at http://4.3.2.1/", SB_AP_SSID);

    while (true) {
        led_status_set(SB_LED_BLUE, 500);
        led_status_tick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}