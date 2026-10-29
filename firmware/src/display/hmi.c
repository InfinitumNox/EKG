#include "hmi.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"

#define PWM_HZ 25000u  // weit über dem EKG-Band; bei 100 % schaltet gar nichts

static uint pwm_wrap;

void hmi_init(void) {
    // Beleuchtung: PWM schaltet nur den Transistor Q1 auf dem Display
    gpio_set_function(HMI_PIN_LED, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(HMI_PIN_LED);
    pwm_wrap = clock_get_hz(clk_sys) / PWM_HZ - 1;
    pwm_set_wrap(slice, pwm_wrap);
    pwm_set_gpio_level(HMI_PIN_LED, 0);
    pwm_set_enabled(slice, true);

    // Touch-Controller einmal ansprechen: Stromsparmodus mit aktiver Meldeleitung (PD = 00)
    gpio_init(HMI_PIN_T_CS);
    gpio_set_dir(HMI_PIN_T_CS, GPIO_OUT);
    gpio_put(HMI_PIN_T_CS, 1);
    gpio_init(HMI_PIN_T_IRQ);
    gpio_set_dir(HMI_PIN_T_IRQ, GPIO_IN);
    gpio_pull_up(HMI_PIN_T_IRQ);
    spi_init(spi1, 1000000);
    gpio_set_function(HMI_PIN_T_CLK, GPIO_FUNC_SPI);
    gpio_set_function(HMI_PIN_T_DIN, GPIO_FUNC_SPI);
    gpio_set_function(HMI_PIN_T_DO, GPIO_FUNC_SPI);
    uint8_t tx[3] = {0xD0, 0x00, 0x00}, rx[3];
    gpio_put(HMI_PIN_T_CS, 0);
    spi_write_read_blocking(spi1, tx, rx, 3);
    gpio_put(HMI_PIN_T_CS, 1);
}

void backlight_set(int permille) {
    if (permille < 0) {
        permille = 0;
    }
    uint level = permille >= 1000 ? pwm_wrap + 1 : (uint)permille * (pwm_wrap + 1) / 1000;
    pwm_set_gpio_level(HMI_PIN_LED, (uint16_t)level);
}

bool touch_pressed(void) {
    return !gpio_get(HMI_PIN_T_IRQ);
}
