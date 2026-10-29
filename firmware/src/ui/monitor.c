#include "monitor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "protocol.h"
#include "theme.h"

#define TOP_H 26
#define TRACE_Y0 TOP_H
#define TRACE_Y1 182
#define TRACE_H (TRACE_Y1 - TRACE_Y0)
#define TRACE_X0 30
#define TRACE_X1 SCREEN_W
#define TRACE_W (TRACE_X1 - TRACE_X0)
#define BASELINE_Y (TRACE_Y0 + TRACE_H * 3 / 5)
#define GRID 20
#define SAMPLES_PER_COL 6  // 500 Hz -> 83 Pixel/s, 3,5 s je Durchlauf
#define ERASE_GAP 12
#define BOTTOM_Y TRACE_Y1

#define VREF_V 3.3
#define GAIN 1100.0
#define ADC_STEPS 4096.0

static const float GAIN_STEPS[] = {15, 20, 30, 40, 60, 80, 120, 160};  // Pixel je mV

// ------------------------------------------------------------------ Hintergrund
static bool grid_at(int x, int y) {  // gepunktetes Raster
    return ((x - TRACE_X0) % GRID == 0 && (y & 1) == 0) ||
           ((y - BASELINE_Y) % GRID == 0 && (x & 1) == 0);
}

static void clear_column(monitor_t *m, int x) {
    if (x < TRACE_X0 || x >= TRACE_X1) {
        return;
    }
    uint16_t bg = rgb565(C_BG), grid = rgb565(C_GRID);
    for (int y = TRACE_Y0; y < TRACE_Y1; y++) {
        m->c->px[y * m->c->w + x] = grid_at(x, y) ? grid : bg;
    }
}

static void draw_trace_bg(monitor_t *m) {
    for (int x = TRACE_X0; x < TRACE_X1; x++) {
        clear_column(m, x);
    }
    gfx_fill(m->c, 0, TRACE_Y0, TRACE_X0, TRACE_H, C_BG);
}

// Eichzacke links: 1 mV, bei starker Verstärkung 0,5 mV, damit sie in die Fläche passt
static void draw_calibration(monitor_t *m) {
    gfx_fill(m->c, 0, TRACE_Y0, TRACE_X0, TRACE_H, C_BG);
    bool half = m->px_per_mv > 80.0f;
    float b = (float)BASELINE_Y;
    float top = b - m->px_per_mv * (half ? 0.5f : 1.0f);
    gfx_line(m->c, 5, b, 10, b, 1.5f, C_TEXT_DIM, OPAQUE);
    gfx_line(m->c, 10, b, 10, top, 1.5f, C_TEXT_DIM, OPAQUE);
    gfx_line(m->c, 10, top, 18, top, 1.5f, C_TEXT_DIM, OPAQUE);
    gfx_line(m->c, 18, top, 18, b, 1.5f, C_TEXT_DIM, OPAQUE);
    gfx_line(m->c, 18, b, 24, b, 1.5f, C_TEXT_DIM, OPAQUE);
    const char *label = half ? "0,5" : "1";
    int w = gfx_text_width(&FONT_SMALL, label);
    gfx_text(m->c, &FONT_SMALL, 14 - w / 2, (int)top - 6, label, C_TEXT_DIM, OPAQUE);
    gfx_text(m->c, &FONT_SMALL, 14 - gfx_text_width(&FONT_SMALL, "mV") / 2, (int)b + 16, "mV",
             C_TEXT_FAINT, OPAQUE);
    m->shown_px_per_mv = m->px_per_mv;
}

// ------------------------------------------------------------------ Kopfleiste
static void draw_chip(monitor_t *m, int *right, const char *text, uint32_t col) {
    int w = gfx_text_width(&FONT_SMALL, text) + 14;
    int x = *right - w;
    gfx_round_frame(m->c, x, 5, w, 17, 8, 1.2f, col);
    gfx_text(m->c, &FONT_SMALL, x + 7, 18, text, col, OPAQUE);
    *right = x - 6;
}

// Batteriesymbol mit Füllstand; liefert die linke Kante
static int draw_battery(monitor_t *m, int right) {
    char buf[8];
    snprintf(buf, sizeof buf, "%d %%", m->battery_pct);
    uint32_t col = m->battery_pct <= 10 ? C_ACCENT : m->battery_pct <= 20 ? C_WARN : C_TEXT_DIM;
    const int bw = 20, bh = 11, by = 8;
    int bx = right - bw - 3;
    gfx_round_frame(m->c, bx, by, bw, bh, 3, 1.3f, col);
    gfx_round_rect(m->c, bx + bw, by + 3, 3, bh - 6, 1, col, OPAQUE);
    int fill = (bw - 4) * m->battery_pct / 100;
    if (fill > 0) {
        uint32_t fc = m->battery_pct <= 20 ? col : C_OK;
        gfx_round_rect(m->c, bx + 2, by + 2, fill, bh - 4, 1, fc, OPAQUE);
    }
    int tw = gfx_text_width(&FONT_SMALL, buf);
    gfx_text(m->c, &FONT_SMALL, bx - 5 - tw, 18, buf, col, OPAQUE);
    return bx - 5 - tw - 8;
}

static void draw_menu_icon(monitor_t *m, int x, int y) {
    for (int k = 0; k < 3; k++) {
        float yy = (float)(y + k * 5) + 0.5f;
        gfx_line(m->c, (float)x, yy, (float)x + 14.0f, yy, 2.0f, C_TEXT_DIM, OPAQUE);
    }
}

static void draw_top(monitor_t *m) {
    gfx_fill(m->c, 0, 0, SCREEN_W, TOP_H, C_PANEL);
    gfx_fill(m->c, 0, TOP_H - 1, SCREEN_W, 1, C_PANEL_EDGE);
    int right = SCREEN_W - 8;
    if (m->measuring) {
        // Messung: Zeit, Fortschritt und Stopp-Knopf
        char buf[32];
        int e = m->meas_elapsed_s, t = m->meas_total_s;
        gfx_disc(m->c, 12.5f, 13.0f, 4.5f, C_ACCENT, (e % 2) ? 120 : OPAQUE);
        int x = gfx_text(m->c, &FONT_LABEL, 24, 19, "Messung", C_TEXT, OPAQUE);
        snprintf(buf, sizeof buf, "%d:%02d / %d:%02d", e / 60, e % 60, t / 60, t % 60);
        gfx_text(m->c, &FONT_SMALL, x + 8, 18, buf, C_TEXT_DIM, OPAQUE);
        const int bw = 62;
        gfx_round_rect(m->c, SCREEN_W - bw - 5, 3, bw, 19, 9, C_ACCENT, OPAQUE);
        const char *stop = "Stopp";
        gfx_text(m->c, &FONT_SMALL, SCREEN_W - 5 - bw / 2 - gfx_text_width(&FONT_SMALL, stop) / 2,
                 17, stop, 0xFFFFFFu, OPAQUE);
        int w = t > 0 ? SCREEN_W * (e < t ? e : t) / t : 0;
        gfx_fill(m->c, 0, TOP_H - 2, w, 2, C_ACCENT);
        right = SCREEN_W - bw - 12;
        if (m->battery_pct >= 0) {
            right = draw_battery(m, right);
        }
    } else {
        gfx_image_hgrad(m->c, &IMG_HEART_14, 9, 6, 0, IMG_HEART_14.w, C_ACCENT, C_ACCENT_2,
                        OPAQUE);
        int x = gfx_text(m->c, &FONT_LABEL, 30, 19, "Sophi", C_TEXT, OPAQUE);
        gfx_text(m->c, &FONT_LABEL, x, 19, "EKG", C_ACCENT, OPAQUE);
        if (m->menu_button) {
            draw_menu_icon(m, SCREEN_W - 22, 8);
            right = SCREEN_W - 32;
        }
        if (m->battery_pct >= 0) {
            right = draw_battery(m, right + 2);
        }
        if (m->test) {
            draw_chip(m, &right, "TEST", C_WARN);
        }
        if (m->streaming) {
            draw_chip(m, &right, "Laptop", C_OK);
        } else if (m->usb) {
            draw_chip(m, &right, "USB", C_TEXT_DIM);
        }
    }
    m->shown_test = m->test;
    m->shown_usb = m->usb;
    m->shown_streaming = m->streaming;
    m->shown_battery = m->battery_pct;
    m->shown_measuring = m->measuring;
    m->shown_elapsed = m->meas_elapsed_s;
    m->shown_menu_button = m->menu_button;
}

// ------------------------------------------------------------------ Fußleiste
static void draw_heart(monitor_t *m, bool big) {
    gfx_fill(m->c, 4, BOTTOM_Y + 4, 40, 50, C_PANEL);
    const alpha_img_t *h = big ? &IMG_HEART_30 : &IMG_HEART_26;
    int cx = 24, cy = BOTTOM_Y + 30;
    if (big && GLOW) {
        gfx_glow(m->c, (float)cx, (float)cy, 22.0f, C_ACCENT, 110);
    }
    gfx_image_hgrad(m->c, h, cx - h->w / 2, cy - h->h / 2, 0, h->w, C_ACCENT, C_ACCENT_2,
                    m->bpm >= 0 ? OPAQUE : 110);
    m->shown_big_heart = big;
}

static void draw_bpm(monitor_t *m) {
    gfx_fill(m->c, 46, BOTTOM_Y + 2, 120, 54, C_PANEL);
    char buf[12];
    int base = SCREEN_H - 10;
    const int right = 128;  // Zahl rechtsbündig, damit „/min“ stehen bleibt
    if (m->bpm >= 0) {
        snprintf(buf, sizeof buf, "%d", m->bpm);
        gfx_text(m->c, &FONT_HR, right - gfx_text_width(&FONT_HR, buf), base, buf, C_TEXT, OPAQUE);
        gfx_text(m->c, &FONT_SMALL, right + 4, base, "/min", C_TEXT_DIM, OPAQUE);
    } else {
        gfx_text(m->c, &FONT_HR, right - gfx_text_width(&FONT_HR, "– –"), base, "– –",
                 C_TEXT_FAINT, OPAQUE);
        gfx_text(m->c, &FONT_SMALL, right + 4, base, "/min", C_TEXT_FAINT, OPAQUE);
    }
    m->shown_bpm = m->bpm;
}

static void draw_status(monitor_t *m) {
    const int x0 = 170;
    gfx_fill(m->c, x0, BOTTOM_Y + 2, SCREEN_W - x0, 54, C_PANEL);
    const char *line1 = "";
    char line2[32] = "";
    uint32_t col = C_TEXT_DIM;
    switch (m->status) {
        case MON_LEAD_OFF:
            line1 = "Elektrode ab";
            snprintf(line2, sizeof line2, "Kontakt prüfen");
            col = C_ACCENT;
            break;
        case MON_CLIPPED:
            line1 = "Übersteuert";
            snprintf(line2, sizeof line2, "ruhig halten");
            col = C_WARN;
            break;
        case MON_LEARNING:
            line1 = "Lernt …";
            snprintf(line2, sizeof line2, "kurz stillhalten");
            break;
        case MON_SEARCHING:
            line1 = "Sucht Puls";
            snprintf(line2, sizeof line2, "Elektroden prüfen");
            break;
        case MON_GOOD:
            line1 = "Signal gut";
            if (m->rr_ms > 0) {
                snprintf(line2, sizeof line2, "RR %d ms", m->rr_ms);
            }
            col = C_OK;
            break;
    }
    gfx_disc(m->c, (float)x0 + 6.0f, (float)BOTTOM_Y + 19.0f, 4.0f, col, OPAQUE);
    gfx_text(m->c, &FONT_LABEL, x0 + 16, BOTTOM_Y + 25, line1, C_TEXT, OPAQUE);
    gfx_text(m->c, &FONT_SMALL, x0 + 16, BOTTOM_Y + 45, line2, C_TEXT_DIM, OPAQUE);
    m->shown_status = m->status;
    m->shown_rr = m->rr_ms;
}

static void draw_bottom(monitor_t *m) {
    gfx_fill(m->c, 0, BOTTOM_Y, SCREEN_W, SCREEN_H - BOTTOM_Y, C_PANEL);
    gfx_fill(m->c, 0, BOTTOM_Y, SCREEN_W, 1, C_PANEL_EDGE);
    draw_heart(m, false);
    draw_bpm(m);
    draw_status(m);
}

// ------------------------------------------------------------------ Hinweis „Elektrode ab“
#define OV_W 196
#define OV_H 66
#define OV_X (TRACE_X0 + (TRACE_W - OV_W) / 2)
#define OV_Y (TRACE_Y0 + 40)

static void draw_overlay(monitor_t *m) {
    gfx_round_rect(m->c, OV_X, OV_Y, OV_W, OV_H, 12, C_PANEL, 235);
    gfx_round_frame(m->c, OV_X, OV_Y, OV_W, OV_H, 12, 1.5f, C_ACCENT);
    const char *t1 = "Elektrode ab", *t2 = "Sitz und Gel prüfen";
    gfx_text(m->c, &FONT_LABEL, OV_X + OV_W / 2 - gfx_text_width(&FONT_LABEL, t1) / 2, OV_Y + 28,
             t1, C_ACCENT, OPAQUE);
    gfx_text(m->c, &FONT_SMALL, OV_X + OV_W / 2 - gfx_text_width(&FONT_SMALL, t2) / 2, OV_Y + 48,
             t2, C_TEXT_DIM, OPAQUE);
}

static void clear_overlay(monitor_t *m) {
    for (int x = OV_X - 2; x < OV_X + OV_W + 2; x++) {
        clear_column(m, x);
    }
}

// ------------------------------------------------------------------ Kurve
static void vspan(canvas_t *c, int x, float lo, float hi, uint32_t col, int opacity) {
    int j0 = (int)floorf(lo), j1 = (int)ceilf(hi);
    if (j0 < TRACE_Y0) j0 = TRACE_Y0;
    if (j1 > TRACE_Y1) j1 = TRACE_Y1;
    for (int j = j0; j < j1; j++) {
        float a = fmaxf(lo, (float)j), b = fminf(hi, (float)j + 1.0f);
        if (b > a) {
            gfx_blend(c, x, j, col, (int)((b - a) * (float)opacity));
        }
    }
}

static float to_y(const monitor_t *m, float mv) {
    float y = (float)BASELINE_Y - mv * m->px_per_mv;
    if (y < TRACE_Y0 + 2) y = TRACE_Y0 + 2;
    if (y > TRACE_Y1 - 3) y = TRACE_Y1 - 3;
    return y;
}

static void next_column(monitor_t *m) {
    if (m->sweep_x >= TRACE_X1) {  // neuer Durchlauf: erst jetzt die Verstärkung wechseln
        m->sweep_x = TRACE_X0;
        m->have_prev = false;
        m->px_per_mv = m->pending_px_per_mv;
    }
}

static void finish_column(monitor_t *m) {
    next_column(m);
    int x = m->sweep_x;
    for (int k = 0; k < ERASE_GAP && x + k < TRACE_X1; k++) {
        clear_column(m, x + k);
    }
    float ymin = to_y(m, m->col_max), ymax = to_y(m, m->col_min);
    float lo = ymin, hi = ymax;
    if (m->have_prev) {
        lo = fminf(lo, m->prev_y);
        hi = fmaxf(hi, m->prev_y);
    }
    if (GLOW) {
        vspan(m->c, x, lo - 2.2f, hi + 2.2f, C_TRACE_GLOW, 90);
    }
    vspan(m->c, x, lo - 0.9f, hi + 0.9f, C_TRACE, OPAQUE);
    if (x + 3 < TRACE_X1) {  // leuchtende Spitze im Löschbereich
        float yl = to_y(m, m->col_last);
        gfx_disc(m->c, (float)x + 3.5f, yl, 2.6f, C_HEAD, 200);
    }
    m->prev_y = to_y(m, m->col_last);
    m->have_prev = true;
    if (m->dirty_from < 0) {
        m->dirty_from = m->col_counter;
    }
    m->dirty_to = m->col_counter;
    m->col_counter++;
    m->sweep_x++;
}

static void plot_sample(monitor_t *m, float mv) {
    if (m->col_n == 0) {
        m->col_min = m->col_max = mv;
    } else {
        m->col_min = fminf(m->col_min, mv);
        m->col_max = fmaxf(m->col_max, mv);
    }
    m->col_last = mv;
    if (++m->col_n == SAMPLES_PER_COL) {
        finish_column(m);
        m->col_n = 0;
    }
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void update_gain(monitor_t *m, double amplitude) {
    m->amps[m->amp_pos] = (float)fabs(amplitude);
    m->amp_pos = (m->amp_pos + 1) % 8;
    if (m->n_amps < 8) {
        m->n_amps++;
    }
    if (m->n_amps < 3) {
        return;
    }
    float tmp[8];
    memcpy(tmp, m->amps, sizeof tmp);
    qsort(tmp, (size_t)m->n_amps, sizeof(float), cmp_float);
    float med = tmp[m->n_amps / 2];
    if (med < 0.05f) {
        return;
    }
    float want = 0.55f * (float)TRACE_H / med;  // R-Zacke über gut die Hälfte der Höhe
    float best = GAIN_STEPS[0];
    for (size_t i = 0; i < sizeof(GAIN_STEPS) / sizeof(GAIN_STEPS[0]); i++) {
        if (GAIN_STEPS[i] <= want) {
            best = GAIN_STEPS[i];
        }
    }
    m->pending_px_per_mv = best;
}

// ------------------------------------------------------------------ Schnittstelle
void monitor_init(monitor_t *m, canvas_t *c) {
    memset(m, 0, sizeof *m);
    m->c = c;
    m->draw = true;
    qrs_init(&m->qrs);
    hr_init(&m->hr, ECG_FS);
    sosf_reset(&m->filt);
    m->px_per_mv = m->pending_px_per_mv = 60.0f;
    m->sweep_x = TRACE_X0;
    m->dirty_from = -1;
    m->status = MON_LEARNING;
    m->bpm = -1;
    m->rr_ms = -1;
    m->battery_pct = -1;
    m->shown_battery = -1;
    m->need_full = true;
}

void monitor_draw_all(monitor_t *m) {
    draw_top(m);
    draw_trace_bg(m);
    draw_calibration(m);
    draw_bottom(m);
    m->overlay = false;
    if (m->status == MON_LEAD_OFF) {
        draw_overlay(m);
        m->overlay = true;
    }
    m->need_full = true;
}

void monitor_set_link(monitor_t *m, bool usb_mounted, bool streaming) {
    m->usb = usb_mounted;
    m->streaming = streaming;
}

void monitor_set_battery(monitor_t *m, int pct) {
    m->battery_pct = pct;
}

void monitor_set_measure(monitor_t *m, bool active, int elapsed_s, int total_s) {
    m->measuring = active;
    m->meas_elapsed_s = active ? elapsed_s : 0;
    m->meas_total_s = total_s;
}

void monitor_relearn(monitor_t *m) {
    qrs_reset(&m->qrs, m->index);
    hr_reset(&m->hr);
    m->bpm = -1;
    m->rr_ms = -1;
    m->status = MON_LEARNING;
}

void monitor_feed(monitor_t *m, const uint32_t *raw, int n, uint8_t flags, uint32_t factor) {
    m->blocks++;
    bool test = (flags & EKG_FLAG_TEST) != 0;
    if (test != m->test) {
        // andere Signalquelle: neu lernen statt die alten Schwellen weiterzuverwenden
        m->test = test;
        qrs_reset(&m->qrs, m->index);
        hr_reset(&m->hr);
        sosf_reset(&m->filt);
    }
    // 1000 Hz: paarweise mitteln, Anzeige und Erkennung laufen mit 500 Hz
    int step = factor <= 32 ? 2 : 1;
    double conv_factor = (double)factor * (double)step;
    double x[QRS_MAX_BLOCK];
    int k = 0;
    for (int i = 0; i + step <= n && k < (int)QRS_MAX_BLOCK; i += step) {
        double sum = 0;
        for (int j = 0; j < step; j++) {
            sum += (double)raw[i + j];
        }
        double v = sum * (VREF_V / ADC_STEPS / conv_factor);
        x[k++] = (v - VREF_V / 2.0) / GAIN * 1000.0;
    }
    bool lead_off = (flags & (EKG_FLAG_LO_PLUS | EKG_FLAG_LO_MINUS)) != 0;
    bool clip = (flags & EKG_FLAG_CLIP) != 0;
    if (clip) {
        m->clip_until = m->blocks + 20;  // eine Sekunde lang anzeigen
    }
    if (lead_off || clip) {
        // wie am Laptop: unbrauchbare Abschnitte setzen Erkennung und HF zurück
        m->index += k;
        qrs_reset(&m->qrs, m->index);
        hr_reset(&m->hr);
        m->bpm = -1;
        m->rr_ms = -1;
    } else {
        qrs_beat_t beats[8];
        size_t nb = qrs_process(&m->qrs, x, (size_t)k, beats, 8);
        m->index += k;
        for (size_t i = 0; i < nb; i++) {
            hr_add(&m->hr, beats[i].index);
            update_gain(m, beats[i].amplitude_mv);
            m->beat_pending = true;
        }
        double bpm, rr;
        m->bpm = hr_bpm(&m->hr, (double)m->index, &bpm) ? (int)lrint(bpm) : -1;
        m->rr_ms = hr_last_rr_ms(&m->hr, &rr) ? (int)lrint(rr) : -1;
    }
    for (int i = 0; i < k; i++) {
        if (lead_off) {
            continue;  // Kurve pausiert, der Hinweis liegt darüber
        }
        m->last_mv = x[i];
        float y = sosf_step(&m->filt, (float)x[i]);
        if (m->draw) {
            plot_sample(m, y);
        }
    }
    if (lead_off) {
        m->status = MON_LEAD_OFF;
    } else if (m->blocks < m->clip_until) {
        m->status = MON_CLIPPED;
    } else if (!qrs_initialized(&m->qrs)) {
        m->status = MON_LEARNING;
    } else if (m->bpm < 0) {
        m->status = MON_SEARCHING;
    } else {
        m->status = MON_GOOD;
    }
}

static int add_rect(rect_t *dirty, int n, int max, int x, int y, int w, int h) {
    if (n < max && w > 0 && h > 0) {
        dirty[n] = (rect_t){x, y, w, h};
        return n + 1;
    }
    return n;
}

int monitor_frame(monitor_t *m, uint32_t now_ms, rect_t *dirty, int max_dirty) {
    int nd = 0;
    if (m->need_full) {
        m->need_full = false;
        m->dirty_from = -1;
        m->beat_pending = false;
        return add_rect(dirty, 0, max_dirty, 0, 0, SCREEN_W, SCREEN_H);
    }
    if (m->beat_pending) {
        m->beat_pending = false;
        m->beat_at = now_ms;
    }
    bool big = m->bpm >= 0 && now_ms - m->beat_at < 140u;
    if (big != m->shown_big_heart) {
        draw_heart(m, big);
        nd = add_rect(dirty, nd, max_dirty, 4, BOTTOM_Y + 4, 40, 50);
    }
    if (m->bpm != m->shown_bpm) {
        draw_bpm(m);
        draw_heart(m, big);
        nd = add_rect(dirty, nd, max_dirty, 4, BOTTOM_Y + 2, 162, 54);
    }
    if (m->status != m->shown_status || m->rr_ms != m->shown_rr) {
        draw_status(m);
        nd = add_rect(dirty, nd, max_dirty, 170, BOTTOM_Y + 2, SCREEN_W - 170, 54);
    }
    if (m->test != m->shown_test || m->usb != m->shown_usb ||
        m->streaming != m->shown_streaming || m->battery_pct != m->shown_battery ||
        m->measuring != m->shown_measuring || m->meas_elapsed_s != m->shown_elapsed ||
        m->menu_button != m->shown_menu_button) {
        draw_top(m);
        nd = add_rect(dirty, nd, max_dirty, 0, 0, SCREEN_W, TOP_H);
    }
    if (m->px_per_mv != m->shown_px_per_mv) {
        draw_calibration(m);
        nd = add_rect(dirty, nd, max_dirty, 0, TRACE_Y0, TRACE_X0, TRACE_H);
    }
    bool want_overlay = m->status == MON_LEAD_OFF;
    if (want_overlay != m->overlay) {
        if (want_overlay) {
            draw_overlay(m);
        } else {
            clear_overlay(m);
        }
        m->overlay = want_overlay;
        nd = add_rect(dirty, nd, max_dirty, OV_X - 2, OV_Y - 1, OV_W + 4, OV_H + 2);
    }
    if (m->dirty_from >= 0) {
        // geänderte Spalten samt Löschbereich; beim Umlauf in zwei Teilen
        int ncols = m->dirty_to - m->dirty_from + 1 + ERASE_GAP;
        if (ncols > TRACE_W) {
            ncols = TRACE_W;
        }
        int start = m->sweep_x - (m->dirty_to - m->dirty_from + 1);
        while (start < TRACE_X0) {
            start += TRACE_W;
        }
        int first = ncols;
        if (start + ncols > TRACE_X1) {
            first = TRACE_X1 - start;
        }
        nd = add_rect(dirty, nd, max_dirty, start, TRACE_Y0, first, TRACE_H);
        nd = add_rect(dirty, nd, max_dirty, TRACE_X0, TRACE_Y0, ncols - first, TRACE_H);
        m->dirty_from = -1;
    }
    return nd;
}
