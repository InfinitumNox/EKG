#include "gfx.h"

#include <math.h>
#include <stddef.h>

static inline uint32_t unpack(uint16_t p) {
    uint32_t r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) {
        return a;
    }
    if (t >= 256) {
        return b;
    }
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        int ca = (int)((a >> s) & 255u), cb = (int)((b >> s) & 255u);
        out |= (uint32_t)((ca * (256 - t) + cb * t) >> 8) << s;
    }
    return out;
}

void gfx_blend(canvas_t *c, int x, int y, uint32_t rgb, int alpha) {
    if (alpha <= 0 || x < 0 || y < 0 || x >= c->w || y >= c->h) {
        return;
    }
    uint16_t *p = &c->px[y * c->w + x];
    *p = rgb565(alpha >= 256 ? rgb : gfx_mix(unpack(*p), rgb, alpha));
}

void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t rgb) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    uint16_t v = rgb565(rgb);
    for (int j = y; j < y1; j++) {
        uint16_t *row = &c->px[j * c->w];
        for (int i = x; i < x1; i++) {
            row[i] = v;
        }
    }
}

// Geordnetes Dithering (Bayer 4×4), damit Verläufe in RGB565 keine Stufen zeigen.
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void gfx_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= c->h) {
            continue;
        }
        uint32_t col = gfx_mix(top, bottom, h > 1 ? j * 256 / (h - 1) : 0);
        int r = (int)(col >> 16), g = (int)((col >> 8) & 255u), b = (int)(col & 255u);
        uint16_t *row = &c->px[yy * c->w];
        for (int i = x < 0 ? 0 : x; i < x + w && i < c->w; i++) {
            int d = BAYER[yy & 3][i & 3];
            int rr = r + (d >> 1), gg = g + (d >> 2), bb = b + (d >> 1);  // < 1 Stufe
            rr = rr > 255 ? 255 : rr;
            gg = gg > 255 ? 255 : gg;
            bb = bb > 255 ? 255 : bb;
            row[i] = (uint16_t)(((rr & 0xF8) << 8) | ((gg & 0xFC) << 3) | (bb >> 3));
        }
    }
}

void gfx_image_hgrad(canvas_t *c, const alpha_img_t *img, int x, int y, int from, int to,
                     uint32_t rgb0, uint32_t rgb1, int opacity) {
    if (from < 0) from = 0;
    if (to > img->w) to = img->w;
    int span = to - from > 1 ? to - from - 1 : 1;
    for (int i = from; i < to; i++) {
        uint32_t col = gfx_mix(rgb0, rgb1, (i - from) * 256 / span);
        for (int j = 0; j < img->h; j++) {
            int a = img->a[j * img->w + i];
            if (a) {
                gfx_blend(c, x + i, y + j, col, (a + (a >> 7)) * opacity >> 8);
            }
        }
    }
}

void gfx_image(canvas_t *c, const alpha_img_t *img, int x, int y, uint32_t rgb, int opacity) {
    gfx_image_hgrad(c, img, x, y, 0, img->w, rgb, rgb, opacity);
}

static uint32_t next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1Fu;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0Fu;
        extra = 2;
    } else {
        cp = p[0] & 0x07u;
        extra = 3;
    }
    p++;
    for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++, p++) {
        cp = (cp << 6) | (*p & 0x3Fu);
    }
    *s = (const char *)p;
    return cp;
}

static const glyph_t *find_glyph(const font_t *f, uint32_t cp) {
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->g[mid].cp == cp) {
            return &f->g[mid];
        }
        if (f->g[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return cp == '?' ? NULL : find_glyph(f, '?');
}

int gfx_text_width(const font_t *f, const char *s) {
    int w = 0;
    while (*s) {
        const glyph_t *g = find_glyph(f, next_cp(&s));
        if (g) {
            w += g->adv;
        }
    }
    return w;
}

int gfx_text(canvas_t *c, const font_t *f, int x, int baseline, const char *s, uint32_t rgb,
             int opacity) {
    while (*s) {
        const glyph_t *g = find_glyph(f, next_cp(&s));
        if (!g) {
            continue;
        }
        const uint8_t *bits = f->bits + g->off;
        for (int j = 0; j < g->h; j++) {
            for (int i = 0; i < g->w; i++) {
                int k = j * g->w + i;
                int v = (bits[k >> 1] >> ((k & 1) ? 0 : 4)) & 15;
                if (v) {
                    gfx_blend(c, x + g->x + i, baseline + g->y + j, rgb, v * 17 * opacity >> 8);
                }
            }
        }
        x += g->adv;
    }
    return x;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void gfx_line(canvas_t *c, float x0, float y0, float x1, float y1, float width, uint32_t rgb,
              int opacity) {
    float half = width * 0.5f;
    int bx0 = (int)floorf(fminf(x0, x1) - half - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + half + 1);
    int by0 = (int)floorf(fminf(y0, y1) - half - 1), by1 = (int)ceilf(fmaxf(y0, y1) + half + 1);
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int j = by0; j <= by1; j++) {
        for (int i = bx0; i <= bx1; i++) {
            float px = (float)i + 0.5f - x0, py = (float)j + 0.5f - y0;
            float t = len2 > 0.0f ? clampf((px * dx + py * dy) / len2, 0.0f, 1.0f) : 0.0f;
            float ex = px - t * dx, ey = py - t * dy;
            float cov = clampf(half + 0.5f - sqrtf(ex * ex + ey * ey), 0.0f, 1.0f);
            if (cov > 0.0f) {
                gfx_blend(c, i, j, rgb, (int)(cov * (float)opacity));
            }
        }
    }
}

void gfx_disc(canvas_t *c, float cx, float cy, float r, uint32_t rgb, int opacity) {
    gfx_line(c, cx, cy, cx, cy, 2.0f * r, rgb, opacity);
}

void gfx_glow(canvas_t *c, float cx, float cy, float r, uint32_t rgb, int opacity) {
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int j = y0; j <= y1; j++) {
        for (int i = x0; i <= x1; i++) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy;
            float u = 1.0f - sqrtf(dx * dx + dy * dy) / r;
            if (u > 0.0f) {
                gfx_blend(c, i, j, rgb, (int)(u * u * (float)opacity));
            }
        }
    }
}

// Abdeckung eines Pixels durch ein abgerundetes Rechteck (Abstandsfunktion).
static float rr_cover(float px, float py, float x, float y, float w, float h, float r) {
    float cx = clampf(px, x + r, x + w - r), cy = clampf(py, y + r, y + h - r);
    float dx = px - cx, dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy) - r;
    if (px > x + r && px < x + w - r) {
        d = fmaxf(y - py, py - (y + h));
    } else if (py > y + r && py < y + h - r) {
        d = fmaxf(x - px, px - (x + w));
    }
    return clampf(0.5f - d, 0.0f, 1.0f);
}

void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t rgb, int opacity) {
    for (int j = y - 1; j <= y + h; j++) {
        for (int i = x - 1; i <= x + w; i++) {
            float cov = rr_cover((float)i + 0.5f, (float)j + 0.5f, (float)x, (float)y, (float)w,
                                 (float)h, (float)r);
            if (cov > 0.0f) {
                gfx_blend(c, i, j, rgb, (int)(cov * (float)opacity));
            }
        }
    }
}

void gfx_round_frame(canvas_t *c, int x, int y, int w, int h, int r, float width, uint32_t rgb) {
    float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h, fr = (float)r;
    for (int j = y - 1; j <= y + h; j++) {
        for (int i = x - 1; i <= x + w; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            float outer = rr_cover(px, py, fx, fy, fw, fh, fr);
            float inner = rr_cover(px, py, fx + width, fy + width, fw - 2 * width, fh - 2 * width,
                                   fr > width ? fr - width : 0.0f);
            float cov = outer - inner;
            if (cov > 0.0f) {
                gfx_blend(c, i, j, rgb, (int)(cov * 256.0f));
            }
        }
    }
}
