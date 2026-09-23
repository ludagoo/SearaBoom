#include "listen_stats.h"

#include "clip_player.h"
#include "nvs.h"
#include "ota_update.h"
#include "radio_player.h"
#include "searaboom.h"

#include "esp_timer.h"

static const char *NVS_NS = "searaboom";
#define NVS_KEY "listen_s"
#define PERSIST_EVERY_S 60u

static bool s_inited;
static bool s_playing;
static uint32_t s_listen_s;
static uint32_t s_session_s;
static uint32_t s_acc_ms;
static uint32_t s_since_persist_s;
static int64_t s_mark_us;

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, NVS_KEY, s_listen_s);
    nvs_commit(h);
    nvs_close(h);
    s_since_persist_s = 0;
}

static void ensure_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;
    uint32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY, &v);
        nvs_close(h);
    }
    s_listen_s = v;
}

static bool station_pcm_now(void)
{
    return radio_player_is_running()
           && radio_player_has_music_info()
           && radio_player_pcm_flowing()
           && radio_player_pcm_has_energy()
           && !clip_player_is_active()
           && !radio_player_wifi_weak_holding()
           && wifi_sta_got_ip()
           && !ota_update_is_busy();
}

void listen_stats_poll(void)
{
    ensure_init();
    bool playing = station_pcm_now();
    int64_t now = esp_timer_get_time();
    if (playing && s_playing && s_mark_us > 0) {
        int64_t dt_us = now - s_mark_us;
        if (dt_us > 0 && dt_us < 2000000) {
            s_acc_ms += (uint32_t)(dt_us / 1000);
            while (s_acc_ms >= 1000) {
                s_acc_ms -= 1000;
                s_listen_s++;
                s_session_s++;
                s_since_persist_s++;
            }
        }
    }
    if (playing) {
        s_mark_us = now;
    } else {
        s_mark_us = 0;
        if (s_playing) {
            persist();
        }
    }
    s_playing = playing;
    if (s_since_persist_s >= PERSIST_EVERY_S) {
        persist();
    }
}

void listen_stats_persist(void)
{
    ensure_init();
    persist();
}

bool listen_stats_playing(void)
{
    return s_playing;
}

uint32_t listen_stats_listen_s(void)
{
    ensure_init();
    return s_listen_s;
}

uint32_t listen_stats_session_s(void)
{
    return s_session_s;
}
