#include "board_hw.h"

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

static const char *TAG = "board_hw";

static board_hw_id_t s_id = BOARD_HW_ZERO;
static int s_led = CONFIG_SEARABOOM_LED_GPIO;
static int s_dout = CONFIG_SEARABOOM_I2S_DOUT;
static int s_bclk = CONFIG_SEARABOOM_I2S_BCLK;
static int s_ws = CONFIG_SEARABOOM_I2S_LRC;
static int s_vol_up = CONFIG_SEARABOOM_BTN_VOL_UP;
static int s_vol_dn = CONFIG_SEARABOOM_BTN_VOL_DOWN;
static bool s_detected;
static uint32_t s_rise_21;
static uint32_t s_rise_47;
static uint32_t s_rise_48;
static char s_lows[64];
static bool s_led_ambiguous;

/* Header GPIOs only. Never 0/19/20/43/44/45/46 (USB, UART, strapping). */
static const int s_scan[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 47, 48,
};

static uint32_t pin_rise_cycles(int gpio)
{
    gpio_num_t p = (gpio_num_t)gpio;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(p, 0);
    esp_rom_delay_us(80);
    if (gpio_get_level(p) != 0) {
        gpio_reset_pin(p);
        return 0;
    }
    uint32_t t0 = esp_cpu_get_cycle_count();
    gpio_set_level(p, 1);
    uint32_t t1 = t0;
    for (int i = 0; i < 200000; i++) {
        t1 = esp_cpu_get_cycle_count();
        if (gpio_get_level(p)) {
            break;
        }
    }
    gpio_reset_pin(p);
    return t1 - t0;
}

static uint32_t pin_rise_avg(int gpio)
{
    uint32_t a = pin_rise_cycles(gpio);
    uint32_t b = pin_rise_cycles(gpio);
    return (a < b) ? a : b;
}

static int pin_pulled_down(int gpio)
{
    gpio_reset_pin((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)gpio, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(40);
    int v = gpio_get_level((gpio_num_t)gpio);
    gpio_reset_pin((gpio_num_t)gpio);
    return v == 0;
}

void board_hw_detect(void)
{
    if (s_detected) {
        return;
    }

    /* Same I2S/touch on both carriers: amp follows the 5V/GND/IO1..6 edge.
     * Only the onboard WS2812 GPIO differs (Zero 21, SuperMini 48). */
    s_dout = CONFIG_SEARABOOM_I2S_DOUT;
    s_bclk = CONFIG_SEARABOOM_I2S_BCLK;
    s_ws = CONFIG_SEARABOOM_I2S_LRC;
    s_vol_up = CONFIG_SEARABOOM_BTN_VOL_UP;
    s_vol_dn = CONFIG_SEARABOOM_BTN_VOL_DOWN;

    s_rise_21 = pin_rise_avg(21);
    s_rise_47 = pin_rise_avg(47);
    s_rise_48 = pin_rise_avg(48);
    ESP_LOGI(TAG, "WS2812 load rise cycles 21=%u 47=%u 48=%u",
             (unsigned)s_rise_21, (unsigned)s_rise_47, (unsigned)s_rise_48);

    uint32_t best = s_rise_21;
    int led = 21;
    board_hw_id_t id = BOARD_HW_ZERO;
    if (s_rise_48 > best) {
        best = s_rise_48;
        led = 48;
        id = BOARD_HW_SUPERMINI;
    }
    if (s_rise_47 > best) {
        best = s_rise_47;
        led = 47;
        id = BOARD_HW_SUPERMINI;
    }

    uint32_t other = (led == 21) ? ((s_rise_48 > s_rise_47) ? s_rise_48 : s_rise_47)
                                 : s_rise_21;
    if (other == 0) {
        other = 1;
    }
    /* Internal 45k pull-up is too stiff to see the WS2812 on 21 vs 48.
     * I2S 2/3/4 is the same on both live carriers; LED is driven on 21 and 48. */
    if (best < other * 3u / 2u) {
        ESP_LOGW(TAG, "LED load ambiguous (21=%u 47=%u 48=%u) — dual-drive 21+48",
                 (unsigned)s_rise_21, (unsigned)s_rise_47, (unsigned)s_rise_48);
        s_led_ambiguous = true;
        id = BOARD_HW_ZERO;
        led = CONFIG_SEARABOOM_LED_GPIO;
    }

    s_id = id;
    s_led = led;
    s_detected = true;
    ESP_LOGI(TAG, "board=%s LED=%d I2S dout=%d bclk=%d ws=%d vol+=%d vol-=%d",
             board_hw_name(), s_led, s_dout, s_bclk, s_ws, s_vol_up, s_vol_dn);

    s_lows[0] = 0;
    size_t n = 0;
    for (size_t i = 0; i < sizeof(s_scan) / sizeof(s_scan[0]); i++) {
        if (n + 8 >= sizeof(s_lows)) {
            break;
        }
        if (pin_pulled_down(s_scan[i])) {
            n += (size_t)snprintf(s_lows + n, sizeof(s_lows) - n, "%s%d",
                                  n ? "," : "", s_scan[i]);
        }
    }
    ESP_LOGI(TAG, "pull-up stayed low: %s", s_lows[0] ? s_lows : "(none)");
}

board_hw_id_t board_hw_id(void)
{
    return s_id;
}

const char *board_hw_name(void)
{
    if (s_led_ambiguous) {
        return "s3-zero+supermini";
    }
    return (s_id == BOARD_HW_SUPERMINI) ? "s3-supermini" : "s3-zero";
}

int board_hw_led_gpio(void)
{
    return s_led;
}

int board_hw_i2s_dout(void)
{
    return s_dout;
}

int board_hw_i2s_bclk(void)
{
    return s_bclk;
}

int board_hw_i2s_ws(void)
{
    return s_ws;
}

int board_hw_vol_up_gpio(void)
{
    return s_vol_up;
}

int board_hw_vol_down_gpio(void)
{
    return s_vol_dn;
}

void board_hw_dump(void)
{
    printf("board=%s i2s dout=%d bclk=%d ws=%d vol+=%d vol-=%d led=21+48 rise21=%u rise47=%u rise48=%u pulled_low=%s\n",
           board_hw_name(), s_dout, s_bclk, s_ws, s_vol_up, s_vol_dn,
           (unsigned)s_rise_21, (unsigned)s_rise_47, (unsigned)s_rise_48,
           s_lows[0] ? s_lows : "none");
}
