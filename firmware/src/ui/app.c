#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "protocol.h"
#include "theme.h"
#include "touchcal.h"

// Anzeigestufen: Tippen (ohne Koordinaten) oder Menü schaltet der Reihe nach
typedef struct {
    const theme_t *theme;
    int backlight;  // Promille
    const char *name;
} look_t;

static const look_t LOOKS[] = {
    {&THEME_DAY, 1000, "Tag"},
    {&THEME_NIGHT, 1000, "Nacht"},
    {&THEME_NIGHT, 500, "Nacht 50 %"},
    {&THEME_NIGHT, 200, "Nacht 20 %"},
};
#define N_LOOKS (int)(sizeof LOOKS / sizeof LOOKS[0])

#define TOAST_MS 2600u
#define HEADER_H 26
#define FOOTER_Y 204

static const char *PAGE_NAMES[APP_PAGES] = {"Übersicht", "Verlauf", "HRV", "Median-Schlag",
                                             "Hinweise"};

// Ziele der Touch-Einrichtung (Bildschirm, aktuelle Drehung)
static const int CAL_X[5] = {30, 290, 290, 30, 160};
static const int CAL_Y[5] = {30, 30, 210, 210, 120};

typedef struct {
    int x, y, w, h;
} box_t;

static bool hit(box_t b, int x, int y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// ------------------------------------------------------------------ Formatierung
// Zahl mit Dezimalkomma; digits Nachkommastellen
static void fmt_num(char *buf, size_t n, double v, int digits) {
    if (!isfinite(v)) {
        snprintf(buf, n, "–");
        return;
    }
    double scale = digits == 2 ? 100.0 : digits == 1 ? 10.0 : 1.0;
    long q = lrint(v * scale);
    bool neg = q < 0;
    unsigned long u = (unsigned long)(neg ? -q : q);
    if (digits == 0) {
        snprintf(buf, n, "%s%lu", neg ? "-" : "", u);
    } else {
        unsigned long whole = u / (unsigned long)scale, frac = u % (unsigned long)scale;
        snprintf(buf, n, digits == 2 ? "%s%lu,%02lu" : "%s%lu,%lu", neg ? "-" : "", whole, frac);
    }
}

static void fmt_duration(char *buf, size_t n, double s) {
    int t = (int)lrint(s < 0 ? 0 : s > 5999 ? 5999 : s);
    snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

// ------------------------------------------------------------------ Bausteine
static void text_center(canvas_t *c, const font_t *f, int cx, int baseline, const char *s,
                        uint32_t col) {
    gfx_text(c, f, cx - gfx_text_width(f, s) / 2, baseline, s, col, OPAQUE);
}

static void button(canvas_t *c, box_t b, const char *label, bool primary, bool enabled) {
    if (primary) {
        gfx_round_rect(c, b.x, b.y, b.w, b.h, 11, C_ACCENT, OPAQUE);
    } else {
        gfx_round_rect(c, b.x, b.y, b.w, b.h, 10, gfx_mix(C_PANEL, C_TEXT, 14), OPAQUE);
        gfx_round_frame(c, b.x, b.y, b.w, b.h, 10, 1.0f, C_PANEL_EDGE);
    }
    uint32_t col = primary ? 0xFFFFFFu : enabled ? C_TEXT : C_TEXT_FAINT;
    text_center(c, &FONT_SMALL, b.x + b.w / 2, b.y + b.h / 2 + 5, label, col);
}

static void close_icon(canvas_t *c, int cx, int cy, uint32_t col) {
    gfx_line(c, (float)cx - 5.0f, (float)cy - 5.0f, (float)cx + 5.0f, (float)cy + 5.0f, 2.0f, col,
             OPAQUE);
    gfx_line(c, (float)cx - 5.0f, (float)cy + 5.0f, (float)cx + 5.0f, (float)cy - 5.0f, 2.0f, col,
             OPAQUE);
}

static void dim_screen(canvas_t *c) {
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            gfx_blend(c, x, y, C_BG, 170);
        }
    }
}

// ------------------------------------------------------------------ Stufen und Live
static void apply_look(app_t *a) {
    g_theme = LOOKS[a->ui->look].theme;
    a->req_backlight = LOOKS[a->ui->look].backlight;
}

int app_backlight(const app_t *a) {
    return LOOKS[a->ui->look].backlight;
}

static void redraw(app_t *a);

static void go_live(app_t *a) {
    a->screen = APP_LIVE;
    a->mon->draw = true;
    a->mon->menu_button = a->touch_xy;
    monitor_draw_all(a->mon);
    a->req_push_all = true;
}

static void show_toast(app_t *a, const char *text, uint32_t now) {
    snprintf(a->toast, sizeof a->toast, "%s", text);
    a->toast_until = now + TOAST_MS;
    a->toast_shown = true;
    redraw(a);
}

static void draw_toast(app_t *a) {
    canvas_t *c = a->c;
    const box_t b = {16, a->screen == APP_LIVE ? 34 : 150, 288, 44};
    gfx_round_rect(c, b.x, b.y, b.w, b.h, 12, C_PANEL, 245);
    gfx_round_frame(c, b.x, b.y, b.w, b.h, 12, 1.5f, C_ACCENT);
    int lines = gfx_text_wrap(NULL, &FONT_SMALL, 0, 0, b.w - 24, 16, a->toast, C_TEXT, 2);
    int base = lines > 1 ? b.y + 19 : b.y + 27;
    gfx_text_wrap(c, &FONT_SMALL, b.x + 12, base, b.w - 24, 16, a->toast, C_TEXT, 2);
}

// ------------------------------------------------------------------ Menü
static const box_t MENU_PANEL = {12, 8, 296, 224};
static const box_t MENU_CLOSE = {256, 8, 52, 40};
static const box_t MENU_MEASURE = {24, 46, 272, 46};
static const box_t MENU_LAST = {24, 100, 132, 36};
static const box_t MENU_LOOK = {164, 100, 132, 36};
static const box_t MENU_ROTATE = {24, 143, 132, 36};
static const box_t MENU_CAL = {164, 143, 132, 36};
static const box_t MENU_INFO = {24, 186, 132, 36};
static const box_t MENU_BACK = {164, 186, 132, 36};

// fresh_bg: Live-Ansicht dahinter neu zeichnen (nach einem Wechsel der Anzeige); beim
// Öffnen bleibt das letzte Bild mit Kurve stehen
static void draw_menu(app_t *a, bool fresh_bg) {
    canvas_t *c = a->c;
    if (fresh_bg) {
        monitor_draw_all(a->mon);
    }
    dim_screen(c);
    const box_t p = MENU_PANEL;
    gfx_round_rect(c, p.x, p.y, p.w, p.h, 14, C_PANEL, OPAQUE);
    gfx_round_frame(c, p.x, p.y, p.w, p.h, 14, 1.0f, C_PANEL_EDGE);
    gfx_text(c, &FONT_LABEL, 28, 33, "Menü", C_TEXT, OPAQUE);
    close_icon(c, 284, 27, C_TEXT_DIM);

    const box_t m = MENU_MEASURE;
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 12, C_ACCENT, OPAQUE);
    gfx_triangle(c, 42.0f, 59.0f, 42.0f, 79.0f, 58.0f, 69.0f, 0xFFFFFFu, OPAQUE);
    gfx_text(c, &FONT_LABEL, 70, 75, "Messen & auswerten", 0xFFFFFFu, OPAQUE);
    char dur[16];
    fmt_duration(dur, sizeof dur, ANA_MAX_S);
    gfx_text(c, &FONT_SMALL, m.x + m.w - 12 - gfx_text_width(&FONT_SMALL, dur), 74, dur,
             0xFFFFFFu, 200);

    char look[32];
    snprintf(look, sizeof look, "Anzeige: %s", LOOKS[a->ui->look].name);
    button(c, MENU_LAST, "Letzte Auswertung", false, a->has_result || a->result_status);
    button(c, MENU_LOOK, look, false, true);
    button(c, MENU_ROTATE, "Bild drehen", false, true);
    button(c, MENU_CAL, "Touch einrichten", false, true);
    button(c, MENU_INFO, "Info", false, true);
    button(c, MENU_BACK, "Schließen", false, true);
}

// ------------------------------------------------------------------ Auswertung läuft
static void draw_analyzing(app_t *a) {
    canvas_t *c = a->c;
    gfx_vgradient(c, 0, 0, SCREEN_W, SCREEN_H, SPLASH_TOP, SPLASH_BOTTOM);
    const alpha_img_t *h = &IMG_HEART_58;
    if (GLOW) {
        gfx_glow(c, SCREEN_W / 2.0f, 84.0f, 56.0f, C_ACCENT, 70);
    }
    gfx_image_hgrad(c, h, SCREEN_W / 2 - h->w / 2, 84 - h->h / 2, 0, h->w, C_ACCENT, C_ACCENT_2,
                    OPAQUE);
    text_center(c, &FONT_LABEL, SCREEN_W / 2, 150, "Auswertung läuft …", C_TEXT);
    text_center(c, &FONT_SMALL, SCREEN_W / 2, 172, "dauert nur wenige Sekunden", C_TEXT_DIM);
}

// ------------------------------------------------------------------ Ergebnis
static const box_t RES_CLOSE = {280, 0, 40, 34};
static const box_t RES_PREV = {6, 208, 46, 28};
static const box_t RES_LIVE = {58, 208, 92, 28};
static const box_t RES_AGAIN = {156, 208, 108, 28};
static const box_t RES_NEXT = {270, 208, 44, 28};

static void result_frame(app_t *a) {
    canvas_t *c = a->c;
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(c, 0, 0, SCREEN_W, HEADER_H, C_PANEL);
    gfx_fill(c, 0, HEADER_H - 1, SCREEN_W, 1, C_PANEL_EDGE);
    int x = gfx_text(c, &FONT_LABEL, 10, 19, "Auswertung", C_TEXT, OPAQUE);
    gfx_text(c, &FONT_SMALL, x + 8, 18, PAGE_NAMES[a->page], C_TEXT_DIM, OPAQUE);
    for (int i = 0; i < APP_PAGES; i++) {  // Seitenpunkte
        float cx = 236.0f + (float)i * 9.0f;
        gfx_disc(c, cx, 13.0f, i == a->page ? 3.2f : 2.4f, i == a->page ? C_ACCENT : C_TEXT_FAINT,
                 OPAQUE);
    }
    close_icon(c, 300, 13, C_TEXT_DIM);

    gfx_fill(c, 0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_PANEL);
    gfx_fill(c, 0, FOOTER_Y, SCREEN_W, 1, C_PANEL_EDGE);
    if (a->touch_xy) {
        button(c, RES_PREV, "‹", false, a->page > 0);
        button(c, RES_LIVE, "Live", false, true);
        button(c, RES_AGAIN, "Neu messen", true, true);
        button(c, RES_NEXT, "›", false, a->page < APP_PAGES - 1);
    } else {
        text_center(c, &FONT_SMALL, SCREEN_W / 2, 226, "Tippen: nächste Seite · Halten: Live",
                    C_TEXT_DIM);
    }
}

static void label_value(canvas_t *c, int x, int y, const char *label, const char *value,
                        const char *unit) {
    gfx_text(c, &FONT_SMALL, x, y + 14, label, C_TEXT_DIM, OPAQUE);
    int e = gfx_text(c, &FONT_VALUE, x, y + 40, value, C_TEXT, OPAQUE);
    if (unit && *unit) {
        gfx_text(c, &FONT_SMALL, e + 4, y + 40, unit, C_TEXT_DIM, OPAQUE);
    }
}

static const char *quality_line(const ana_result_t *r, uint32_t *col) {
    *col = C_OK;
    if (r->has_mains_ratio && r->mains_ratio > 0.5) {
        *col = C_ACCENT;
        return "sehr starker 50-Hz-Brumm";
    }
    if (r->masked_s > 10.0) {
        *col = C_WARN;
        return "größere Abschnitte ausgeblendet";
    }
    if (r->has_mains_ratio && r->mains_ratio > 0.1) {
        *col = C_WARN;
        return "50-Hz-Brumm erhöht";
    }
    if (r->masked_s > 1.0) {
        *col = C_WARN;
        return "kurze Abschnitte ausgeblendet";
    }
    return "gut";
}

// kurze Fassung eines Rhythmus-Hinweises für die Übersicht (voll auf der Seite „Hinweise“)
static const char *short_hint(const char *h) {
    if (strncmp(h, "Herzfrequenz schwankt stark", 27) == 0) {
        return "Herzfrequenz schwankt gleichmäßig mit der Atmung – in Ruhe normal";
    }
    if (strncmp(h, "Rhythmus über weite Strecken", 29) == 0) {
        return "Über weite Strecken zufällig unregelmäßig – ärztlich abklären";
    }
    return h;
}

static void page_overview(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    char buf[64], b2[32];
    if (r->has_hrv) {
        fmt_num(buf, sizeof buf, r->hrv.mean_hr, 0);
        int e = gfx_text(c, &FONT_HR, 12, 92, buf, C_TEXT, OPAQUE);
        gfx_text(c, &FONT_SMALL, e + 6, 92, "/min Ø", C_TEXT_DIM, OPAQUE);
        fmt_num(buf, sizeof buf, r->hrv.min_hr, 0);
        fmt_num(b2, sizeof b2, r->hrv.max_hr, 0);
        char line[128];
        snprintf(line, sizeof line, "min %s · max %s /min", buf, b2);
        gfx_text(c, &FONT_SMALL, 14, 114, line, C_TEXT_DIM, OPAQUE);
    } else {
        gfx_text(c, &FONT_HR, 12, 92, "– –", C_TEXT_FAINT, OPAQUE);
        gfx_text(c, &FONT_SMALL, 14, 114, "zu wenige gültige Schläge", C_TEXT_DIM, OPAQUE);
    }
    fmt_duration(buf, sizeof buf, r->duration_s);
    gfx_text(c, &FONT_SMALL, 200, 48, "Dauer", C_TEXT_DIM, OPAQUE);
    gfx_text(c, &FONT_LABEL, 200, 68, buf, C_TEXT, OPAQUE);
    snprintf(buf, sizeof buf, "%d", r->n_beats);
    gfx_text(c, &FONT_SMALL, 200, 90, "Schläge", C_TEXT_DIM, OPAQUE);
    gfx_text(c, &FONT_LABEL, 200, 110, buf, C_TEXT, OPAQUE);

    gfx_fill(c, 12, 124, SCREEN_W - 24, 1, C_PANEL_EDGE);
    gfx_text(c, &FONT_SMALL, 14, 142, "Rhythmus", C_TEXT_DIM, OPAQUE);
    if (a->result_test) {
        gfx_text(c, &FONT_SMALL, 14, 160, "Testsignal – kein Mensch", C_WARN, OPAQUE);
    } else if (r->n_hints) {
        int lines = gfx_text_wrap(c, &FONT_SMALL, 14, 160, SCREEN_W - 28, 16,
                                  short_hint(r->hints[0]), C_TEXT, 2);
        if (r->n_hints > 1 && lines < 2) {
            char more[48];
            snprintf(more, sizeof more, "+%d weitere (Seite Hinweise)", r->n_hints - 1);
            gfx_text(c, &FONT_SMALL, 14, 176, more, C_TEXT_FAINT, OPAQUE);
        }
    } else if (r->n_beats >= 20) {
        gfx_text(c, &FONT_SMALL, 14, 160, "Keine Auffälligkeiten erkannt", C_OK, OPAQUE);
    } else {
        gfx_text(c, &FONT_SMALL, 14, 160, "zu kurz für eine Aussage", C_TEXT_DIM, OPAQUE);
    }
    uint32_t col;
    const char *q = quality_line(r, &col);
    gfx_disc(c, 18.0f, 191.0f, 4.0f, col, OPAQUE);
    snprintf(buf, sizeof buf, "Signal: %s", q);
    gfx_text(c, &FONT_SMALL, 28, 196, buf, C_TEXT, OPAQUE);
}

static void page_trend(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    const int px0 = 44, px1 = 310, py0 = 38, py1 = 170;
    if (r->n_beats < 3) {
        text_center(c, &FONT_SMALL, SCREEN_W / 2, 110, "zu wenige Schläge", C_TEXT_DIM);
        return;
    }
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i + 1 < r->n_beats; i++) {
        double rr = (r->r_index[i + 1] - r->r_index[i]) * 1000.0 / ANA_FS;
        if (r->rr_ok[i] || !r->has_hrv) {
            lo = rr < lo ? rr : lo;
            hi = rr > hi ? rr : hi;
        }
    }
    if (lo > hi) {
        lo = 600;
        hi = 1200;
    }
    lo = floor(lo / 100.0) * 100.0 - 50.0;
    hi = ceil(hi / 100.0) * 100.0 + 50.0;
    const double tmax = r->duration_s > 1 ? r->duration_s : 1;
    // Raster: Zeit alle 30 s, RR alle 100 ms
    for (int s = 0; s <= (int)tmax; s += 30) {
        int x = px0 + (int)((px1 - px0) * s / tmax);
        gfx_fill(c, x, py0, 1, py1 - py0, C_GRID);
        char b[16];
        snprintf(b, sizeof b, "%d", s);
        text_center(c, &FONT_SMALL, x, py1 + 15, b, C_TEXT_FAINT);
    }
    for (int v = (int)lo + 50; v < hi; v += 100) {
        int y = py1 - (int)((py1 - py0) * (v - lo) / (hi - lo));
        gfx_fill(c, px0, y, px1 - px0, 1, C_GRID);
        char b[16];
        snprintf(b, sizeof b, "%d", v);
        gfx_text(c, &FONT_SMALL, px0 - 6 - gfx_text_width(&FONT_SMALL, b), y + 5, b,
                 C_TEXT_FAINT, OPAQUE);
    }
    float prev_x = 0, prev_y = 0;
    bool have = false;
    for (int i = 0; i + 1 < r->n_beats; i++) {
        double rr = (r->r_index[i + 1] - r->r_index[i]) * 1000.0 / ANA_FS;
        double t = r->r_index[i + 1] / ANA_FS;
        float x = (float)(px0 + (px1 - px0) * t / tmax);
        float y = (float)(py1 - (py1 - py0) * (rr - lo) / (hi - lo));
        bool ok = !r->has_hrv || r->rr_ok[i];
        if (y < py0 || y > py1) {
            have = false;
            continue;
        }
        if (ok) {
            if (have) {
                gfx_line(c, prev_x, prev_y, x, y, 1.6f, C_TRACE, OPAQUE);
            }
            prev_x = x;
            prev_y = y;
            have = true;
        } else {
            gfx_disc(c, x, y, 2.6f, C_ACCENT, OPAQUE);
            have = false;
        }
    }
    gfx_text(c, &FONT_SMALL, 10, 200, "RR (ms) über Zeit (s)", C_TEXT_FAINT, OPAQUE);
    gfx_line(c, 160.0f, 196.0f, 172.0f, 196.0f, 2.0f, C_TRACE, OPAQUE);
    int e = gfx_text(c, &FONT_SMALL, 176, 200, "gültig", C_TEXT_DIM, OPAQUE);
    gfx_disc(c, (float)e + 12.0f, 196.0f, 2.6f, C_ACCENT, OPAQUE);
    gfx_text(c, &FONT_SMALL, e + 19, 200, "verworfen", C_TEXT_DIM, OPAQUE);
}

static void page_hrv(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    if (!r->has_hrv) {
        text_center(c, &FONT_SMALL, SCREEN_W / 2, 110, "zu wenige gültige Schläge für die HRV",
                    C_TEXT_DIM);
        return;
    }
    const ana_hrv_t *h = &r->hrv;
    char v[32], v2[16];
    fmt_num(v, sizeof v, h->rmssd_ms, 0);
    label_value(c, 14, 30, "RMSSD", v, "ms");
    fmt_num(v, sizeof v, h->sdnn_ms, 0);
    label_value(c, 166, 30, "SDNN", v, "ms");
    fmt_num(v, sizeof v, h->pnn50, 1);
    label_value(c, 14, 82, "pNN50", v, "%");
    fmt_num(v, sizeof v, h->sd1_ms, 0);
    fmt_num(v2, sizeof v2, h->sd2_ms, 0);
    char sd[64];
    snprintf(sd, sizeof sd, "%s / %s", v, v2);
    label_value(c, 166, 82, "SD1 / SD2", sd, "ms");
    if (h->has_lf_hf) {
        fmt_num(v, sizeof v, h->lf_hf, 2);
    } else {
        snprintf(v, sizeof v, "–");
    }
    label_value(c, 14, 134, "LF / HF", v, "");
    if (h->has_peak) {
        fmt_num(v, sizeof v, h->peak_hz * 60.0, 0);
        label_value(c, 166, 134, "Stärkste Schwankung", v, "/min");
    } else {
        label_value(c, 166, 134, "Stärkste Schwankung", "–", "");
    }
    const char *note = h->duration_s < 120.0 ? "Kürzer als 2 min: HRV nur grob, LF/HF kaum deutbar."
                                             : "LF/HF erst ab 5 min belastbar; hier nur grob.";
    gfx_text(c, &FONT_SMALL, 14, 198, note, C_TEXT_FAINT, OPAQUE);
}

static void marker(canvas_t *c, float x, int y0, int y1, const char *label, uint32_t col) {
    for (int y = y0; y < y1; y += 4) {
        gfx_line(c, x, (float)y, x, (float)y + 2.0f, 1.2f, col, 200);
    }
    text_center(c, &FONT_SMALL, (int)x, y0 - 2, label, col);
}

static void page_beat(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    if (!r->has_median) {
        text_center(c, &FONT_SMALL, SCREEN_W / 2, 110, "kein Median-Schlag (zu wenige Schläge)",
                    C_TEXT_DIM);
        return;
    }
    const int px0 = 10, px1 = 196, py0 = 48, py1 = 184;
    float lo = r->tmpl[0], hi = r->tmpl[0];
    for (int i = 1; i < ANA_TMPL_LEN; i++) {
        lo = r->tmpl[i] < lo ? r->tmpl[i] : lo;
        hi = r->tmpl[i] > hi ? r->tmpl[i] : hi;
    }
    float pad = (hi - lo) * 0.08f + 0.02f;
    lo -= pad;
    hi += pad;
    const float sx = (float)(px1 - px0) / (float)(ANA_TMPL_LEN - 1);
    const float sy = (float)(py1 - py0) / (hi - lo);
    for (int k = 0; k < ANA_TMPL_LEN; k += ANA_FS / 10) {  // 100-ms-Raster
        gfx_fill(c, px0 + (int)((float)k * sx), py0, 1, py1 - py0, C_GRID);
    }
    if (r->has_delineation) {
        float yb = (float)py1 - (float)(r->d.baseline_mv - lo) * sy;
        gfx_fill(c, px0, (int)yb, px1 - px0, 1, C_GRID);
    }
    for (int i = 1; i < ANA_TMPL_LEN; i++) {
        float x0 = (float)px0 + (float)(i - 1) * sx, x1 = (float)px0 + (float)i * sx;
        float y0 = (float)py1 - (r->tmpl[i - 1] - lo) * sy;
        float y1 = (float)py1 - (r->tmpl[i] - lo) * sy;
        gfx_line(c, x0, y0, x1, y1, 1.8f, C_TRACE, OPAQUE);
    }
    char v[24];
    const ana_delineation_t *d = &r->d;
    if (r->has_delineation) {
        if (d->p_onset >= 0) {
            marker(c, (float)px0 + (float)d->p_onset * sx, py0, py1, "P", C_WARN);
        }
        marker(c, (float)px0 + (float)d->qrs_onset * sx, py0, py1, "Q", C_ACCENT);
        marker(c, (float)px0 + (float)d->qrs_offset * sx, py0, py1, "S", C_ACCENT);
        if (d->t_end >= 0) {
            marker(c, (float)px0 + (float)d->t_end * sx, py0, py1, "T", C_OK);
        }
    }
    const int vx = 208;
    const struct {
        const char *label;
        double v;
    } rows[] = {
        {"PR", r->has_delineation ? d->pr_ms : NAN},
        {"QRS", r->has_delineation ? d->qrs_ms : NAN},
        {"QT", r->has_delineation ? d->qt_ms : NAN},
        {"QTc Fridericia", r->has_delineation ? d->qtc_fridericia_ms : NAN},
    };
    for (int i = 0; i < 4; i++) {
        int y = 32 + i * 42;
        gfx_text(c, &FONT_SMALL, vx, y + 14, rows[i].label, C_TEXT_DIM, OPAQUE);
        fmt_num(v, sizeof v, rows[i].v, 0);
        int e = gfx_text(c, &FONT_LABEL, vx, y + 34, v, C_TEXT, OPAQUE);
        if (isfinite(rows[i].v)) {
            gfx_text(c, &FONT_SMALL, e + 4, y + 34, "ms", C_TEXT_DIM, OPAQUE);
        }
    }
    char n[48];
    snprintf(n, sizeof n, "%d Schläge · experimentell", r->median_beats);
    gfx_text(c, &FONT_SMALL, px0, 200, n, C_TEXT_FAINT, OPAQUE);
}

static void page_notes(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    int y = 46;
    const int max_y = 182;
    const char *texts[ANA_MAX_HINTS + ANA_MAX_QNOTES + 2];
    int nt = 0;
    if (a->result_test) {
        texts[nt++] = "Testsignal vom Laptop – kein Mensch gemessen.";
    }
    for (int i = 0; i < r->n_hints; i++) {
        texts[nt++] = r->hints[i];
    }
    for (int i = 0; i < r->n_qnotes; i++) {
        texts[nt++] = r->qnotes[i];
    }
    if (nt == 0) {
        texts[nt++] = "Keine Hinweise zu Rhythmus und Signal.";
    }
    for (int i = 0; i < nt && y <= max_y; i++) {
        int room = (max_y - y) / 16 + 1;
        gfx_disc(c, 17.0f, (float)y - 4.0f, 2.5f, C_ACCENT, OPAQUE);
        int lines = gfx_text_wrap(c, &FONT_SMALL, 26, y, SCREEN_W - 40, 16, texts[i], C_TEXT,
                                  room);
        y += lines * 16 + 6;
    }
    gfx_text(c, &FONT_SMALL, 14, 198, "Hinweise, keine Diagnose.", C_TEXT_FAINT, OPAQUE);
}

static void draw_result(app_t *a) {
    result_frame(a);
    if (a->result_status != ANA_OK) {
        const char *why = a->result_status == ANA_TOO_SHORT       ? "Messung zu kurz (mind. 30 s)."
                          : a->result_status == ANA_TOO_MANY_GAPS ? "Zu viele Störungen im Signal."
                                                                  : "Nicht genug Arbeitsspeicher.";
        text_center(a->c, &FONT_LABEL, SCREEN_W / 2, 100, "Keine Auswertung möglich", C_TEXT);
        text_center(a->c, &FONT_SMALL, SCREEN_W / 2, 124, why, C_TEXT_DIM);
        return;
    }
    switch (a->page) {
        case 0:
            page_overview(a);
            break;
        case 1:
            page_trend(a);
            break;
        case 2:
            page_hrv(a);
            break;
        case 3:
            page_beat(a);
            break;
        default:
            page_notes(a);
            break;
    }
}

// ------------------------------------------------------------------ Touch einrichten
static void draw_cal(app_t *a) {
    canvas_t *c = a->c;
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    int s = a->cal_step;
    float x = (float)CAL_X[s], y = (float)CAL_Y[s];
    gfx_line(c, x - 14.0f, y, x + 14.0f, y, 1.6f, C_ACCENT, OPAQUE);
    gfx_line(c, x, y - 14.0f, x, y + 14.0f, 1.6f, C_ACCENT, OPAQUE);
    gfx_round_frame(c, (int)x - 7, (int)y - 7, 14, 14, 7, 1.5f, C_ACCENT);
    text_center(c, &FONT_LABEL, SCREEN_W / 2, 84, "Touch einrichten", C_TEXT);
    text_center(c, &FONT_SMALL, SCREEN_W / 2, 160, "Genau auf die Mitte des Kreuzes tippen –",
                C_TEXT_DIM);
    text_center(c, &FONT_SMALL, SCREEN_W / 2, 176, "am besten mit Fingernagel oder Stift.",
                C_TEXT_DIM);
    char b[32];
    snprintf(b, sizeof b, "Punkt %d von 5", s + 1);
    text_center(c, &FONT_SMALL, SCREEN_W / 2, 196, b, C_TEXT_FAINT);
}

static void start_cal(app_t *a) {
    a->screen = APP_CAL;
    a->mon->draw = false;
    a->cal_step = 0;
    a->cal_bad = 0;
    draw_cal(a);
    a->req_push_all = true;
}

static void finish_cal(app_t *a, uint32_t now) {
    float sx[5], sy[5];
    for (int i = 0; i < 5; i++) {  // Ziele für Drehung 0 umrechnen
        sx[i] = (float)(a->ui->rotation ? SCREEN_W - 1 - CAL_X[i] : CAL_X[i]);
        sy[i] = (float)(a->ui->rotation ? SCREEN_H - 1 - CAL_Y[i] : CAL_Y[i]);
    }
    touch_cal_t cal;
    float err = 1e9f;
    if (touchcal_solve(sx, sy, a->cal_rx, a->cal_ry, 5, &cal, &err) && err < 24.0f) {
        a->ui->cal = cal;
        a->ui->has_cal = true;
        a->touch_xy = true;
        a->req_save = true;
        go_live(a);
        show_toast(a, "Touch eingerichtet. Tippen öffnet das Menü.", now);
    } else {
        a->cal_step = 0;
        draw_cal(a);
        a->req_push_all = true;
        show_toast(a, "Zu ungenau – bitte noch einmal, genau auf die Kreuzmitte.", now);
    }
}

// ------------------------------------------------------------------ Info
static void draw_info(app_t *a) {
    canvas_t *c = a->c;
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(c, 0, 0, SCREEN_W, HEADER_H, C_PANEL);
    gfx_fill(c, 0, HEADER_H - 1, SCREEN_W, 1, C_PANEL_EDGE);
    gfx_text(c, &FONT_LABEL, 10, 19, "Info", C_TEXT, OPAQUE);
    close_icon(c, 300, 13, C_TEXT_DIM);
    char b[96];
    int y = 52;
    snprintf(b, sizeof b, "SophiEKG %s", a->version ? a->version : "");
    gfx_text(c, &FONT_LABEL, 14, y, b, C_TEXT, OPAQUE);
    y += 26;
    snprintf(b, sizeof b, "Display: %s (Kennung 0x%04lx)",
             a->display_id == 0x9341u ? "ILI9341" : "unbekannt", (unsigned long)a->display_id);
    gfx_text(c, &FONT_SMALL, 14, y, b, C_TEXT_DIM, OPAQUE);
    y += 20;
    gfx_text(c, &FONT_SMALL, 14, y,
             a->touch_xy ? "Touch: eingerichtet" : "Touch: nur Tippen (keine Koordinaten)",
             C_TEXT_DIM, OPAQUE);
    y += 20;
    if (a->usb) {
        snprintf(b, sizeof b, "Strom: USB%s", a->streaming ? ", Laptop empfängt" : "");
    } else if (a->battery_pct >= 0) {
        char v[16];
        fmt_num(v, sizeof v, a->vsys_ema / 1000.0, 2);
        snprintf(b, sizeof b, "Strom: Batterie %s V (%d %%)", v, a->battery_pct);
    } else {
        snprintf(b, sizeof b, "Strom: Batterie (Spannung nicht messbar)");
    }
    gfx_text(c, &FONT_SMALL, 14, y, b, C_TEXT_DIM, OPAQUE);
    y += 20;
    char dur[16];
    fmt_duration(dur, sizeof dur, ANA_MAX_S);
    snprintf(b, sizeof b, "Messung: %s, Auswertung wie am Laptop", dur);
    gfx_text(c, &FONT_SMALL, 14, y, b, C_TEXT_DIM, OPAQUE);
    y += 30;
    gfx_text_wrap(c, &FONT_SMALL, 14, y, SCREEN_W - 28, 16,
                  "Ein Kanal, 0,5–40 Hz: keine ST-Beurteilung. Hinweise sind keine Diagnose.",
                  C_TEXT_FAINT, 3);
}

// ------------------------------------------------------------------ Messung
static void start_measure(app_t *a) {
    ana_rec_clear(a->rec);
    a->measuring = true;
    a->measure_full = false;
    a->meas_usb_seen = a->usb;
    a->meas_test_blocks = 0;
    a->have_seq = false;
    monitor_set_measure(a->mon, true, 0, ANA_MAX_S);
    go_live(a);
}

static void stop_measure(app_t *a, bool analyze, uint32_t now) {
    a->measuring = false;
    monitor_set_measure(a->mon, false, 0, ANA_MAX_S);
    if (analyze && ana_rec_seconds(a->rec) >= ANA_MIN_S) {
        a->screen = APP_ANALYZING;
        a->mon->draw = false;
        draw_analyzing(a);
        a->analyze_in = 1;
        a->req_push_all = true;
        return;
    }
    go_live(a);
    if (analyze) {
        show_toast(a, "Messung zu kurz – mindestens 30 s, besser die vollen 2 Minuten.", now);
    }
}

static void run_analysis(app_t *a) {
    ana_power_t power = a->meas_usb_seen ? ANA_POWER_UNKNOWN : ANA_POWER_BATTERY;
    a->result_test = a->meas_test_blocks * 2 > (int)a->rec->n_blocks;
    a->result_status = ana_run(a->rec, power, &a->mon->qrs, a->scratch, a->scratch_size, a->res);
    monitor_relearn(a->mon);  // der Detektor wurde für die Auswertung geliehen
    a->has_result = a->result_status == ANA_OK;
    a->screen = APP_RESULT;
    a->page = 0;
    draw_result(a);
    a->req_push_all = true;
}

// ------------------------------------------------------------------ Bildaufbau
static void redraw(app_t *a) {
    switch (a->screen) {
        case APP_LIVE:
            monitor_draw_all(a->mon);
            break;
        case APP_MENU:
            draw_menu(a, true);
            break;
        case APP_ANALYZING:
            draw_analyzing(a);
            break;
        case APP_RESULT:
            draw_result(a);
            break;
        case APP_CAL:
            draw_cal(a);
            break;
        case APP_INFO:
            draw_info(a);
            break;
    }
    if (a->toast_shown) {
        draw_toast(a);
    }
    a->req_push_all = true;
}

// ------------------------------------------------------------------ Schnittstelle
void app_init(app_t *a) {
    a->screen = APP_LIVE;
    a->battery_pct = -1;
    a->req_backlight = -1;
    a->result_status = ANA_OK;
    a->has_result = false;
    apply_look(a);
}

void app_start(app_t *a, bool touch_hw_ok, uint32_t now) {
    (void)now;
    a->touch_xy = a->ui->has_cal;
    if (!a->ui->has_cal && touch_hw_ok) {
        start_cal(a);
    } else {
        go_live(a);
    }
}

void app_feed(app_t *a, const uint32_t *raw, int n, uint8_t flags, uint32_t factor,
              uint32_t seq) {
    monitor_feed(a->mon, raw, n, flags, factor);
    if (a->measuring && !a->measure_full) {
        if (a->have_seq) {
            uint32_t missing = seq - a->last_seq - 1u;
            for (uint32_t k = 0; k < missing && k < 100u; k++) {  // Display kam nicht nach
                if (!ana_rec_add_gap(a->rec)) {
                    a->measure_full = true;
                    break;
                }
            }
        }
        uint32_t r64[ANA_BLOCK];
        int m = 0;
        if (factor <= 32) {  // 1000 Hz: paarweise zu 64-fachen Summen
            for (int i = 0; i + 1 < n && m < ANA_BLOCK; i += 2) {
                r64[m++] = raw[i] + raw[i + 1];
            }
        } else {
            for (int i = 0; i < n && m < ANA_BLOCK; i++) {
                r64[m++] = raw[i];
            }
        }
        if (m == ANA_BLOCK && !a->measure_full && !ana_rec_add(a->rec, r64, m, flags)) {
            a->measure_full = true;
        }
        if (a->rec->n_blocks >= ANA_MAX_BLOCKS) {
            a->measure_full = true;
        }
        a->meas_test_blocks += (flags & EKG_FLAG_TEST) ? 1 : 0;
        a->meas_usb_seen |= a->usb;
    }
    a->last_seq = seq;
    a->have_seq = true;
}

int app_battery_percent(int vsys_mv) {
    if (vsys_mv < 2500 || vsys_mv > 5600) {
        return -1;  // kein Messteiler oder unplausibel
    }
    // drei Alkali-Zellen unter Last: Spannung je Zelle -> Restladung
    static const int MV[] = {1000, 1100, 1200, 1300, 1450};
    static const int PCT[] = {0, 13, 40, 73, 100};
    int cell = vsys_mv / 3;
    if (cell <= MV[0]) {
        return 0;
    }
    if (cell >= MV[4]) {
        return 100;
    }
    for (int i = 1; i < 5; i++) {
        if (cell <= MV[i]) {
            return PCT[i - 1] + (PCT[i] - PCT[i - 1]) * (cell - MV[i - 1]) / (MV[i] - MV[i - 1]);
        }
    }
    return 100;
}

void app_set_status(app_t *a, bool usb, bool streaming, int vsys_mv) {
    a->usb = usb;
    a->streaming = streaming;
    if (vsys_mv != a->vsys_mv && vsys_mv > 0) {
        a->vsys_ema = a->vsys_ema > 0 ? 0.7f * a->vsys_ema + 0.3f * (float)vsys_mv : (float)vsys_mv;
    }
    a->vsys_mv = vsys_mv;
    int pct = usb || a->vsys_ema <= 0 ? -1 : app_battery_percent((int)a->vsys_ema);
    if (pct >= 0) {
        pct = (pct + 2) / 5 * 5;  // in 5-%-Schritten, sonst springt die Anzeige
    }
    a->battery_pct = pct;
    monitor_set_link(a->mon, usb, streaming);
    monitor_set_battery(a->mon, pct);
}

static void next_look(app_t *a) {
    a->ui->look = (uint8_t)((a->ui->look + 1) % N_LOOKS);
    apply_look(a);
    a->req_save = true;
    redraw(a);
}

void app_calibrate(app_t *a) {
    if (a->measuring) {
        a->measuring = false;
        monitor_set_measure(a->mon, false, 0, ANA_MAX_S);
    }
    start_cal(a);
}

void app_event(app_t *a, ui_event_t ev, uint32_t now) {
    if (ev.type == EV_NONE) {
        return;
    }
    const bool tap = ev.type == EV_TAP;
    switch (a->screen) {
        case APP_LIVE:
            if (!a->touch_xy) {  // ohne Koordinaten: Tippen = Anzeige, Halten = Messung
                if (tap) {
                    next_look(a);
                } else if (ev.type == EV_LONG) {
                    if (a->measuring) {
                        stop_measure(a, true, now);
                    } else {
                        start_measure(a);
                    }
                }
            } else if (a->measuring) {
                if (tap && ev.x >= 236 && ev.y <= 40) {
                    stop_measure(a, true, now);
                }
            } else if (tap || ev.type == EV_LONG) {
                a->screen = APP_MENU;
                a->mon->draw = false;
                draw_menu(a, false);
                a->req_push_all = true;
            }
            break;
        case APP_MENU:
            if (!tap) {
                break;
            }
            if (hit(MENU_MEASURE, ev.x, ev.y)) {
                start_measure(a);
            } else if (hit(MENU_LAST, ev.x, ev.y) && (a->has_result || a->result_status)) {
                a->screen = APP_RESULT;
                a->page = 0;
                redraw(a);
            } else if (hit(MENU_LOOK, ev.x, ev.y)) {
                next_look(a);
            } else if (hit(MENU_ROTATE, ev.x, ev.y)) {
                a->ui->rotation ^= 1u;
                a->req_rotate = true;
                a->req_save = true;
                redraw(a);
            } else if (hit(MENU_CAL, ev.x, ev.y)) {
                start_cal(a);
            } else if (hit(MENU_INFO, ev.x, ev.y)) {
                a->screen = APP_INFO;
                redraw(a);
            } else if (hit(MENU_BACK, ev.x, ev.y) || hit(MENU_CLOSE, ev.x, ev.y) ||
                       !hit(MENU_PANEL, ev.x, ev.y)) {
                go_live(a);
            }
            break;
        case APP_RESULT:
            if (!a->touch_xy) {
                if (tap) {
                    a->page = (a->page + 1) % APP_PAGES;
                    redraw(a);
                } else if (ev.type == EV_LONG) {
                    go_live(a);
                }
                break;
            }
            if (ev.type == EV_SWIPE_LEFT || (tap && hit(RES_NEXT, ev.x, ev.y))) {
                if (a->page < APP_PAGES - 1) {
                    a->page++;
                    redraw(a);
                }
            } else if (ev.type == EV_SWIPE_RIGHT || (tap && hit(RES_PREV, ev.x, ev.y))) {
                if (a->page > 0) {
                    a->page--;
                    redraw(a);
                }
            } else if (tap && hit(RES_AGAIN, ev.x, ev.y)) {
                start_measure(a);
            } else if ((tap && (hit(RES_LIVE, ev.x, ev.y) || hit(RES_CLOSE, ev.x, ev.y))) ||
                       ev.type == EV_LONG) {
                go_live(a);
            }
            break;
        case APP_CAL:
            if (ev.type == EV_LONG) {  // abbrechen
                go_live(a);
                break;
            }
            if (!tap) {
                break;
            }
            if (!ev.has_pos) {
                if (++a->cal_bad >= 3) {
                    a->touch_xy = false;
                    go_live(a);
                    show_toast(a,
                               "Touch liefert keine Koordinaten (T_DO an GP12?). Bedienung: "
                               "Tippen = Anzeige, Halten = Messung.",
                               now);
                }
                break;
            }
            a->cal_rx[a->cal_step] = (float)ev.rx;
            a->cal_ry[a->cal_step] = (float)ev.ry;
            if (++a->cal_step >= 5) {
                finish_cal(a, now);
            } else {
                draw_cal(a);
                a->req_push_all = true;
            }
            break;
        case APP_INFO:
            if (tap || ev.type == EV_LONG) {
                a->screen = APP_MENU;
                redraw(a);
            }
            break;
        case APP_ANALYZING:
            break;
    }
}

int app_frame(app_t *a, uint32_t now, rect_t *dirty, int max_dirty) {
    if (a->toast_shown && (int32_t)(now - a->toast_until) >= 0) {
        a->toast_shown = false;
        redraw(a);
        return 0;
    }
    if (a->screen == APP_ANALYZING) {
        if (a->analyze_in > 0) {
            a->analyze_in--;
        } else {
            run_analysis(a);
        }
        return 0;
    }
    if (a->measuring) {
        monitor_set_measure(a->mon, true, (int)ana_rec_seconds(a->rec), ANA_MAX_S);
        if (a->measure_full) {
            stop_measure(a, true, now);
            return 0;
        }
    }
    if (!a->low_warned && a->battery_pct >= 0 && a->battery_pct <= 10) {
        a->low_warned = true;
        show_toast(a, "Batterien fast leer – bald tauschen.", now);
        return 0;
    }
    if (a->screen == APP_LIVE && !a->toast_shown) {
        return monitor_frame(a->mon, now, dirty, max_dirty);
    }
    return 0;
}
