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

// Eine Wandlung: Befehl senden, 12 Bit lesen (PD = 00: danach Stromsparen mit Meldeleitung)
static int xpt_read(uint8_t cmd) {
    uint8_t tx[3] = {cmd, 0x00, 0x00}, rx[3];
    spi_write_read_blocking(spi1, tx, rx, 3);
    return (int)(((((uint32_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFFu);
}

static int median5(int *v, int *spread) {
    for (int i = 1; i < 5; i++) {
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    }
    *spread = v[3] - v[1];
    return v[2];
}

void touch_read(touch_sample_t *t) {
    t->pressed = !gpio_get(HMI_PIN_T_IRQ);
    t->valid = false;
    t->rx = t->ry = t->z = 0;
    if (!t->pressed) {
        return;
    }
    int xs[5], ys[5];
    gpio_put(HMI_PIN_T_CS, 0);
    int z1 = xpt_read(0xB0), z2 = xpt_read(0xC0);
    (void)xpt_read(0xD0);  // erste Wandlung nach dem Umschalten streut
    for (int i = 0; i < 5; i++) {
        xs[i] = xpt_read(0xD0);
    }
    (void)xpt_read(0x90);
    for (int i = 0; i < 5; i++) {
        ys[i] = xpt_read(0x90);
    }
    gpio_put(HMI_PIN_T_CS, 1);
    int sx, sy;
    t->rx = median5(xs, &sx);
    t->ry = median5(ys, &sy);
    t->z = z1 + 4095 - z2;
    bool still = !gpio_get(HMI_PIN_T_IRQ);  // während des Lesens nicht losgelassen
    t->valid = still && t->z > 250 && t->rx > 40 && t->rx < 4055 && t->ry > 40 && t->ry < 4055 &&
               sx < 80 && sy < 80;
}
