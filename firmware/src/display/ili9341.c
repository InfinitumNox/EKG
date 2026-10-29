#include "ili9341.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define SPI spi0

static void cs(bool active) {
    gpio_put(ILI_PIN_CS, !active);
}

static void wait_idle(void) {
    while (spi_is_busy(SPI)) {
        tight_loop_contents();
    }
}

static void command(uint8_t cmd, const uint8_t *data, size_t n) {
    cs(true);
    gpio_put(ILI_PIN_DC, 0);
    spi_write_blocking(SPI, &cmd, 1);
    wait_idle();
    if (n) {
        gpio_put(ILI_PIN_DC, 1);
        spi_write_blocking(SPI, data, n);
        wait_idle();
    }
    cs(false);
}

// Initialisierung wie im verbreiteten Adafruit-Treiber
static const uint8_t INIT[] = {
    0xEF, 3, 0x03, 0x80, 0x02,
    0xCF, 3, 0x00, 0xC1, 0x30,
    0xED, 4, 0x64, 0x03, 0x12, 0x81,
    0xE8, 3, 0x85, 0x00, 0x78,
    0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
    0xF7, 1, 0x20,
    0xEA, 2, 0x00, 0x00,
    0xC0, 1, 0x23,        // Power Control 1
    0xC1, 1, 0x10,        // Power Control 2
    0xC5, 2, 0x3E, 0x28,  // VCOM
    0xC7, 1, 0x86,
    0x37, 1, 0x00,        // kein Scrollen
    0x3A, 1, 0x55,        // 16 Bit je Pixel
    0xB1, 2, 0x00, 0x18,  // Bildfrequenz ~79 Hz
    0xB6, 3, 0x08, 0x82, 0x27,
    0xF2, 1, 0x00,
    0x26, 1, 0x01,        // Gamma-Kurve 1
    0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E,
    0x09, 0x00,
    0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31,
    0x36, 0x0F,
    0x00,
};

void ili_set_rotation(uint8_t rotation) {
    // MV (Zeilen/Spalten tauschen) für quer, BGR-Reihenfolge des Panels
    uint8_t madctl = rotation ? 0xE8 : 0x28;
    command(0x36, &madctl, 1);
}

// Ein Parameterbyte lesen. Über SPI liefert der ILI9341 mehrbytige Register nur sauber über
// das undokumentierte Register 0xD9 (Index 0x10 + n), so machen es auch Adafruit und TFT_eSPI.
static uint8_t read_param(uint8_t cmd, uint8_t index) {
    uint8_t sel = (uint8_t)(0x10u + index), v = 0;
    command(0xD9, &sel, 1);
    cs(true);
    gpio_put(ILI_PIN_DC, 0);
    spi_write_blocking(SPI, &cmd, 1);
    gpio_put(ILI_PIN_DC, 1);
    spi_read_blocking(SPI, 0x00, &v, 1);
    cs(false);
    return v;
}

uint32_t ili_read_id(void) {
    spi_set_baudrate(SPI, 4000000);  // Lesen nur langsam (Datenblatt: max. ~6 MHz)
    uint32_t id = ((uint32_t)read_param(0xD3, 2) << 8) | read_param(0xD3, 3);  // 0x93, 0x41
    if (id != 0x9341u) {
        // Ersatzweg: alle vier Bytes am Stück (Dummy, 0x00, 0x93, 0x41)
        uint8_t cmd = 0xD3, rx[4] = {0};
        cs(true);
        gpio_put(ILI_PIN_DC, 0);
        spi_write_blocking(SPI, &cmd, 1);
        gpio_put(ILI_PIN_DC, 1);
        spi_read_blocking(SPI, 0x00, rx, 4);
        cs(false);
        uint32_t direct = ((uint32_t)rx[2] << 8) | rx[3];
        if (direct == 0x9341u) {
            id = direct;
        }
    }
    spi_set_baudrate(SPI, ILI_SPI_HZ);
    return id;
}

void ili_init(uint8_t rotation) {
    spi_init(SPI, ILI_SPI_HZ);
    spi_set_format(SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(ILI_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(ILI_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(ILI_PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(ILI_PIN_MISO);  // ohne angeschlossenes SDO liest die Kennung 0xFFFF
    const uint pins[] = {ILI_PIN_DC, ILI_PIN_RST, ILI_PIN_CS};
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 1);
    }
    gpio_put(ILI_PIN_RST, 0);  // Hardware-Reset
    sleep_ms(10);
    gpio_put(ILI_PIN_RST, 1);
    sleep_ms(120);
    command(0x01, NULL, 0);  // Software-Reset
    sleep_ms(150);
    for (const uint8_t *p = INIT; *p;) {
        uint8_t cmd = *p++, n = *p++;
        command(cmd, p, n);
        p += n;
    }
    ili_set_rotation(rotation);
    command(0x11, NULL, 0);  // aus dem Schlafmodus
    sleep_ms(120);
    command(0x29, NULL, 0);  // Anzeige an
}

void ili_push_rect(const uint16_t *fb, int fb_w, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    uint16_t x1 = (uint16_t)(x + w - 1), y1 = (uint16_t)(y + h - 1);
    uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t pa[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
    command(0x2A, ca, 4);  // Spalten
    command(0x2B, pa, 4);  // Zeilen
    cs(true);
    gpio_put(ILI_PIN_DC, 0);
    uint8_t ramwr = 0x2C;
    spi_write_blocking(SPI, &ramwr, 1);
    wait_idle();
    gpio_put(ILI_PIN_DC, 1);
    // Pixel als 16-Bit-Wörter: das Display erwartet das höherwertige Byte zuerst
    spi_set_format(SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    for (int j = 0; j < h; j++) {
        spi_write16_blocking(SPI, &fb[(y + j) * fb_w + x], (size_t)w);
    }
    wait_idle();
    spi_set_format(SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    cs(false);
}
