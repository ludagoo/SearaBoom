#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "searaboom.h"
#include "board.h"
#include "config_store.h"
#include "wifi_boot.h"
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
static volatile bool s_sta_got_ip;
static volatile bool s_wifi_need_reconnect;
static volatile int s_wifi_disc_reason;
static volatile int s_wifi_fails;
static volatile bool s_sta_did_assoc;
static volatile bool s_ap_seen;
static volatile int64_t s_wifi_retry_at_ms;
static esp_timer_handle_t s_wifi_retry_timer;
static bool s_healthy_marked;
#define WIFI_OK_BIT BIT0
#define WIFI_BACKOFF_MAX_MS 30000
#define WIFI_HEALTHY_MS 120000

static volatile int s_pad_taps;
static unsigned s_flag_seq;

static void volume_cb(int delta, void *ctx)
{
    (void)ctx;
    /* Live player step, not NVS. Serial `vol` used to show the saved
     * value, so a knob already at 24 looked "stuck" at 21. */
    int cur = radio_player_get_volume();
    int v = radio_player_nudge_volume(delta);
    if (v == cur) {
        ESP_LOGI(TAG, "pad vol limit live=%d max=%d alc=%d dB",
                 cur, SB_VOLUME_MAX, radio_player_alc_db(cur));
        radio_player_beep_limit();
        return;
    }
    clip_player_set_volume(v);
    s_cfg.volume = v;
    config_store_save_volume_deferred(v);
    ESP_LOGI(TAG, "pad vol %d -> %d alc=%d dB", cur, v, radio_player_alc_db(v));
    radio_player_beep();
}

static void pad_gesture_cb(int taps, void *ctx)
{
    (void)ctx;
    s_pad_taps = taps;
}

static void handle_pad_gesture(int taps)
{
    if (taps == 2) {
        config_store_load(&s_cfg);
        config_store_absorb_deferred_volume(&s_cfg);
        bool was2 = strncmp(s_cfg.url_key, "URL2", 4) == 0;
        strncpy(s_cfg.url_key, was2 ? "URL1" : "URL2", sizeof(s_cfg.url_key) - 1);
        s_cfg.url_key[sizeof(s_cfg.url_key) - 1] = 0;
        config_store_save(&s_cfg);
        const char *url = config_store_stream_url(&s_cfg);
        sb_clip_id_t tune = was2 ? SB_CLIP_TUNE_102 : SB_CLIP_TUNE_104;
        ESP_LOGI(TAG, "Station switch -> %s %s", s_cfg.url_key, url);
        /* Same as boot: prefetch while the mixer plays the ident, then go
         * live. Starting the stream live during the clip fills the radio
         * PCM rb with nobody reading it; HTTP/AAC stall and never recover. */
        radio_player_stop();
        if (radio_player_prefetch(url, radio_player_get_volume()) != ESP_OK) {
            ESP_LOGE(TAG, "Station switch prefetch failed");
        }
        clip_player_play_wait(tune, 15000);
        if (radio_player_go_live() != ESP_OK) {
            ESP_LOGE(TAG, "Station switch go_live failed");
        }
        clip_player_release_idle();
        return;
    }
    if (taps == 3) {
        s_flag_seq++;
        ESP_LOGW(TAG, "USER_FLAG seq=%u uptime_ms=%lld", s_flag_seq,
                 (long long)(esp_timer_get_time() / 1000));
        log_shipper_flush();
        radio_player_beep();
        return;
    }
    if (taps >= 4) {
        ESP_LOGW(TAG, "USER_WIFI_WIPE");
        log_shipper_flush();
        led_status_set(SB_LED_BLUE, 80);
        radio_player_beep_limit();
        config_store_clear_wifi();
        wifi_forget_driver_config();
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    }
}

static void wifi_retry_timer_cb(void *arg)
{
    (void)arg;
    s_wifi_retry_at_ms = 0;
    wifi_reconnect_tick();
}

static void wifi_retry_timer_init(void)
{
    if (s_wifi_retry_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = wifi_retry_timer_cb,
        .name = "wifi_retry",
    };
    if (esp_timer_create(&args, &s_wifi_retry_timer) != ESP_OK) {
        ESP_LOGW(TAG, "wifi retry timer create failed");
        s_wifi_retry_timer = NULL;
    }
}

static void wifi_retry_timer_stop(void)
{
    if (s_wifi_retry_timer) {
        esp_timer_stop(s_wifi_retry_timer);
    }
}

static void wifi_retry_timer_arm(int backoff_ms)
{
    if (!s_wifi_retry_timer) {
        return;
    }
    if (backoff_ms < 1) {
        backoff_ms = 1;
    }
    esp_timer_stop(s_wifi_retry_timer);
    esp_timer_start_once(s_wifi_retry_timer, (uint64_t)backoff_ms * 1000);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = data;
        int reason = disc ? (int)disc->reason : 0;
        s_sta_got_ip = false;
        s_wifi_disc_reason = reason;
        /* Anything but NO_AP_FOUND (201) means the AP was there (auth/DHCP/assoc). */
        if (reason != WIFI_REASON_NO_AP_FOUND) {
            s_ap_seen = true;
        }
        if (s_sta_retry) {
            radio_player_on_sta_lost();
        }
        log_shipper_wifi_down();
        if (!s_sta_retry) {
            wifi_retry_timer_stop();
            return;
        }
        s_wifi_fails++;
        int shift = s_wifi_fails - 1;
        if (shift > 5) {
            shift = 5;
        }
        int backoff_ms = 1000 << shift;
        if (backoff_ms > WIFI_BACKOFF_MAX_MS) {
            backoff_ms = WIFI_BACKOFF_MAX_MS;
        }
        s_wifi_retry_at_ms = (esp_timer_get_time() / 1000) + backoff_ms;
        s_wifi_need_reconnect = true;
        wifi_retry_timer_arm(backoff_ms);
        ESP_LOGW(TAG, "WiFi disconnected reason=%d fail=%d backoff_ms=%d",
                 reason, s_wifi_fails, backoff_ms);
        return;
    }
    if (!s_sta_retry) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_did_assoc = true;
        s_ap_seen = true;
        radio_player_arm_rssi_threshold();
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi <= -78) {
            radio_player_on_rssi_low(ap.rssi);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_BSS_RSSI_LOW) {
        int rssi = 0;
        if (data) {
            rssi = ((wifi_event_bss_rssi_low_t *)data)->rssi;
        }
        radio_player_on_rssi_low(rssi);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR " (after %d fail, reason=%d)",
                 IP2STR(&event->ip_info.ip), s_wifi_fails, s_wifi_disc_reason);
        s_sta_got_ip = true;
        s_wifi_fails = 0;
        s_wifi_need_reconnect = false;
        wifi_retry_timer_stop();
        radio_player_on_sta_got_ip();
        log_shipper_wifi_up();
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_OK_BIT);
        }
    }
}

bool wifi_sta_got_ip(void)
{
    return s_sta_got_ip;
}

void wifi_reconnect_tick(void)
{
    if (!s_wifi_need_reconnect || !s_sta_retry || s_sta_got_ip) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_wifi_retry_at_ms) {
        return;
    }
    s_wifi_need_reconnect = false;
    ESP_LOGI(TAG, "WiFi reconnect try fail=%d reason=%d", s_wifi_fails,
             s_wifi_disc_reason);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "WiFi connect: %s", esp_err_to_name(err));
        s_wifi_retry_at_ms = now + 2000;
        s_wifi_need_reconnect = true;
        wifi_retry_timer_arm(2000);
    }
}

esp_err_t wifi_sta_join(const sb_config_t *cfg)
{
    wifi_config_t wifi = {0};
    strncpy((char *)wifi.sta.ssid, cfg->ssid, sizeof(wifi.sta.ssid));
    strncpy((char *)wifi.sta.password, cfg->password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    s_sta_got_ip = false;
    s_sta_retry = true;
    s_sta_did_assoc = false;
    s_ap_seen = false;
    s_wifi_fails = 0;
    s_wifi_need_reconnect = false;
    wifi_retry_timer_init();
    wifi_retry_timer_stop();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    return esp_wifi_connect();
}

void wifi_set_sta_retry(bool on)
{
    s_sta_retry = on;
    if (!on) {
        s_wifi_need_reconnect = false;
        wifi_retry_timer_stop();
        esp_wifi_disconnect();
    }
}

void wifi_forget_driver_config(void)
{
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi restore: %s", esp_err_to_name(err));
    }
}

typedef enum {
    SB_WIFI_OK = 0,
    SB_WIFI_NEED_SETUP,
    SB_WIFI_NO_IP,
} sb_wifi_result_t;

static void wifi_start_sta_idle(void)
{
    s_sta_retry = false;
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wifi_recover_ssid_from_driver(sb_config_t *cfg)
{
    wifi_config_t w = {0};
    if (!cfg || esp_wifi_get_config(WIFI_IF_STA, &w) != ESP_OK) {
        return false;
    }
    if (!w.sta.ssid[0]) {
        return false;
    }
    memset(cfg->ssid, 0, sizeof(cfg->ssid));
    memcpy(cfg->ssid, w.sta.ssid,
           sizeof(w.sta.ssid) < sizeof(cfg->ssid) - 1 ? sizeof(w.sta.ssid)
                                                      : sizeof(cfg->ssid) - 1);
    memset(cfg->password, 0, sizeof(cfg->password));
    memcpy(cfg->password, w.sta.password,
           sizeof(w.sta.password) < sizeof(cfg->password) - 1
               ? sizeof(w.sta.password)
               : sizeof(cfg->password) - 1);
    if (!cfg->ssid[0]) {
        return false;
    }
    ESP_LOGW(TAG, "Restored ssid=%s from WiFi driver NVS", cfg->ssid);
    (void)config_store_save_wifi(cfg);
    return true;
}

static bool wifi_sta_is_associated(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

/* Retry off + disconnect, then wait until not associated. Scan-while-connecting
 * must not run (and must not be treated as "gone"). */
static bool wifi_idle_sta_for_scan(void)
{
    wifi_set_sta_retry(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    int64_t deadline = (esp_timer_get_time() / 1000) + 2000;
    while (wifi_sta_is_associated()) {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
        if ((esp_timer_get_time() / 1000) >= deadline) {
            ESP_LOGW(TAG, "STA still associated before probe scan — stay STA");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

/* Directed probe of the saved SSID. Broadcast + "first 20 APs" is not "gone".
 * Fail (start / ap_num / still connecting) → UNKNOWN, not GONE. */
static sb_wifi_air_t wifi_probe_saved_ssid(const char *want)
{
    if (!want || !want[0]) {
        return SB_WIFI_AIR_GONE;
    }
    uint8_t ssid_buf[33] = {0};
    strncpy((char *)ssid_buf, want, sizeof(ssid_buf) - 1);
    wifi_scan_config_t scan = {0};
    scan.ssid = ssid_buf;
    scan.show_hidden = true;
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        ESP_LOGW(TAG, "home SSID probe scan failed — stay STA");
        return SB_WIFI_AIR_UNKNOWN;
    }
    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK) {
        ESP_LOGW(TAG, "home SSID probe ap_num failed — stay STA");
        return SB_WIFI_AIR_UNKNOWN;
    }
    if (n == 0) {
        ESP_LOGI(TAG, "probe scan: saved ssid=%s not on air", want);
        return SB_WIFI_AIR_GONE;
    }
    ESP_LOGI(TAG, "probe scan saw saved ssid=%s aps=%u", want, (unsigned)n);
    return SB_WIFI_AIR_SEEN;
}

static sb_wifi_air_t wifi_saved_network_on_air(void)
{
    if (s_sta_did_assoc || s_ap_seen) {
        ESP_LOGI(TAG, "saved ssid seen assoc=%d ap_seen=%d reason=%d",
                 (int)s_sta_did_assoc, (int)s_ap_seen, s_wifi_disc_reason);
        return SB_WIFI_AIR_SEEN;
    }
    if (!wifi_idle_sta_for_scan()) {
        return SB_WIFI_AIR_UNKNOWN;
    }
    return wifi_probe_saved_ssid(s_cfg.ssid);
}

static sb_wifi_result_t wifi_sta_wait_for_ip(void)
{
    led_status_set(SB_LED_RED, 500);
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + CONFIG_SEARABOOM_WIFI_TIMEOUT_MS;
    EventBits_t bits = 0;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
        wifi_reconnect_tick();
        bits = xEventGroupWaitBits(s_wifi_events, WIFI_OK_BIT, pdFALSE, pdTRUE,
                                   pdMS_TO_TICKS(1000));
        if (bits & WIFI_OK_BIT) {
            return SB_WIFI_OK;
        }
    }
    return SB_WIFI_NO_IP;
}

static sb_wifi_result_t wifi_connect_or_setup(bool *play_welcome)
{
    if (play_welcome) {
        *play_welcome = false;
    }
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
    wifi_retry_timer_init();

    bool force_ap = config_store_force_setup();
    /* Mode before get_config so driver NVS is visible for ssid recover. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (!force_ap && !config_store_has_wifi(&s_cfg)) {
        wifi_recover_ssid_from_driver(&s_cfg);
    }

    if (force_ap || !config_store_has_wifi(&s_cfg)) {
        if (force_ap) {
            ESP_LOGW(TAG, "WiFi wipe -> setup AP");
        } else {
            ESP_LOGW(TAG, "No WiFi SSID saved -> setup AP");
        }
        if (play_welcome) {
            *play_welcome = true;
        }
        wifi_start_sta_idle();
        return SB_WIFI_NEED_SETUP;
    }

    wifi_config_t wifi = {0};
    strncpy((char *)wifi.sta.ssid, s_cfg.ssid, sizeof(wifi.sta.ssid));
    strncpy((char *)wifi.sta.password, s_cfg.password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    s_sta_did_assoc = false;
    s_ap_seen = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 20 dBm + 240 MHz + I2S amp sags USB 5V. 17 dBm is enough for STA. */
    esp_wifi_set_max_tx_power(68);

    if (wifi_sta_wait_for_ip() == SB_WIFI_OK) {
        return SB_WIFI_OK;
    }

    sb_wifi_air_t air = wifi_saved_network_on_air();
    bool welcome = sb_wifi_should_play_welcome(s_cfg.ssid, false, air);
    if (play_welcome) {
        *play_welcome = welcome;
    }
    if (!sb_wifi_should_start_portal(s_cfg.ssid, false, air)) {
        ESP_LOGW(TAG, "WiFi join timeout ssid=%s fails=%d air=%d — STA retry, no setup AP",
                 s_cfg.ssid, s_wifi_fails, (int)air);
        s_sta_retry = true;
        s_wifi_need_reconnect = true;
        wifi_retry_timer_arm(1000);
        return SB_WIFI_NO_IP;
    }
    ESP_LOGW(TAG, "saved ssid not on air -> setup AP");
    return SB_WIFI_NEED_SETUP;
}

static void boot_start_radio(bool play_updated)
{
    if (radio_player_is_running()) {
        return;
    }
    const char *url = config_store_stream_url(&s_cfg);
    sb_clip_id_t tune = (strncmp(s_cfg.url_key, "URL2", 4) == 0)
        ? SB_CLIP_TUNE_104 : SB_CLIP_TUNE_102;
    ESP_LOGI(TAG, "Prefetch stream %s (%s) vol=%d", s_cfg.url_key, url, s_cfg.volume);
    if (radio_player_prefetch(url, s_cfg.volume) != ESP_OK) {
        ESP_LOGE(TAG, "Radio prefetch failed");
    }
    if (play_updated) {
        ESP_LOGI(TAG, "First boot after update — playing atualizado");
        clip_player_play_wait(SB_CLIP_OTA_DONE, 20000);
    } else {
        clip_player_play_wait(tune, 15000);
    }
    if (radio_player_go_live() != ESP_OK) {
        ESP_LOGE(TAG, "Radio go_live failed");
    }
    clip_player_release_idle();
}

void app_main(void)
{
    /* sdkconfig on this host has DEBUG; mix/nvs/wifi spam starves I2S and HTTP. */
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("DOWNMIX", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_INFO);
    esp_log_level_set("nvs", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_INFO);

    log_shipper_init();
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "task wdt add failed");
    } else {
        ESP_LOGI(TAG, "task wdt subscribed timeout=%ds panic=1 crashes=%u",
                 CONFIG_ESP_TASK_WDT_TIMEOUT_S,
                 (unsigned)log_shipper_crash_count());
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "SearaBoom ADF starting (fw %s)", app->version);
    ESP_ERROR_CHECK(config_store_init());
    board_hw_detect();
    /* SPIFFS is already mounted by log_shipper_init(). USB factory rewrites
     * storage.bin (marker present) but not NVS — drop pad cal only. */
    config_store_wipe_touch_if_usb_factory();
    ESP_ERROR_CHECK(led_status_init());
    led_status_set(SB_LED_WHITE, 0);

    config_store_load(&s_cfg);
    serial_cmd_init();
    if (ota_update_start_task() != ESP_OK) {
        ESP_LOGE(TAG, "OTA task create failed at boot");
    }
    clip_player_init(s_cfg.volume);
    /* Touch element FSM before I2S/AAC. Starting pads at go-live used to buzz. */
    volume_buttons_init(volume_cb, pad_gesture_cb, NULL);

    bool play_updated = config_store_take_play_updated();
    play_updated = config_store_consume_fw_change(app->version) || play_updated;
    if (play_updated && config_store_is_first_setup(&s_cfg)) {
        ESP_LOGI(TAG, "First boot after update, no Wi-Fi — atualizado then setup AP");
        radio_player_start_idle(s_cfg.volume);
        clip_player_play_wait(SB_CLIP_OTA_DONE, 20000);
    }

    bool play_welcome = false;
    bool wait_for_sta = false;
    sb_wifi_result_t wifi_boot = wifi_connect_or_setup(&play_welcome);
    if (wifi_boot == SB_WIFI_NEED_SETUP) {
        ESP_LOGW(TAG, "Starting captive portal welcome=%d", (int)play_welcome);
        captive_portal_run(play_welcome); /* returns after save when the phone leaves the AP */
        config_store_load(&s_cfg);
        s_pad_taps = 0;
        /* Portal used to start the station before AP teardown. If that
         * pipeline is wedged/silent, do not skip the boot ident. */
        if (!radio_player_has_music_info()) {
            radio_player_stop();
        }
    } else if (wifi_boot == SB_WIFI_NO_IP) {
        /* Saved SSID was on the air (or associated) but DHCP/auth missed
         * the boot window. Keep STA retry. No welcome. */
        wait_for_sta = true;
        ESP_LOGW(TAG, "Configured WiFi not joined — quiet STA retry, no welcome");
    }

    if (!wait_for_sta) {
        boot_start_radio(play_updated);
        led_status_set(SB_LED_OFF, 0);
    }

    /* Version check after the stream is up. If an update is found the OTA
     * task stops radio, speaks, then downloads with audio off. */
    int64_t live_at_ms = esp_timer_get_time() / 1000;
    bool ota_started = false;

    while (true) {
        wifi_reconnect_tick();
        if (wait_for_sta && wifi_sta_got_ip() && !radio_player_is_running()) {
            wait_for_sta = false;
            boot_start_radio(play_updated);
            led_status_set(SB_LED_OFF, 0);
            live_at_ms = esp_timer_get_time() / 1000;
        }
        if (!s_healthy_marked && (esp_timer_get_time() / 1000) >= WIFI_HEALTHY_MS) {
            log_shipper_mark_healthy();
            ota_update_mark_healthy();
            s_healthy_marked = true;
        }
        led_status_tick();
        clip_player_tick();
        volume_buttons_poll();
        if (radio_player_take_stop_clip()) {
            clip_player_stop();
        }
        if (s_pad_taps) {
            int taps = s_pad_taps;
            s_pad_taps = 0;
            handle_pad_gesture(taps);
        }
        if (radio_player_wifi_weak_resume_ready()) {
            clip_player_stop();
            radio_player_hold_stream(false);
        } else if (!ota_update_is_busy() && !clip_player_is_active()) {
            if (radio_player_wifi_weak_should_speak()) {
                /* Speak first so the warning is not lost in a mute gap, then
                 * stop consuming radio PCM so HTTP can refill. Skip during OTA
                 * so the download does not fight a UI clip for Wi-Fi/CPU. */
                clip_player_loop(SB_CLIP_WIFI_WEAK);
                radio_player_hold_stream(true);
            } else if (radio_player_http_slow_should_speak()) {
                /* Hold first: only ~8 s of AAC left. Speaking over the
                 * station would finish the ring during the prompt. */
                radio_player_hold_stream(true);
                clip_player_loop(SB_CLIP_NET_SLOW);
            }
        } else if (!radio_player_wifi_weak_holding()
                   && (clip_player_playing() == SB_CLIP_WIFI_WEAK
                       || clip_player_playing() == SB_CLIP_NET_SLOW)) {
            /* Prompt clips only run during hold. teardown_radio() clears
             * hold without stopping the clip; without this the loop
             * keeps saying "internet lenta" over a healthy station. */
            ESP_LOGI(TAG, "stop %s clip - not holding",
                     clip_player_name(clip_player_playing()));
            clip_player_stop();
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (!ota_started && (now - live_at_ms) > 15000 && radio_player_has_music_info()) {
            if (ota_update_skip_boot()) {
                ESP_LOGW(TAG, "OTA in-flight crash loop — skipping boot OTA");
            } else {
                ESP_LOGI(TAG, "WiFi OK, checking OTA after stream");
                ota_update_kick(OTA_POLICY_STABLE);
            }
            ota_started = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
