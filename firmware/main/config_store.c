#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config_store.h"
#include "listen_stats.h"
#include "sdkconfig.h"

static const char *TAG = "config_store";
static const char *NVS_NS = "searaboom";
#define VOL_DEFER_MS 1000

static int s_def_vol;
static bool s_def_dirty;
static int64_t s_def_at_ms;

static void sanitize_ident(char *s, size_t cap)
{
    if (!s || cap == 0) {
        return;
    }
    char tmp[41];
    size_t o = 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    for (; *p && o + 1 < sizeof(tmp) && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '<' || c == '>') {
            continue;
        }
        tmp[o++] = (char)c;
    }
    while (o > 0 && tmp[o - 1] == ' ') {
        o--;
    }
    tmp[o] = 0;
    memcpy(s, tmp, o + 1);
}

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_store_load(sb_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->ssid, CONFIG_SEARABOOM_WIFI_SSID, sizeof(cfg->ssid) - 1);
    strncpy(cfg->password, CONFIG_SEARABOOM_WIFI_PASSWORD, sizeof(cfg->password) - 1);
    strncpy(cfg->url_key, "URL1", sizeof(cfg->url_key) - 1);
    cfg->name[0] = 0;
    cfg->city[0] = 0;
    cfg->volume = SB_DEFAULT_VOLUME;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved config, using defaults (vol=%d)", SB_DEFAULT_VOLUME);
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "volume", SB_DEFAULT_VOLUME);
            nvs_set_i32(h, "vol_curve", 2);
            nvs_commit(h);
            nvs_close(h);
        }
        return ESP_OK;
    }

    size_t len = sizeof(cfg->ssid);
    if (nvs_get_str(h, "ssid", cfg->ssid, &len) != ESP_OK) {
        strncpy(cfg->ssid, CONFIG_SEARABOOM_WIFI_SSID, sizeof(cfg->ssid) - 1);
    }
    len = sizeof(cfg->password);
    if (nvs_get_str(h, "pass", cfg->password, &len) != ESP_OK) {
        strncpy(cfg->password, CONFIG_SEARABOOM_WIFI_PASSWORD, sizeof(cfg->password) - 1);
    }
    len = sizeof(cfg->url_key);
    if (nvs_get_str(h, "url", cfg->url_key, &len) != ESP_OK) {
        strncpy(cfg->url_key, "URL1", sizeof(cfg->url_key) - 1);
    }
    len = sizeof(cfg->name);
    if (nvs_get_str(h, "name", cfg->name, &len) != ESP_OK) {
        cfg->name[0] = 0;
    }
    sanitize_ident(cfg->name, sizeof(cfg->name));
    len = sizeof(cfg->city);
    if (nvs_get_str(h, "city", cfg->city, &len) != ESP_OK) {
        cfg->city[0] = 0;
    }
    sanitize_ident(cfg->city, sizeof(cfg->city));
    int32_t vol = SB_DEFAULT_VOLUME;
    bool had_volume = (nvs_get_i32(h, "volume", &vol) == ESP_OK);
    if (had_volume) {
        cfg->volume = (int)vol;
    }
    int32_t vol_curve = 0;
    nvs_get_i32(h, "vol_curve", &vol_curve);
    nvs_close(h);

    if (cfg->volume < SB_VOLUME_MIN) {
        cfg->volume = SB_VOLUME_MIN;
    }
    if (cfg->volume > SB_VOLUME_MAX) {
        cfg->volume = SB_VOLUME_MAX;
    }

    /*
     * Curve 1: 1→-36dB … 21→+9dB (distorted near the top).
     * Curve 2: 1→-36dB … 21→+2dB (clean max measured at old step 18).
     * Remap so perceived loudness is preserved.
     */
    bool dirty = false;
    if (vol_curve < 2) {
        int old = cfg->volume;
        int old_alc = -36 + ((old - 1) * 45) / (SB_VOLUME_MAX - 1);
        if (old_alc > 2) {
            old_alc = 2;
        }
        cfg->volume = 1 + ((old_alc + 36) * (SB_VOLUME_MAX - 1) + 19) / 38;
        if (cfg->volume < SB_VOLUME_MIN) {
            cfg->volume = SB_VOLUME_MIN;
        }
        if (cfg->volume > SB_VOLUME_MAX) {
            cfg->volume = SB_VOLUME_MAX;
        }
        ESP_LOGI(TAG, "Migrated volume curve v1→v2: step %d → %d", old, cfg->volume);
        dirty = true;
        vol_curve = 2;
    }

    /* Seed default (max) or migrated volume into NVS so it survives reboot/OTA. */
    if (!had_volume || dirty) {
        nvs_handle_t hw;
        if (nvs_open(NVS_NS, NVS_READWRITE, &hw) == ESP_OK) {
            nvs_set_i32(hw, "volume", cfg->volume);
            nvs_set_i32(hw, "vol_curve", vol_curve < 2 ? 2 : vol_curve);
            nvs_commit(hw);
            nvs_close(hw);
        }
    }

    ESP_LOGI(TAG, "Loaded ssid=%s url=%s vol=%d name=%s city=%s",
             cfg->ssid, cfg->url_key, cfg->volume,
             cfg->name[0] ? cfg->name : "-", cfg->city[0] ? cfg->city : "-");
    return ESP_OK;
}

static int clamp_volume(int volume)
{
    if (volume < SB_VOLUME_MIN) {
        return SB_VOLUME_MIN;
    }
    if (volume > SB_VOLUME_MAX) {
        return SB_VOLUME_MAX;
    }
    return volume;
}

static esp_err_t write_volume_now(int volume)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "volume nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(h, "volume", volume);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved volume=%d", volume);
    } else {
        ESP_LOGE(TAG, "volume save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_save_volume_deferred(int volume)
{
    s_def_vol = clamp_volume(volume);
    s_def_dirty = true;
    s_def_at_ms = esp_timer_get_time() / 1000 + VOL_DEFER_MS;
    return ESP_OK;
}

void config_store_flush_deferred(void)
{
    if (!s_def_dirty) {
        return;
    }
    if ((esp_timer_get_time() / 1000) < s_def_at_ms) {
        return;
    }
    config_store_flush_deferred_now();
}

void config_store_flush_deferred_now(void)
{
    if (!s_def_dirty) {
        return;
    }
    s_def_dirty = false;
    write_volume_now(s_def_vol);
}

void config_store_absorb_deferred_volume(sb_config_t *cfg)
{
    if (!cfg || !s_def_dirty) {
        return;
    }
    cfg->volume = s_def_vol;
}

esp_err_t config_store_save(const sb_config_t *cfg)
{
    s_def_dirty = false;
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", cfg->ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", cfg->password));
    ESP_ERROR_CHECK(nvs_set_str(h, "url", cfg->url_key));
    {
        char name[sizeof(cfg->name)];
        char city[sizeof(cfg->city)];
        strncpy(name, cfg->name, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        strncpy(city, cfg->city, sizeof(city) - 1);
        city[sizeof(city) - 1] = 0;
        sanitize_ident(name, sizeof(name));
        sanitize_ident(city, sizeof(city));
        ESP_ERROR_CHECK(nvs_set_str(h, "name", name));
        ESP_ERROR_CHECK(nvs_set_str(h, "city", city));
    }
    ESP_ERROR_CHECK(nvs_set_i32(h, "volume", cfg->volume));
    /* Keep curve marker so boot does not remigrate a user-chosen level. */
    ESP_ERROR_CHECK(nvs_set_i32(h, "vol_curve", 2));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    listen_stats_persist();
    ESP_LOGI(TAG, "Saved config vol=%d name=%s city=%s", cfg->volume,
             cfg->name[0] ? cfg->name : "-", cfg->city[0] ? cfg->city : "-");
    return ESP_OK;
}

const char *config_store_stream_url(const sb_config_t *cfg)
{
    if (cfg && strncmp(cfg->url_key, "URL2", 4) == 0) {
        return SB_URL2;
    }
    return SB_URL1;
}

esp_err_t config_store_set_play_updated(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "play_upd", on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool config_store_take_play_updated(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_get_u8(h, "play_upd", &v);
    if (v) {
        nvs_set_u8(h, "play_upd", 0);
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}

bool config_store_consume_fw_change(const char *version)
{
    if (!version || !version[0]) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    char prev[32] = {0};
    size_t len = sizeof(prev);
    nvs_get_str(h, "last_fw", prev, &len);
    bool changed = (strcmp(prev, version) != 0);
    nvs_set_str(h, "last_fw", version);
    nvs_commit(h);
    nvs_close(h);
    return changed;
}

bool config_store_has_wifi(const sb_config_t *cfg)
{
    return cfg && cfg->ssid[0] != 0;
}

esp_err_t config_store_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", "");
    nvs_set_str(h, "pass", "");
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "Cleared saved WiFi");
    return err;
}

bool config_store_load_touch_sens(float *up, float *dn, uint8_t *rev)
{
    nvs_handle_t h;
    int32_t u = 0;
    int32_t d = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t ok_flag = 0;
    uint8_t stored_rev = 1;
    nvs_get_u8(h, "tsens_ok", &ok_flag);
    if (nvs_get_u8(h, "tsens_rev", &stored_rev) != ESP_OK || stored_rev < 1) {
        stored_rev = 1;
    }
    bool ok = ok_flag == 1
              && nvs_get_i32(h, "tsens_up", &u) == ESP_OK
              && nvs_get_i32(h, "tsens_dn", &d) == ESP_OK
              && u >= 1 && u <= 500 && d >= 1 && d <= 500;
    nvs_close(h);
    if (!ok) {
        return false;
    }
    if (up) {
        *up = (float)u / 1000.0f;
    }
    if (dn) {
        *dn = (float)d / 1000.0f;
    }
    if (rev) {
        *rev = stored_rev;
    }
    return true;
}

esp_err_t config_store_save_touch_sens(float up, float dn)
{
    int32_t u = (int32_t)(up * 1000.0f + 0.5f);
    int32_t d = (int32_t)(dn * 1000.0f + 0.5f);
    if (u < 1) {
        u = 1;
    }
    if (u > 500) {
        u = 500;
    }
    if (d < 1) {
        d = 1;
    }
    if (d > 500) {
        d = 500;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, "tsens_up", u);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, "tsens_dn", d);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tsens_ok", 1);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tsens_rev", SB_TOUCH_SENS_REV);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "touch sens up=%.3f dn=%.3f", (float)u / 1000.0f,
                 (float)d / 1000.0f);
    }
    return err;
}

esp_err_t config_store_wipe_touch_if_usb_factory(void)
{
    struct stat st;
    if (stat("/spiffs/usb_factory", &st) != 0) {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB factory: pad-cal wipe skipped (%s), keeping marker",
                 esp_err_to_name(err));
        return err;
    }
    (void)nvs_erase_key(h, "tsens_ok");
    (void)nvs_erase_key(h, "tsens_up");
    (void)nvs_erase_key(h, "tsens_dn");
    (void)nvs_erase_key(h, "tsens_rev");
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB factory: pad-cal NVS commit failed (%s), keeping marker",
                 esp_err_to_name(err));
        return err;
    }

    if (unlink("/spiffs/usb_factory") != 0) {
        ESP_LOGW(TAG, "USB factory: wiped pad cal but could not unlink marker");
    } else {
        ESP_LOGW(TAG, "USB factory: wiped pad calibration");
    }
    return ESP_OK;
}