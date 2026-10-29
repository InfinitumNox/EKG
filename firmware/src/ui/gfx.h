// Grafik für den Bildspeicher (RGB565), ohne Hardware-Abhängigkeit: dieselben Funktionen
// laufen im Gerät und in tools/display_preview.py am Laptop.
//
// Farben werden als 0xRRGGBB übergeben; Deckkraft 0 … 256. Kanten sind geglättet.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t *px;
    int w;
    int h;
} canvas_t;

typedef struct {
    int x, y, w, h;
} rect_t;

typedef struct {
    uint16_t cp;  // Unicode
    uint8_t w, h;
    int8_t x;  // Versatz zum Stift
    int8_t y;  // Oberkante relativ zur Grundlinie (negativ = darüber)
    uint8_t adv;
    uint32_t off;  // Byte-Offset in bits (4 Bit je Pixel)
} glyph_t;

typedef struct {
    const glyph_t *g;
    uint16_t n;
    const uint8_t *bits;
    int8_t ascent;
    int8_t descent;
} font_t;

typedef struct {
    uint16_t w, h;
    const uint8_t *a;  // 8 Bit Deckkraft je Pixel
} alpha_img_t;

#define OPAQUE 256

static inline uint16_t rgb565(uint32_t rgb) {
    return (uint16_t)(((rgb >> 8) & 0xF800u) | ((rgb >> 5) & 0x07E0u) | ((rgb >> 3) & 0x001Fu));
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t);  // t = 0 … 256 (0 = a)

void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t rgb);
void gfx_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_blend(canvas_t *c, int x, int y, uint32_t rgb, int alpha);
void gfx_image(canvas_t *c, const alpha_img_t *img, int x, int y, uint32_t rgb, int opacity);
// Spalten [from, to) des Bildes, waagrecht von rgb0 nach rgb1 eingefärbt
void gfx_image_hgrad(canvas_t *c, const alpha_img_t *img, int x, int y, int from, int to,
                     uint32_t rgb0, uint32_t rgb1, int opacity);
int gfx_text(canvas_t *c, const font_t *f, int x, int baseline, const char *utf8, uint32_t rgb,
             int opacity);
int gfx_text_width(const font_t *f, const char *utf8);
void gfx_line(canvas_t *c, float x0, float y0, float x1, float y1, float width, uint32_t rgb,
              int opacity);
void gfx_disc(canvas_t *c, float cx, float cy, float r, uint32_t rgb, int opacity);
// weicher, nach außen auslaufender Schein (Deckkraft in der Mitte)
void gfx_glow(canvas_t *c, float cx, float cy, float r, uint32_t rgb, int opacity);
void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t rgb, int opacity);
void gfx_round_frame(canvas_t *c, int x, int y, int w, int h, int r, float width, uint32_t rgb);
