#include "splash.h"

#include <math.h>
#include <stdio.h>

#include "assets.h"
#include "theme.h"

// Stilisierter Herzschlag (P, QRS, T) als Polylinie, x in 0 … 1, y in mV-ähnlichen Einheiten
static const float BEAT[][2] = {
    {0.00f, 0.0f},   {0.10f, 0.0f},   {0.14f, 0.12f},  {0.18f, 0.0f},   {0.24f, 0.0f},
    {0.26f, -0.15f}, {0.29f, 1.0f},   {0.32f, -0.35f}, {0.35f, 0.0f},   {0.46f, 0.0f},
    {0.52f, 0.22f},  {0.58f, 0.24f},  {0.64f, 0.0f},   {1.00f, 0.0f},
};
#define BEAT_N (int)(sizeof(BEAT) / sizeof(BEAT[0]))

static int ramp(uint32_t t, uint32_t start, uint32_t dur) {
    if (t <= start) {
        return 0;
    }
    if (t >= start + dur) {
        return 256;
    }
    float u = (float)(t - start) / (float)dur;
    u = u * u * (3.0f - 2.0f * u);  // weich ein- und auslaufen
    return (int)(u * 256.0f);
}

int splash_backlight(uint32_t t_ms) {
    return ramp(t_ms, 0, 600) * 1000 / 256;
}

// EKG-Linie bis zur Position u (0 … 1) mit leuchtender Spitze
static void draw_trace(canvas_t *c, float u, int opacity, float x0, float x1, float y0) {
    const float amp = 24.0f;
    const int beats = 2;
    float head_x = x0, head_y = y0;
    float prev_x = x0, prev_y = y0;
    for (int b = 0; b < beats; b++) {
        for (int k = 1; k < BEAT_N; k++) {
            float f0 = ((float)b + BEAT[k - 1][0]) / (float)beats;
            float f1 = ((float)b + BEAT[k][0]) / (float)beats;
            if (f0 >= u) {
                break;
            }
            float frac = f1 <= u ? 1.0f : (u - f0) / (f1 - f0);
            float ya = BEAT[k - 1][1], yb = BEAT[k][1];
            float xa = x0 + (x1 - x0) * f0, xb = x0 + (x1 - x0) * (f0 + (f1 - f0) * frac);
            float yb2 = ya + (yb - ya) * frac;
            float pa = y0 - ya * amp, pb = y0 - yb2 * amp;
            if (GLOW) {
                gfx_line(c, xa, pa, xb, pb, 5.0f, C_TRACE_GLOW, opacity / 3);
            }
            gfx_line(c, xa, pa, xb, pb, 2.0f, C_TRACE, opacity);
            prev_x = xb;
            prev_y = pb;
        }
    }
    head_x = prev_x;
    head_y = prev_y;
    if (u > 0.0f && u < 1.0f) {
        if (GLOW) {
            gfx_disc(c, head_x, head_y, 6.0f, C_TRACE, opacity / 5);
        }
        gfx_disc(c, head_x, head_y, 3.0f, C_HEAD, opacity);
    }
}

void splash_render(canvas_t *c, uint32_t t, const char *version) {
    const int w = c->w, h = c->h;
    const bool portrait = h > w;
    // hochkant: schmalerer Schriftzug, alles etwas tiefer
    const alpha_img_t *logo = portrait ? &IMG_LOGO_S : &IMG_LOGO;
    const int split = portrait ? LOGO_S_SPLIT_X : LOGO_SPLIT_X;
    const int logo_base = portrait ? LOGO_S_BASELINE : LOGO_BASELINE;
    const int heart_y = portrait ? 104 : 62, logo_y = portrait ? 188 : 140;
    const int sub_y = portrait ? 216 : 168;
    const float trace_y = portrait ? 280.0f : 214.0f, margin = portrait ? 22.0f : 28.0f;

    gfx_vgradient(c, 0, 0, w, h, SPLASH_TOP, SPLASH_BOTTOM);

    // Herz: schlägt mit ~70/min, zuerst eingeblendet
    int heart_in = ramp(t, 0, 500);
    uint32_t phase = t % 860u;
    bool beat = t > 300 && phase < 140u;
    const alpha_img_t *hi = beat ? &IMG_HEART_66 : &IMG_HEART_58;
    int hx = w / 2 - hi->w / 2, hy = heart_y - hi->h / 2;
    if (GLOW) {
        gfx_glow(c, (float)w / 2.0f, (float)heart_y + 2.0f, beat ? 62.0f : 54.0f, C_ACCENT,
                 heart_in * (beat ? 110 : 70) / 256);
    }
    gfx_image_hgrad(c, hi, hx, hy, 0, hi->w, C_ACCENT, C_ACCENT_2, heart_in);

    // Schriftzug „Sophi“ in Textfarbe, „EKG“ im Verlauf
    int logo_in = ramp(t, 250, 700);
    int lx = w / 2 - logo->w / 2;
    int ly = logo_y - logo_base + (256 - logo_in) * 10 / 256;  // gleitet leicht nach oben
    gfx_image_hgrad(c, logo, lx, ly, 0, split, C_TEXT, C_TEXT, logo_in);
    gfx_image_hgrad(c, logo, lx, ly, split, logo->w, C_ACCENT, C_ACCENT_2, logo_in);

    // Untertitel
    int sub_in = ramp(t, 900, 600);
    const char *sub = "1-Kanal-EKG";
    gfx_text(c, &FONT_LABEL, w / 2 - gfx_text_width(&FONT_LABEL, sub) / 2, sub_y, sub,
             C_TEXT_DIM, sub_in);

    // EKG-Linie zeichnet sich
    float u = (float)ramp(t, 700, 1900) / 256.0f;
    draw_trace(c, u, 256, margin, (float)w - margin, trace_y);

    // Version unten rechts
    if (version) {
        char buf[32];
        snprintf(buf, sizeof buf, "v%s", version);
        int vw = gfx_text_width(&FONT_SMALL, buf);
        gfx_text(c, &FONT_SMALL, w - vw - 10, h - 9, buf, C_TEXT_FAINT, sub_in);
    }
}
