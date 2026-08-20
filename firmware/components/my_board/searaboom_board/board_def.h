#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

#define BUTTON_VOLUP_ID           7  /* TOUCH_PAD_NUM7 */
#define BUTTON_VOLDOWN_ID         6  /* TOUCH_PAD_NUM6 */
#define BUTTON_MUTE_ID            -1
#define BUTTON_SET_ID             -1
#define BUTTON_MODE_ID            -1
#define BUTTON_PLAY_ID            -1
#define PA_ENABLE_GPIO            -1
#define ADC_DETECT_GPIO           -1
#define BATTERY_DETECT_GPIO       -1
#define SDCARD_INTR_GPIO          -1

#define SDCARD_OPEN_FILE_NUM_MAX  5
#define BOARD_PA_GAIN             (0)

#define SDCARD_PWR_CTRL             -1
#define ESP_SD_PIN_CLK              -1
#define ESP_SD_PIN_CMD              -1
#define ESP_SD_PIN_D0               -1
#define ESP_SD_PIN_D1               -1
#define ESP_SD_PIN_D2               -1
#define ESP_SD_PIN_D3               -1
#define ESP_SD_PIN_D4               -1
#define ESP_SD_PIN_D5               -1
#define ESP_SD_PIN_D6               -1
#define ESP_SD_PIN_D7               -1
#define ESP_SD_PIN_CD               -1
#define ESP_SD_PIN_WP               -1

#include "dummy_codec.h"

#define AUDIO_CODEC_DEFAULT_CONFIG(){                   \
        .adc_input  = AUDIO_HAL_ADC_INPUT_LINE1,        \
        .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,         \
        .codec_mode = AUDIO_HAL_CODEC_MODE_DECODE,      \
        .i2s_iface = {                                  \
            .mode = AUDIO_HAL_MODE_SLAVE,               \
            .fmt = AUDIO_HAL_I2S_NORMAL,                \
            .samples = AUDIO_HAL_44K_SAMPLES,           \
            .bits = AUDIO_HAL_BIT_LENGTH_16BITS,        \
        },                                              \
}

/* Volume is IDF touch_element on T6/T7 (volume_buttons.c). ADF
 * input_key_service is not used: PERIPH_ID_BUTTON is GPIO, and
 * periph_touch does not read S3 pads. */
#define INPUT_KEY_NUM 0
#define INPUT_KEY_DEFAULT_INFO() {}

#endif
