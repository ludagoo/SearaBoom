#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include "board.h"
#include "audio_error.h"
#include "audio_mem.h"
#include "soc/soc_caps.h"

static const char *TAG = "SEARABOOM_BOARD";

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config)
{
    AUDIO_NULL_CHECK(TAG, i2c_config, return ESP_FAIL);
    i2c_config->sda_io_num = -1;
    i2c_config->scl_io_num = -1;
    (void)port;
    return ESP_OK;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config)
{
    AUDIO_NULL_CHECK(TAG, i2s_config, return ESP_FAIL);
    if (port == 0) {
        /* Match original BoomV5S3Zero.ino pinout */
        i2s_config->mck_io_num = -1;
        i2s_config->bck_io_num = GPIO_NUM_3;
        i2s_config->ws_io_num = GPIO_NUM_4;
        i2s_config->data_out_num = GPIO_NUM_2;
        i2s_config->data_in_num = -1;
    } else {
        memset(i2s_config, -1, sizeof(board_i2s_pin_t));
        ESP_LOGE(TAG, "i2s port %d is not supported", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_device_interface_config)
{
    AUDIO_NULL_CHECK(TAG, spi_config, return ESP_FAIL);
    AUDIO_NULL_CHECK(TAG, spi_device_interface_config, return ESP_FAIL);
    spi_config->mosi_io_num = -1;
    spi_config->miso_io_num = -1;
    spi_config->sclk_io_num = -1;
    spi_config->quadwp_io_num = -1;
    spi_config->quadhd_io_num = -1;
    spi_device_interface_config->spics_io_num = -1;
    return ESP_OK;
}

int8_t get_sdcard_intr_gpio(void) { return SDCARD_INTR_GPIO; }
int8_t get_sdcard_open_file_num_max(void) { return SDCARD_OPEN_FILE_NUM_MAX; }
int8_t get_input_volup_id(void) { return BUTTON_VOLUP_ID; }
int8_t get_input_voldown_id(void) { return BUTTON_VOLDOWN_ID; }
int8_t get_pa_enable_gpio(void) { return PA_ENABLE_GPIO; }
int8_t get_input_mode_id(void) { return BUTTON_MODE_ID; }
int8_t get_input_set_id(void) { return BUTTON_SET_ID; }
int8_t get_input_play_id(void) { return BUTTON_PLAY_ID; }
int8_t get_input_mute_id(void) { return BUTTON_MUTE_ID; }