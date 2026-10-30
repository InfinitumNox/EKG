// Atem-Modus: laufende Messung und Ergebnisseiten. Die Schätzung steckt in ana_breath.c.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_int.h"
#include "assets.h"
#include "check.h"
#include "theme.h"

#define LIVE_WINDOW_S 40.0
#define EST_FROM_S 45.0
#define GRID (LIVE_WINDOW_S * ANA_BREATH_HZ)

void breath_live_reset(app_t *a) {
    memset(&a->bl, 0, sizeof a->bl);
    a->seen_beat_seq = a->mon->beat_seq;
    a->bl_draw_at = 0;
    a->bl_est_at = 0;
}

// Aufruf nach jedem Messblock: neuen Schlag in die Reihe übernehmen
void breath_live_feed(app_t *a) {
    if (a->mon->beat_seq == a->seen_beat_seq) {
        return;
    }
    a->seen_beat_seq = a->mon->beat_seq;
    breath_live_t *b = &a->bl;
    if (b->n >= BL_MAX) {
        return;
    }
    if (b->n == 0) {
        b->t[0] = a->mon->beat_t_s;  // Zeit ab dem ersten Schlag
    }
    b->t[b->n] = a->mon->beat_t_s;
    b->a[b->n] = a->mon->beat_amp_mv;
    b->hr[b->n] = a->mon->bpm > 0 ? (float)a->mon->bpm : 0.0f;
    b->n++;
}

// laufende Schätzung alle 5 s, wenn mindestens 45 s Schläge vorliegen
static void update_estimate(app_t *a, uint32_t now) {
    breath_live_t *b = &a->bl;
    if (now < a->bl_est_at || b->n < 40 || b->t[b->n - 1] - b->t[0] < EST_FROM_S) {
        return;
    }
    a->bl_est_at = now + 5000;
    double hz;
    b->has_est = ana_breath_peak(b->t, b->a, b->n, &hz, a->scratch, a->scratch_size) &&
                 hz >= 0.13 && hz <= 0.45;
    b->est_hz = b->has_est ? hz : 0.0;
}

// ------------------------------------------------------------------ Zeichnen
static void curve_box(app_t *a, box_t r, const double *g, const bool *valid, int n, double scale,
                      uint32_t col, float width) {
    canvas_t *c = a->c;
    for (int k = 1; k <= 3; k++) {
        gfx_fill(c, r.x, r.y + r.h * k / 4, r.w, 1, C_GRID);
    }
    float px = 0, py = 0;
    bool have = false;
    for (int i = 0; i < n; i++) {
        if (!valid[i]) {
            have = false;
            continue;
        }
        float x = (float)r.x + (float)(r.w - 1) * (float)i / (float)(n > 1 ? n - 1 : 1);
        double q = g[i] / scale;
        q = q > 1.0 ? 1.0 : q < -1.0 ? -1.0 : q;
        float y = (float)r.y + (float)r.h / 2.0f - (float)q * (float)r.h * 0.46f;
        if (have) {
            gfx_line(c, px, py, x, y, width, col, OPAQUE);
        }
        px = x;
        py = y;
        have = true;
    }
}

static const char *signal_text(int q) {
    return q == 0 ? "Signal gut" : q == 1 ? "Signal leicht gestört" : "Signal stark gestört";
}

void breath_draw_live(app_t *a) {
    const uint32_t now = a->clock;
    canvas_t *c = a->c;
    lay_t l = lay(a);
    breath_live_t *b = &a->bl;
    update_estimate(a, now);
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    gfx_fill(c, 0, 0, l.w, HEADER_H, C_PANEL);
    gfx_fill(c, 0, HEADER_H - 1, l.w, 1, C_PANEL_EDGE);
    int x = gfx_text(c, &FONT_LABEL, 10, 19, "Atmung", C_TEXT, OPAQUE);
    const int el = (int)ana_rec_seconds(a->rec);
    char t[24];
    fmt_duration(t, sizeof t, el);
    char tt[32];
    snprintf(tt, sizeof tt, l.portrait ? "%s" : "%s / 2:00", t);
    gfx_text(c, &FONT_SMALL, x + 8, 18, tt, C_TEXT_DIM, OPAQUE);
    if (!l.portrait) {
        gfx_fill(c, 170, 11, 70, 4, C_PANEL_EDGE);
        gfx_fill(c, 170, 11, 70 * el / ANA_MAX_S, 4, C_ACCENT_2);
    }
    button(c, (box_t){l.w - 70, 3, 62, 20}, "Stopp", true, true);

    // Atemkurve der letzten 40 s
    const box_t cb = l.portrait ? (box_t){12, 56, l.w - 24, 100} : (box_t){12, 54, 200, 78};
    gfx_text(c, &FONT_SMALL, 12, 44, "Atmung (aus dem EKG)", C_TEXT_DIM, OPAQUE);
    if (b->n > 8) {
        const double t_end = b->t[b->n - 1];
        const int ng = (int)GRID + 4 * 12;
        double g[(int)GRID + 4 * 12 + 1], tmp[(int)GRID + 4 * 12 + 1];
        bool valid[(int)GRID + 4 * 12 + 1];
        ana_breath_series(b->t, b->a, b->n, t_end - LIVE_WINDOW_S - 12.0, ng, g, valid, tmp);
        const int skip = 4 * 12;
        double scale = 0.0, med = 0.0;
        for (int i = skip; i < ng; i++) {
            scale = fmax(scale, fabs(g[i]));
        }
        for (int i = 0; i < b->n; i++) {
            med += b->a[i];
        }
        med /= b->n;
        scale = fmax(scale, 0.05 * med);  // ohne Atmung nicht das Rauschen aufblasen
        curve_box(a, cb, g + skip, valid + skip, ng - skip, scale, C_ACCENT_2, 2.0f);
    } else {
        text_center(c, &FONT_SMALL, cb.x + cb.w / 2, cb.y + cb.h / 2, "Kurve baut sich auf …",
                    C_TEXT_FAINT);
    }

    // Atemzüge (≈ aus der Amplitude)
    char v[24];
    const int rx = l.portrait ? 12 : 226, ry = l.portrait ? 180 : 44;
    gfx_text(c, &FONT_SMALL, rx, ry, "Atemzüge ≈", C_TEXT_DIM, OPAQUE);
    if (b->has_est) {
        snprintf(v, sizeof v, "%ld", lrint(b->est_hz * 60.0));
        int e = gfx_text(c, &FONT_HR, rx - 2, l.portrait ? ry + 52 : ry + 60, v, C_TEXT, OPAQUE);
        gfx_text(c, &FONT_SMALL, e + 6, l.portrait ? ry + 52 : ry + 60, "/min", C_TEXT_DIM,
                 OPAQUE);
    } else {
        gfx_text(c, &FONT_HR, rx - 2, l.portrait ? ry + 52 : ry + 60, "– –", C_TEXT_FAINT, OPAQUE);
        if (!l.portrait) {
            gfx_text(c, &FONT_SMALL, rx, ry + 82, "ab 45 s", C_TEXT_FAINT, OPAQUE);
        }
    }
    gfx_fill(c, 12, l.portrait ? 258 : 144, l.w - 24, 1, C_PANEL_EDGE);
    const int sy = l.portrait ? 276 : 164;
    gfx_text(c, &FONT_SMALL, 12, sy, "Puls", C_TEXT_DIM, OPAQUE);
    snprintf(v, sizeof v, a->mon->bpm > 0 ? "%d /min" : "– –", a->mon->bpm);
    gfx_text(c, &FONT_LABEL, 12, sy + 22, v, C_TEXT, OPAQUE);
    float lo = 999, hi = 0;
    for (int i = b->n > 60 ? b->n - 60 : 0; i < b->n; i++) {
        if (b->hr[i] > 0) {
            lo = fminf(lo, b->hr[i]);
            hi = fmaxf(hi, b->hr[i]);
        }
    }
    gfx_text(c, &FONT_SMALL, l.portrait ? 110 : 120, sy, "Spanne (1 min)", C_TEXT_DIM, OPAQUE);
    snprintf(v, sizeof v, hi > 0 ? "%d–%d" : "– –", (int)lo, (int)hi);
    gfx_text(c, &FONT_LABEL, l.portrait ? 110 : 120, sy + 22, v, C_TEXT, OPAQUE);
    const int q = a->mon->status == MON_GOOD || a->mon->status == MON_SEARCHING ? 0 : 1;
    gfx_disc(c, 16.0f, (float)l.h - 14.0f, 4.0f, q ? C_WARN : C_OK, OPAQUE);
    gfx_text(c, &FONT_SMALL, 26, l.h - 9,
             q ? (l.portrait ? "Signal prüfen" : "Signal prüfen · ruhig sitzen")
               : (l.portrait ? "Signal gut" : "Signal gut · ruhig sitzen, normal atmen"),
             C_TEXT_DIM, OPAQUE);
}

// ------------------------------------------------------------------ Ergebnis
int breath_result_pages(void) {
    return 3;
}

const char *breath_page_name(int page) {
    static const char *const N[3] = {"Übersicht", "Kurve", "Hinweise"};
    return N[page < 0 ? 0 : page > 2 ? 2 : page];
}

static void curve_from_result(app_t *a, box_t r, uint32_t col, float width) {
    const ana_breath_t *br = &a->breath;
    double g[ANA_BREATH_CURVE];
    bool valid[ANA_BREATH_CURVE];
    for (int i = 0; i < br->curve_n; i++) {
        g[i] = br->curve[i];
        valid[i] = true;
    }
    curve_box(a, r, g, valid, br->curve_n, 100.0, col, width);
}

static const char *why_text(ana_breath_why_t w) {
    switch (w) {
        case BR_TOO_SHORT:
            return "Zu wenige saubere Schläge für eine Aussage.";
        case BR_NO_PEAK:
            return "Keine deutliche Atemschwankung im EKG.";
        case BR_MISMATCH:
            return "Atmung und Puls passen nicht zusammen: Wert nicht sicher.";
        default:
            return "";
    }
}

static void page_overview(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const ana_breath_t *br = &a->breath;
    char v[48];
    gfx_text(c, &FONT_SMALL, 14, 46, "Atemzüge", C_TEXT_DIM, OPAQUE);
    if (br->why == BR_OK) {
        const long rate = lrint(br->rate_hz * 60.0);
        snprintf(v, sizeof v, "%ld", rate);
        int e = gfx_text(c, &FONT_HR, 12, 96, v, C_TEXT, OPAQUE);
        gfx_text(c, &FONT_SMALL, e + 6, 96, "/min", C_TEXT_DIM, OPAQUE);
        const bool usual = rate >= 12 && rate <= 20;
        gfx_text(c, &FONT_SMALL, 14, 116,
                 usual            ? "im üblichen Bereich (12–20)"
                 : rate < 12      ? "langsamer als üblich (12–20)"
                                  : "schneller als üblich (12–20)",
                 usual ? C_OK : C_TEXT_DIM, OPAQUE);
    } else {
        gfx_text(c, &FONT_HR, 12, 96, "– –", C_TEXT_FAINT, OPAQUE);
        gfx_text_wrap(c, &FONT_SMALL, 14, 114, l.portrait ? l.w - 28 : 156, 15, why_text(br->why),
                      C_TEXT_DIM, 3);
    }
    // Kennwerte: quer rechts, hochkant darunter
    const int kx = l.portrait ? 14 : 168, ky = l.portrait ? 150 : 46, ks = l.portrait ? 26 : 26;
    const int kr = l.w - 12;
    const char *lab[3] = {"Schwankung", "Gleichmaß", "HRV (RMSSD)"};
    char val[3][24];
    uint32_t col[3] = {C_TEXT, C_TEXT, C_TEXT};
    snprintf(val[0], sizeof val[0], br->has_swing ? "%ld /min" : "–", lrint(br->swing_bpm));
    if (br->cycle_cv < 0) {
        snprintf(val[1], sizeof val[1], "–");
    } else if (br->cycle_cv < 0.15) {
        snprintf(val[1], sizeof val[1], "hoch");
        col[1] = C_OK;
    } else if (br->cycle_cv < 0.30) {
        snprintf(val[1], sizeof val[1], "mittel");
        col[1] = C_WARN;
    } else {
        snprintf(val[1], sizeof val[1], "niedrig");
        col[1] = C_WARN;
    }
    if (isfinite(a->breath_rmssd)) {
        snprintf(val[2], sizeof val[2], "%ld ms", lrint(a->breath_rmssd));
    } else {
        snprintf(val[2], sizeof val[2], "–");
    }
    for (int i = 0; i < 3; i++) {
        gfx_text(c, &FONT_SMALL, kx, ky + i * ks, lab[i], C_TEXT_DIM, OPAQUE);
        gfx_text(c, &FONT_LABEL, kr - gfx_text_width(&FONT_LABEL, val[i]), ky + 1 + i * ks, val[i],
                 col[i], OPAQUE);
    }
    const box_t cb = l.portrait ? (box_t){12, 210, l.w - 24, 44} : (box_t){14, 134, l.w - 28, 46};
    if (br->curve_n > 8) {
        curve_from_result(a, cb, C_ACCENT_2, 1.8f);
    }
    gfx_text(c, &FONT_SMALL, 14, l.footer_y - 6, "Atemkurve aus dem EKG · 2 min", C_TEXT_FAINT,
             OPAQUE);
}

static void page_curve(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const ana_breath_t *br = &a->breath;
    const box_t cb = {14, 54, l.w - 28, l.footer_y - 54 - 56};
    for (int s = 0; s <= 120; s += 30) {
        int x = cb.x + (cb.w - 1) * s / 120;
        gfx_fill(c, x, cb.y, 1, cb.h, C_GRID);
        char b[8];
        snprintf(b, sizeof b, "%d", s);
        text_center(c, &FONT_SMALL, x < 20 ? 20 : x > l.w - 20 ? l.w - 20 : x, cb.y + cb.h + 16, b,
                    C_TEXT_FAINT);
    }
    if (br->curve_n > 8) {
        curve_from_result(a, cb, C_ACCENT_2, 2.0f);
    } else {
        text_center(c, &FONT_SMALL, l.w / 2, cb.y + cb.h / 2, "keine Kurve", C_TEXT_DIM);
    }
    char s[64];
    if (br->n_cycles > 0) {
        snprintf(s, sizeof s, "%d Atemzüge erkannt", br->n_cycles);
    } else {
        snprintf(s, sizeof s, "keine Atemzüge erkannt");
    }
    gfx_text(c, &FONT_SMALL, 14, l.footer_y - 24, s, C_TEXT, OPAQUE);
    gfx_text(c, &FONT_SMALL, 14, l.footer_y - 7, "Sekunden seit Beginn · Höhe: R-Zacke", C_TEXT_FAINT,
             OPAQUE);
}

static void page_notes(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const ana_breath_t *br = &a->breath;
    const char *texts[6];
    int n = 0;
    if (a->breath_test) {
        texts[n++] = "Testsignal – keine echte Messung.";
    }
    texts[n++] = "Aus dem EKG geschätzt, nicht direkt gemessen: Beim Atmen schwanken die R-Zacke "
                 "und der Puls.";
    if (br->why == BR_OK) {
        texts[n++] = "Gegengeprüft: Amplitude und Puls schwanken im selben Takt.";
    } else {
        texts[n++] = why_text(br->why);
    }
    if (br->why != BR_OK && br->has_edr) {
        static char rough[64];
        snprintf(rough, sizeof rough, "Grobe Schätzung nur aus der Amplitude: etwa %ld pro Minute.",
                 lrint(br->edr_hz * 60.0));
        texts[n++] = rough;
    }
    if (a->breath_signal) {
        texts[n++] = signal_text(a->breath_signal);
    }
    texts[n++] = "Ruhig sitzen, normal atmen, nicht sprechen; frische Elektroden helfen.";
    int y = 50;
    for (int i = 0; i < n && y < l.footer_y - 24; i++) {
        gfx_disc(c, 17.0f, (float)y - 4.0f, 2.5f, C_ACCENT, OPAQUE);
        int lines = gfx_text_wrap(c, &FONT_SMALL, 26, y, l.w - 40, 16, texts[i], C_TEXT,
                                  (l.footer_y - 24 - y) / 16 + 1);
        y += lines * 16 + 6;
    }
    gfx_text(c, &FONT_SMALL, 14, l.footer_y - 6, "Keine Diagnose.", C_TEXT_FAINT, OPAQUE);
}

void breath_draw_result(app_t *a) {
    switch (a->page) {
        case 0:
            page_overview(a);
            break;
        case 1:
            page_curve(a);
            break;
        default:
            page_notes(a);
            break;
    }
}

// nach ana_run: Atmung auswerten und die Kennwerte der Aufnahme übernehmen
void breath_finish(app_t *a) {
    const char *dummy;
    a->breath_test = a->result_test;
    a->breath_rmssd = a->res->has_hrv ? a->res->hrv.rmssd_ms : NAN;
    a->breath_hr = a->res->has_hrv ? a->res->hrv.mean_hr : NAN;
    a->breath_signal = check_quality(a->res, &dummy);
    a->has_breath = a->result_status == ANA_OK &&
                    ana_breath(a->rec, a->res, &a->breath, a->scratch, a->scratch_size);
    a->has_result = false;  // die Aufnahme gehört jetzt zur Atmung
    a->result_kind = 1;
}
