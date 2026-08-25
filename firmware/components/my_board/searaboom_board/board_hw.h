#ifndef SEARABOOM_BOARD_HW_H
#define SEARABOOM_BOARD_HW_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_HW_ZERO = 0,
    BOARD_HW_SUPERMINI,
} board_hw_id_t;

/* Input/LED-load only. Call before I2S or WS2812 init. */
void board_hw_detect(void);

board_hw_id_t board_hw_id(void);
const char *board_hw_name(void);
int board_hw_led_gpio(void);
int board_hw_i2s_dout(void);
int board_hw_i2s_bclk(void);
int board_hw_i2s_ws(void);
int board_hw_vol_up_gpio(void);
int board_hw_vol_down_gpio(void);
void board_hw_dump(void);

#ifdef __cplusplus
}
#endif

#endif
