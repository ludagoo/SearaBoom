#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "config_store.h"
#include "sdkconfig.h"

static const char *TAG = "config_store";
static const char *NVS_NS = "searaboom";

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
    cfg->volume = SB_DEFAULT_VOLUME;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved config, using defaults");
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
    int32_t vol = SB_DEFAULT_VOLUME;
    if (nvs_get_i32(h, "volume", &vol) == ESP_OK) {
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
        nvs_handle_t hw;
        if (nvs_open(NVS_NS, NVS_READWRITE, &hw) == ESP_OK) {
            nvs_set_i32(hw, "volume", cfg->volume);
            nvs_set_i32(hw, "vol_curve", 2);
            nvs_commit(hw);
            nvs_close(hw);
        }
    }

    ESP_LOGI(TAG, "Loaded ssid=%s url=%s vol=%d", cfg->ssid, cfg->url_key, cfg->volume);
    return ESP_OK;
}

esp_err_t config_store_save(const sb_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", cfg->ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", cfg->password));
    ESP_ERROR_CHECK(nvs_set_str(h, "url", cfg->url_key));
    ESP_ERROR_CHECK(nvs_set_i32(h, "volume", cfg->volume));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Saved config");
    return ESP_OK;
}

const char *config_store_stream_url(const sb_config_t *cfg)
{
    if (cfg && strncmp(cfg->url_key, "URL2", 4) == 0) {
        return SB_URL2;
    }
    return SB_URL1;
}