#ifndef _DUMMY_CODEC_H_
#define _DUMMY_CODEC_H_

#include "audio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

bool new_codec_initialized(void);
esp_err_t new_codec_init(audio_hal_codec_config_t *codec_cfg);
esp_err_t new_codec_deinit(void);
esp_err_t new_codec_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state);
esp_err_t new_codec_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface);
esp_err_t new_codec_set_voice_mute(bool mute);
esp_err_t new_codec_set_voice_volume(int volume);
esp_err_t new_codec_get_voice_volume(int *volume);

extern audio_hal_func_t AUDIO_NEW_CODEC_DEFAULT_HANDLE;

#ifdef __cplusplus
}
#endif

#endif
