// ILI9341 (2,8″, 240 × 320) an SPI0, quer betrieben (320 × 240).
//
// Anschluss (docs/verdrahtung.svg): SCK GP2, MOSI GP3, MISO GP4, CS GP8, DC GP6, RESET GP7.

#pragma once

#include <stdint.h>

#define ILI_PIN_SCK 2u
#define ILI_PIN_MOSI 3u
#define ILI_PIN_MISO 4u
#define ILI_PIN_DC 6u
#define ILI_PIN_RST 7u
#define ILI_PIN_CS 8u

#ifndef ILI_SPI_HZ
#define ILI_SPI_HZ 25000000u  // für 10–20 cm Steckkabel; mehr nur bei kurzen Leitungen
#endif

// rotation: 0 = quer, 1 = quer um 180° gedreht
void ili_init(uint8_t rotation);
void ili_set_rotation(uint8_t rotation);
// Kennung aus Register 0xD3 (ILI9341: 0x9341); 0xFFFF, wenn über SDO nichts zurückkommt
uint32_t ili_read_id(void);
// Rechteck aus dem Bildspeicher (Breite fb_w, RGB565) übertragen
void ili_push_rect(const uint16_t *fb, int fb_w, int x, int y, int w, int h);
