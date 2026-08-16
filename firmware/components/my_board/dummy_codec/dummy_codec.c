#include <string.h>
#include "esp_log.h"
#include "dummy_codec.h"

static const char *TAG = "dummy_codec";
static bool codec_init_flag;
static int s_volume = 50;

audio_hal_func_t AUDIO_NEW_CODEC_DEFAULT_HANDLE = {
    .audio_codec_initialize = new_codec_init,
    .audio_codec_deinitialize = new_codec_deinit,
    .audio_codec_ctrl = new_codec_ctrl_state,
    .audio_codec_config_iface = new_codec_config_i2s,
    .audio_codec_set_mute = new_codec_set_voice_mute,
    .audio_codec_set_volume = new_codec_set_voice_volume,
    .audio_codec_get_volume = new_codec_get_voice_volume,
};

bool new_codec_initialized(void)
{
    return codec_init_flag;
}

esp_err_t new_codec_init(audio_hal_codec_config_t *cfg)
{
    (void)cfg;
    ESP_LOGI(TAG, "passthrough codec (external I2S amp)");
    codec_init_flag = true;
    return ESP_OK;
}

esp_err_t new_codec_deinit(void)
{
    codec_init_flag = false;
    return ESP_OK;
}

esp_err_t new_codec_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    (void)mode;
    (void)ctrl_state;
    return ESP_OK;
}

esp_err_t new_codec_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    (void)mode;
    (void)iface;
    return ESP_OK;
}

esp_err_t new_codec_set_voice_mute(bool mute)
{
    (void)mute;
    return ESP_OK;
}

esp_err_t new_codec_set_voice_volume(int volume)
{
    s_volume = volume;
    return ESP_OK;
}

esp_err_t new_codec_get_voice_volume(int *volume)
{
    if (!volume) {
        return ESP_FAIL;
    }
    *volume = s_volume;
    return ESP_OK;
}