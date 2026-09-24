#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "searaboom.h"
#include "serial_cmd.h"
#include "ota_update.h"
#include "config_store.h"
#include "clip_player.h"
#include "radio_player.h"
#include "radio_buf.h"
#include "log_shipper.h"
#include "listen_stats.h"
#include "volume_buttons.h"
#include "board.h"
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
        log_shipper_printf("logtest GET %s/healthz ...\n", CONFIG_SEARABOOM_LOG_URL);
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
        log_shipper_printf("logtest err=%s http=%d heap=%u\n", esp_err_to_name(err), status,
               (unsigned)esp_get_free_heap_size());
        esp_http_client_cleanup(client);

        log_shipper_printf("logtest GET https OTA host /healthz ...\n");
        snprintf(url, sizeof(url), "%s/healthz", CONFIG_SEARABOOM_OTA_URL);
        esp_http_client_config_t cfg2 = {
            .url = url,
            .timeout_ms = 8000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        client = esp_http_client_init(&cfg2);
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        log_shipper_printf("logtest-ota err=%s http=%d heap=%u\n", esp_err_to_name(err), status,
               (unsigned)esp_get_free_heap_size());
        esp_http_client_cleanup(client);
        return;
    }
    if (strcasecmp(line, "logstat") == 0) {
        log_shipper_logstat();
        return;
    }
    if (strcasecmp(line, "listen") == 0) {
        log_shipper_printf("listen_s=%u session_s=%u playing=%d\n",
               (unsigned)listen_stats_listen_s(),
               (unsigned)listen_stats_session_s(),
               listen_stats_playing() ? 1 : 0);
        return;
    }
    if (strncasecmp(line, "name", 4) == 0 && (line[4] == 0 || line[4] == ' ')) {
        sb_config_t cfg;
        config_store_load(&cfg);
        const char *arg = line + 4;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg) {
            strncpy(cfg.name, arg, sizeof(cfg.name) - 1);
            cfg.name[sizeof(cfg.name) - 1] = 0;
            config_store_save(&cfg);
        }
        log_shipper_printf("name=%s city=%s\n", cfg.name[0] ? cfg.name : "-", cfg.city[0] ? cfg.city : "-");
        return;
    }
    if (strncasecmp(line, "city", 4) == 0 && (line[4] == 0 || line[4] == ' ')) {
        sb_config_t cfg;
        config_store_load(&cfg);
        const char *arg = line + 4;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg) {
            strncpy(cfg.city, arg, sizeof(cfg.city) - 1);
            cfg.city[sizeof(cfg.city) - 1] = 0;
            config_store_save(&cfg);
        }
        log_shipper_printf("name=%s city=%s\n", cfg.name[0] ? cfg.name : "-", cfg.city[0] ? cfg.city : "-");
        return;
    }
    if (strcasecmp(line, "heap") == 0) {
        log_shipper_printf("heap free=%u min_free=%u\n",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size());
        log_shipper_printf("  internal free=%u largest=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        log_shipper_printf("  spiram   free=%u largest=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return;
    }
    if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        log_shipper_printf("commands: help | ver | board | ota | heap | logtest | logstat | listen | reboot | wifi wipe | wifi weak | http | vol [n] | name [x] | city [x] | clip <name>|stop | audiotest | touch | touch cal | touch raw | touch sens\n");
        log_shipper_printf("  ota        - force OTA now (applies any newer X.Y.Z including patch)\n");
        log_shipper_printf("  heap       - free internal / PSRAM (buffer headroom)\n");
        log_shipper_printf("  logtest    - probe log URL + OTA host connectivity\n");
        log_shipper_printf("  logstat    - log shipper ring/flash/seq/http\n");
        log_shipper_printf("  listen     - lifetime/session station PCM seconds and playing\n");
        log_shipper_printf("  name [x]   - show or set owner name (portal Nome)\n");
        log_shipper_printf("  city [x]   - show or set owner city (portal Cidade)\n");
        log_shipper_printf("  ver        - print firmware version\n");
        log_shipper_printf("  reboot     - restart\n");
        log_shipper_printf("  wifi wipe  - clear saved SSID/pass and reboot into setup AP\n");
        log_shipper_printf("  http       - station HTTP ringbuf + stall snapshot\n");
        log_shipper_printf("  vol [n|+|-] - live knob %d-%d (vol + is the pad path)\n",
               SB_VOLUME_MIN, SB_VOLUME_MAX);
        log_shipper_printf("  clip name  - play a UI clip (welcome|connected|page|...|stop)\n");
        log_shipper_printf("  audiotest  - PCM start/end markers + AAC clip duration at the mixer tap\n");
        log_shipper_printf("  touch      - show per-pad sensitivity\n");
        log_shipper_printf("  touch cal  - factory: hold + , then hold -\n");
        log_shipper_printf("  touch raw  - 8s live pad readings (press each)\n");
        log_shipper_printf("  touch sens a b - save vol+ / vol- (e.g. 0.22 0.22)\n");
        return;
    }
    if (strcasecmp(line, "touch") == 0) {
        float up = 0;
        float dn = 0;
        volume_buttons_get_sens(&up, &dn);
        log_shipper_printf("touch sens vol+=%.3f vol-=%.3f need_cal=%d\n",
               up, dn, (int)volume_buttons_needs_cal());
        volume_buttons_log_status();
        return;
    }
    if (strcasecmp(line, "touch raw") == 0) {
        volume_buttons_dump_raw(8000);
        return;
    }
    if (strncasecmp(line, "touch sens", 10) == 0) {
        float up = 0;
        float dn = 0;
        if (sscanf(line + 10, "%f %f", &up, &dn) != 2) {
            log_shipper_printf("usage: touch sens <up> <dn>   e.g. touch sens 0.22 0.22\n");
            return;
        }
        esp_err_t err = volume_buttons_set_sens(up, dn);
        float a = 0;
        float b = 0;
        volume_buttons_get_sens(&a, &b);
        log_shipper_printf("touch sens vol+=%.3f vol-=%.3f %s\n", a, b, esp_err_to_name(err));
        return;
    }
    if (strcasecmp(line, "touch cal") == 0) {
        log_shipper_printf("touch cal: hold + then -\n");
        radio_player_start_idle(radio_player_get_volume());
        esp_err_t err = volume_buttons_calibrate();
        log_shipper_printf("touch cal %s\n", esp_err_to_name(err));
        return;
    }
    if (strcasecmp(line, "board") == 0) {
        board_hw_dump();
        return;
    }
    if (strcasecmp(line, "ver") == 0 || strcasecmp(line, "version") == 0) {
        const esp_app_desc_t *app = esp_app_get_description();
        log_shipper_printf("version=%s kconfig=%s board=%s\n", app->version,
               CONFIG_SEARABOOM_FW_VERSION, board_hw_name());
        return;
    }
    if (strcasecmp(line, "reboot") == 0 || strcasecmp(line, "reset") == 0) {
        log_shipper_printf("rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }
    if (strcasecmp(line, "wifi wipe") == 0 || strcasecmp(line, "wifiwipe") == 0) {
        log_shipper_printf("wiping WiFi, rebooting into setup AP...\n");
        config_store_clear_wifi();
        wifi_forget_driver_config();
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        return;
    }
    if (strcasecmp(line, "wifi weak") == 0) {
        radio_player_on_rssi_low(-90);
        log_shipper_printf("wifi weak latched (holds stream + prompt until HTTP rb >= %dk)\n",
                           SB_WIFI_HTTP_RESUME_BYTES / 1024);
        return;
    }
    if (strcasecmp(line, "http") == 0) {
        log_shipper_printf("http rb=%d hold=%d music=%d prebuf=%d peak=%d\n",
               radio_player_http_buffered(),
               radio_player_wifi_weak_holding() ? 1 : 0,
               radio_player_has_music_info() ? 1 : 0,
               radio_player_is_prebuffering() ? 1 : 0,
               radio_player_pcm_tap_peak());
        radio_player_log_health("serial");
        return;
    }
    if (strncasecmp(line, "vol", 3) == 0 && (line[3] == 0 || line[3] == ' ')) {
        sb_config_t cfg;
        config_store_load(&cfg);
        const char *arg = line + 3;
        while (*arg == ' ') {
            arg++;
        }
        int live = radio_player_get_volume();
        if (arg[0] == '+' || arg[0] == '-') {
            int delta = (arg[0] == '+') ? 1 : -1;
            int cur = live;
            int v = radio_player_nudge_volume(delta);
            if (v == cur) {
                radio_player_beep_limit();
                log_shipper_printf("volume=%d alc=%d dB nvs=%d limit max=%d\n",
                       v, radio_player_alc_db(v), cfg.volume, SB_VOLUME_MAX);
                return;
            }
            clip_player_set_volume(v);
            cfg.volume = v;
            config_store_save(&cfg);
            radio_player_beep();
            log_shipper_printf("volume=%d alc=%d dB nvs=%d (pad path)\n",
                   v, radio_player_alc_db(v), v);
            return;
        }
        if (*arg) {
            int v = atoi(arg);
            if (v < SB_VOLUME_MIN) {
                v = SB_VOLUME_MIN;
            }
            if (v > SB_VOLUME_MAX) {
                v = SB_VOLUME_MAX;
            }
            cfg.volume = v;
            config_store_save(&cfg);
            radio_player_set_volume(v);
            clip_player_set_volume(v);
            log_shipper_printf("volume=%d alc=%d dB nvs=%d\n", v,
                   radio_player_alc_db(v), v);
        } else {
            /* Live player, not NVS — a saved 21 is not the knob. */
            log_shipper_printf("volume=%d alc=%d dB nvs=%d max=%d\n", live,
                   radio_player_alc_db(live), cfg.volume, SB_VOLUME_MAX);
        }
        return;
    }
    if (strncasecmp(line, "clip", 4) == 0 && (line[4] == 0 || line[4] == ' ')) {
        const char *arg = line + 4;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg == 0 || strcasecmp(arg, "stop") == 0) {
            clip_player_stop();
            log_shipper_printf("clip stop\n");
            return;
        }
        sb_clip_id_t id = SB_CLIP_COUNT;
        for (int i = 0; i < SB_CLIP_COUNT; i++) {
            const char *nm = clip_player_name((sb_clip_id_t)i);
            if (strcasecmp(arg, nm) == 0) {
                id = (sb_clip_id_t)i;
                break;
            }
            if (strncmp(nm, "ap_", 3) == 0 && strcasecmp(arg, nm + 3) == 0) {
                id = (sb_clip_id_t)i;
                break;
            }
            if (strcmp(nm, "wifi_weak") == 0 && strcasecmp(arg, "weak") == 0) {
                id = (sb_clip_id_t)i;
                break;
            }
            if (strcmp(nm, "net_slow") == 0
                && (strcasecmp(arg, "slow") == 0 || strcasecmp(arg, "internet") == 0)) {
                id = (sb_clip_id_t)i;
                break;
            }
        }
        if (id == SB_CLIP_COUNT) {
            log_shipper_printf("unknown clip '%s'\n", arg);
            return;
        }
        bool loop = (id == SB_CLIP_AP_WELCOME || id == SB_CLIP_OTA_AVAILABLE);
        if (radio_player_start_idle(radio_player_get_volume()) != ESP_OK) {
            log_shipper_printf("audio out failed\n");
            return;
        }
        clip_player_play(id, loop);
        log_shipper_printf("clip %s loop=%d\n", clip_player_name(id), (int)loop);
        return;
    }
    if (strcasecmp(line, "audiotest") == 0) {
        log_shipper_printf("audiotest start\n");
        radio_probe_result_t pcm = {0};
        esp_err_t err = clip_player_run_pcm_probe(&pcm, 4000);
        log_shipper_printf("AUDIOTEST pcm start=%d end=%d dur_ms=%d peak=%d start_hz=%d end_hz=%d err=%s\n",
               pcm.start_ok, pcm.end_ok, pcm.dur_ms, pcm.peak, pcm.start_hz, pcm.end_hz,
               esp_err_to_name(err));
        int heard = 0;
        int expected = clip_player_duration_ms(SB_CLIP_OTA_REBOOTING);
        err = clip_player_run_clip_probe(SB_CLIP_OTA_REBOOTING, &heard, expected + 4000);
        int ok = (heard >= expected - 250) && (heard <= expected + 1500);
        log_shipper_printf("AUDIOTEST clip name=%s expected_ms=%d heard_ms=%d ok=%d err=%s\n",
               clip_player_name(SB_CLIP_OTA_REBOOTING), expected, heard, ok,
               esp_err_to_name(err));
        log_shipper_printf("audiotest done\n");
        return;
    }
    if (strcasecmp(line, "ota") == 0 || strcasecmp(line, "ota force") == 0) {
        if (ota_update_is_busy()) {
            log_shipper_printf("OTA already running\n");
            return;
        }
        log_shipper_printf("OTA force check queued\n");
        ESP_LOGW(TAG, "Serial-triggered OTA (dev policy)");
        ota_update_kick(OTA_POLICY_DEV);
        return;
    }

    log_shipper_printf("unknown cmd '%s' (try help)\n", line);
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
    log_shipper_printf("\nSearaBoom console — type 'help'\n");

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
    /* Internal: logtest TLS and NVS saves disable the flash cache. */
    if (xTaskCreatePinnedToCore(serial_task, "serial_cmd", 8192, NULL, 5, NULL, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
