#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "lwip/sockets.h"
#include "captive_portal.h"
#include "config_store.h"
#include "led_status.h"
#include "clip_player.h"
#include "esp_mac.h"

static const char *TAG = "captive_portal";
static httpd_handle_t s_server;
static char s_ssid_options[2048];
static volatile sb_clip_id_t s_want_clip = SB_CLIP_COUNT;
static volatile int64_t s_clip_at;
static volatile bool s_want_loop;
static bool s_page_said;
static bool s_need_page;
static volatile bool s_saving;
static int64_t s_sta_empty_at;
static sb_clip_id_t s_last_clip = SB_CLIP_COUNT;
static int64_t s_last_clip_at;
static sb_clip_id_t s_want_field = SB_CLIP_COUNT;

static bool ap_clip_is_field(sb_clip_id_t id)
{
    return id == SB_CLIP_AP_STATION || id == SB_CLIP_AP_WIFI
        || id == SB_CLIP_AP_PASSWORD || id == SB_CLIP_AP_SAVEBTN;
}

/* HTTP/Wi-Fi callbacks only set these. Playback runs on the portal loop. */
static void request_ap_clip(sb_clip_id_t id, int delay_ms, bool loop)
{
    int64_t now = esp_timer_get_time();
    if (s_saving && id != SB_CLIP_AP_SAVED) {
        return;
    }
    if (id == SB_CLIP_AP_WELCOME && (s_page_said || s_need_page)) {
        return;
    }
    if (id == SB_CLIP_AP_PAGE) {
        s_need_page = true;
        ESP_LOGI(TAG, "need page clip");
        return;
    }
    if (ap_clip_is_field(id)) {
        s_want_field = id;
        ESP_LOGI(TAG, "need field clip %s", clip_player_name(id));
        return;
    }
    if (id != SB_CLIP_AP_SAVED && id == s_last_clip
        && (now - s_last_clip_at) < 1500 * 1000LL) {
        return;
    }
    s_last_clip = id;
    s_last_clip_at = now;
    s_want_clip = id;
    s_want_loop = loop;
    s_clip_at = now + (int64_t)delay_ms * 1000;
}

static void ap_clip_tick(void)
{
    sb_clip_id_t cur = clip_player_playing();
    if (cur == SB_CLIP_AP_PAGE) {
        s_need_page = false;
        s_page_said = true;
    }

    if (s_saving) {
        return;
    }

    if (s_want_clip < SB_CLIP_COUNT && esp_timer_get_time() >= s_clip_at) {
        sb_clip_id_t id = s_want_clip;
        bool loop = s_want_loop;
        s_want_clip = SB_CLIP_COUNT;
        if (id == SB_CLIP_AP_CONNECTED) {
            wifi_sta_list_t list = {0};
            esp_wifi_ap_get_sta_list(&list);
            if (list.num == 0) {
                id = SB_CLIP_AP_WELCOME;
                loop = true;
            }
        }
        ESP_LOGI(TAG, "play queued %s", clip_player_name(id));
        clip_player_play(id, loop);
        return;
    }

    cur = clip_player_playing();
    /* Only block while that clip is actually outputting. Finished clips
     * must not keep the form silent. */
    if (cur == SB_CLIP_AP_CONNECTED || cur == SB_CLIP_AP_WELCOME) {
        return;
    }
    if (s_need_page && cur != SB_CLIP_AP_PAGE) {
        ESP_LOGI(TAG, "play page clip");
        clip_player_play(SB_CLIP_AP_PAGE, false);
        return;
    }
    if (cur == SB_CLIP_AP_PAGE) {
        return;
    }
    if (s_want_field < SB_CLIP_COUNT) {
        sb_clip_id_t id = s_want_field;
        s_want_field = SB_CLIP_COUNT;
        ESP_LOGI(TAG, "play field clip %s", clip_player_name(id));
        clip_player_play(id, false);
    }
}

static void ap_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "STA joined " MACSTR, MAC2STR(e->mac));
        s_sta_empty_at = 0;
        /* Swap only — do not pause I2S. Short delay so DHCP can start. */
        request_ap_clip(SB_CLIP_AP_CONNECTED, 50, false);
        /* Page clip waits for GET / so it speaks on the form, not in Wi-Fi settings. */
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_sta_list_t list = {0};
        esp_wifi_ap_get_sta_list(&list);
        ESP_LOGI(TAG, "STA left, remaining=%d", list.num);
        if (list.num == 0 && !s_saving) {
            s_sta_empty_at = esp_timer_get_time();
        }
    }
}

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
        "<!DOCTYPE html><html lang=\"pt-BR\"><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,"
        "minimum-scale=1,user-scalable=no,viewport-fit=cover\">"
        "<meta name=\"HandheldFriendly\" content=\"true\">"
        "<title>SearaBoom Configuração</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box;font-family:Arial,Helvetica,sans-serif;"
        "-webkit-text-size-adjust:100%;text-size-adjust:100%}"
        "html,body{width:100%;max-width:100%;overflow-x:hidden;background:#fff}"
        "article{width:100%;padding:8px}"
        "form{width:100%;max-width:360px;margin:0 auto;padding:8px;border-radius:.5em;"
        "background:rgba(0,0,0,.25)}"
        "h1{margin:0;text-align:center}"
        "h1 img{display:block;width:100%;max-width:240px;height:auto;margin:0 auto}"
        "fieldset{border:0;margin:8px 0;padding:10px;border-radius:.4em;background:rgba(0,0,0,.5)}"
        "legend{display:block;width:100%;color:#fff;padding:0 0 .4em;font-weight:600;text-align:center}"
        "label{display:block;color:#fff;margin:.5em 0 .3em}"
        "select,input,button{display:block;width:100%;font-size:16px;border:0;border-radius:.4em}"
        "select,input{height:2.4em;padding:.4em;background:rgba(0,0,0,.5);color:#fff}"
        "button{margin-top:8px;padding:.9em;background:#1ccc33;color:#fff;font-size:1.05em}"
        "</style>"
        "<link rel=\"stylesheet\" href=\"styles.css\"></head><body><article>"
        "<form action=\"/\" method=\"POST\"><h1>"
        "<img src=\"background.png\" width=\"240\" height=\"132\" alt=\"SearaBoom\"></h1>"
        "<iframe name=\"sbclip\" style=\"position:absolute;width:0;height:0;border:0\"></iframe>"
        "<img src=\"/clip/page\" width=\"1\" height=\"1\" alt=\"\">"
        "<fieldset><fieldset><legend><a href=\"/clip/station\" target=\"sbclip\">Estação</a></legend>"
        "<select name=\"url\" id=\"URL\" onclick=\"new Image().src='/clip/station?t='+Date.now()\" "
        "onchange=\"new Image().src='/clip/station?t='+Date.now()\">"
        "<option value=\"URL1\">Nova Russas (FM 102.7)</option>"
        "<option value=\"URL2\">Ibiapina (FM 104.7)</option>"
        "</select></fieldset>"
        "<fieldset><legend>Configurações do WiFi</legend>"
        "<label for=\"SSID\"><a href=\"/clip/wifi\" target=\"sbclip\">Nome:</a></label>"
        "<select name=\"ssid\" id=\"SSID\" onclick=\"new Image().src='/clip/wifi?t='+Date.now()\" "
        "onchange=\"new Image().src='/clip/wifi?t='+Date.now()\">";
    const char *html2 =
        "</select><label for=\"PASS\"><a href=\"/clip/pass\" target=\"sbclip\">Senha:</a></label>"
        "<input name=\"pass\" id=\"PASS\" type=\"text\" placeholder=\"Senha Do WiFi\""
        " autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\""
        " onclick=\"new Image().src='/clip/pass?t='+Date.now()\">"
        "</fieldset><button type=\"submit\" id=\"SAVE\""
        " onclick=\"new Image().src='/clip/save?t='+Date.now()\">Salvar</button></form>"
        "<script>"
        "setTimeout(function(){var a=document.activeElement; if(a&&a.blur) a.blur();},0);"
        "</script></article></body></html>";

    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, html1);
    httpd_resp_sendstr_chunk(req, s_ssid_options);
    httpd_resp_sendstr_chunk(req, html2);
    httpd_resp_sendstr_chunk(req, NULL);
    if (!s_saving && !s_page_said) {
        request_ap_clip(SB_CLIP_AP_PAGE, 0, false);
    }
    return ESP_OK;
}

static esp_err_t clip_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    sb_clip_id_t id = SB_CLIP_COUNT;
    if (strstr(req->uri, "station")) {
        id = SB_CLIP_AP_STATION;
    } else if (strstr(req->uri, "wifi")) {
        id = SB_CLIP_AP_WIFI;
    } else if (strstr(req->uri, "pass")) {
        id = SB_CLIP_AP_PASSWORD;
    } else if (strstr(req->uri, "save")) {
        /* Salvar audio is the POST saved clip, not this tap. */
        id = SB_CLIP_COUNT;
    } else if (strstr(req->uri, "page")) {
        if (s_page_said) {
            id = SB_CLIP_COUNT;
        } else {
            id = SB_CLIP_AP_PAGE;
        }
    }
    if (id < SB_CLIP_COUNT) {
        ESP_LOGI(TAG, "portal clip %s", clip_player_name(id));
        request_ap_clip(id, 0, false);
    }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
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

    s_saving = true;
    s_need_page = false;
    s_want_clip = SB_CLIP_COUNT;
    s_want_field = SB_CLIP_COUNT;
    config_store_save(&cfg);
    httpd_resp_send(req, "Done. Will restart", HTTPD_RESP_USE_STRLEN);
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
    wifi_set_sta_retry(false);

    /* WiFi stack already initialized by app_main */
    esp_wifi_disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "wifi_stop: %s", esp_err_to_name(stop_err));
        }
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == NULL) {
        esp_netif_create_default_wifi_ap();
    }

    build_ssid_options();
    esp_wifi_stop();

    /* APSTA: some phones fail open AP-only; STA stays idle (retry off). */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, SB_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(SB_AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.ssid_hidden = 0;
    ap.ap.beacon_interval = 100;
    ap.ap.pmf_cfg.capable = false;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* USB 5V brownouts when AP TX is at 20 dBm plus I2S. */
    esp_wifi_set_max_tx_power(52);

    /* wifi_start() resets the AP netif to 192.168.4.1 — apply 4.3.2.1 after it is up. */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 4, 3, 2, 1);
    IP4_ADDR(&ip_info.gw, 4, 3, 2, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    esp_netif_ip_info_t got = {0};
    esp_netif_get_ip_info(ap_netif, &got);
    ESP_LOGI(TAG, "AP netif " IPSTR, IP2STR(&got.ip));
    ESP_LOGI(TAG, "AP %s up at http://4.3.2.1/", SB_AP_SSID);
    clip_player_loop(SB_CLIP_AP_WELCOME);

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
    config.stack_size = 8192;
    config.task_priority = 7;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/", .method = HTTP_POST, .handler = root_post},
        {.uri = "/styles.css", .method = HTTP_GET, .handler = styles_get},
        {.uri = "/background.png", .method = HTTP_GET, .handler = bg_get},
        {.uri = "/clip/page", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/clip/station", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/clip/wifi", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/clip/pass", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/clip/save", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/clip/*", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/*", .method = HTTP_GET, .handler = captive_redirect},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    xTaskCreatePinnedToCore(dns_server_task, "dns", 4096, NULL, 3, NULL, 1);

    led_status_set(SB_LED_BLUE, 500);
    while (true) {
        if (s_saving) {
            clip_player_stop();
            clip_player_play_wait(SB_CLIP_AP_SAVED, 15000);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        if (s_sta_empty_at && !s_saving
            && (esp_timer_get_time() - s_sta_empty_at) >= 3000 * 1000LL) {
            wifi_sta_list_t list = {0};
            esp_wifi_ap_get_sta_list(&list);
            s_sta_empty_at = 0;
            if (list.num == 0) {
                s_page_said = false;
                s_need_page = false;
                s_want_field = SB_CLIP_COUNT;
                s_last_clip = SB_CLIP_COUNT;
                request_ap_clip(SB_CLIP_AP_WELCOME, 0, true);
            }
        }
        clip_player_tick();
        ap_clip_tick();
        led_status_tick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}