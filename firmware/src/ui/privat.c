// Privater Bereich: PIN, Übersicht, Solo-Session mit lernender Anzeige, Verlauf.
//
// Zugang nur über fünf schnelle Tipps auf die Versionszeile unter Info. Die Daten bleiben im
// Gerät (eigener Flash-Sektor, PIN nur als Prüfwert); sie gehen nie zum Laptop.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_int.h"
#include "assets.h"
#include "theme.h"

#define TAP_GAP_MS 1000u
#define LOCK_MS 30000u
#define MAX_FAILS 3

enum { PIN_ENTER, PIN_SET1, PIN_SET2 };
enum { PROMPT_NONE, PROMPT_DELETE, PROMPT_FORGOT };

static void leave(app_t *a) {
    memset(&a->priv.digits, 0, sizeof a->priv.digits);
    a->priv.n_digits = 0;
    a->priv.prompt = PROMPT_NONE;
    go_live(a);
}

// ------------------------------------------------------------------ Zugang
box_t info_version_box(lay_t l) {
    return (box_t){0, 34, l.w, 26};
}

static void open_pin(app_t *a, int mode, bool changing) {
    priv_ui_t *p = &a->priv;
    p->pin_mode = (uint8_t)mode;
    p->changing = changing;
    p->n_digits = 0;
    memset(p->digits, 0, sizeof p->digits);
    p->prompt = PROMPT_NONE;
    a->screen = APP_PRIV_PIN;
    a->mon->draw = false;
    redraw(a);
}

void priv_tap_version(app_t *a, uint32_t now) {
    priv_ui_t *p = &a->priv;
    if (!a->pd) {
        return;
    }
    if (p->n_taps > 0 && now - p->tap_at[p->n_taps - 1] > TAP_GAP_MS) {
        p->n_taps = 0;
    }
    p->tap_at[p->n_taps++] = now;
    if (p->n_taps >= 5) {
        p->n_taps = 0;
        open_pin(a, a->pd->has_pin ? PIN_ENTER : PIN_SET1, false);
    }
}

// ------------------------------------------------------------------ Zeichnen: Bausteine
static box_t pin_key(lay_t l, int i) {
    const int x0 = l.portrait ? 12 : 24, kw = l.portrait ? (l.w - 24) / 3 : 91;
    const int y0 = l.portrait ? 96 : 70, kh = l.portrait ? 52 : 42;
    return (box_t){x0 + (i % 3) * kw, y0 + (i / 3) * kh, kw, kh};
}

static box_t prompt_button(lay_t l, int i) {  // 0 = Abbrechen, 1 = Bestätigen
    const int w = (l.w - 24 - 24 - 10) / 2;
    return (box_t){24 + 12 + i * (w + 10), l.h / 2 + 18, w, 34};
}

static void draw_prompt(app_t *a, const char *title, const char *text, const char *yes) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    dim_screen(c);
    const box_t p = {24, l.h / 2 - 64, l.w - 48, 130};
    gfx_round_rect(c, p.x, p.y, p.w, p.h, 14, C_PANEL, OPAQUE);
    gfx_round_frame(c, p.x, p.y, p.w, p.h, 14, 1.5f, C_ACCENT);
    gfx_text(c, &FONT_LABEL, p.x + 14, p.y + 26, title, C_TEXT, OPAQUE);
    gfx_text_wrap(c, &FONT_SMALL, p.x + 14, p.y + 48, p.w - 28, 16, text, C_TEXT_DIM, 2);
    button(c, prompt_button(l, 0), "Abbrechen", false, true);
    button(c, prompt_button(l, 1), yes, true, true);
}

static void lock_icon(canvas_t *c, int x, int y, uint32_t col) {
    gfx_round_frame(c, x + 2, y - 2, 8, 9, 4, 1.6f, col);
    gfx_round_rect(c, x, y + 3, 12, 9, 2, col, OPAQUE);
}

static void check_row(canvas_t *c, int y, const char *text, bool ok) {
    gfx_disc(c, 20.0f, (float)y - 5.0f, 5.0f, ok ? C_OK : C_WARN, OPAQUE);
    if (ok) {
        gfx_line(c, 17.5f, (float)y - 5.0f, 19.5f, (float)y - 2.5f, 1.4f, 0xFFFFFFu, OPAQUE);
        gfx_line(c, 19.5f, (float)y - 2.5f, 23.0f, (float)y - 7.5f, 1.4f, 0xFFFFFFu, OPAQUE);
    } else {
        gfx_fill(c, 19, y - 8, 2, 5, 0x202020u);
        gfx_fill(c, 19, y - 2, 2, 2, 0x202020u);
    }
    gfx_text(c, &FONT_SMALL, 32, y, text, ok ? C_TEXT : C_WARN, OPAQUE);
}

// ------------------------------------------------------------------ PIN
static void draw_pin(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    priv_ui_t *p = &a->priv;
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Privat", p->pin_mode == PIN_ENTER ? NULL : "PIN festlegen");
    const char *hint = p->pin_mode == PIN_ENTER ? "PIN" : p->pin_mode == PIN_SET1 ? "Neue PIN"
                                                                              : "Wiederholen";
    const bool locked = a->clock < p->locked_until;
    char t[40];
    if (locked) {
        snprintf(t, sizeof t, "gesperrt: %d s", (int)((p->locked_until - a->clock + 999u) / 1000u));
        hint = t;
    }
    const int dy = l.portrait ? 64 : 49;
    gfx_text(c, &FONT_SMALL, 16, dy + 5, hint, locked ? C_WARN : C_TEXT_DIM, OPAQUE);
    for (int i = 0; i < PRIV_PIN_DIGITS; i++) {
        gfx_disc(c, (float)(l.w - 22 - (PRIV_PIN_DIGITS - 1 - i) * 22), (float)dy, 6.0f,
                 i < p->n_digits ? C_ACCENT : C_PANEL_EDGE, OPAQUE);
    }
    static const char *const K[12] = {"7", "8", "9", "4", "5", "6", "1", "2", "3", "‹", "0", "OK"};
    for (int i = 0; i < 12; i++) {
        const box_t k = pin_key(l, i);
        const bool ok = i == 11, on = ok && p->n_digits == PRIV_PIN_DIGITS && !locked;
        gfx_round_rect(c, k.x + 1, k.y + 1, k.w - 2, k.h - 2, 7,
                       on ? C_ACCENT : ok ? C_PANEL : gfx_mix(C_PANEL, C_TEXT, 14), OPAQUE);
        text_center(c, &FONT_LABEL, k.x + k.w / 2, k.y + k.h / 2 + 6, K[i],
                    on ? 0xFFFFFFu : ok ? C_TEXT_FAINT : C_TEXT);
    }
    if (p->prompt == PROMPT_FORGOT) {
        draw_prompt(a, "PIN vergessen?", "Alle privaten Daten werden gelöscht, dann gilt eine neue PIN.",
                    "Löschen");
    }
}

static void pin_wrong(app_t *a, uint32_t now, const char *msg) {
    priv_ui_t *p = &a->priv;
    p->n_digits = 0;
    memset(p->digits, 0, sizeof p->digits);
    show_toast(a, msg, now);
}

static void pin_confirm(app_t *a, uint32_t now) {
    priv_ui_t *p = &a->priv;
    priv_data_t *d = a->pd;
    if (p->n_digits != PRIV_PIN_DIGITS) {
        return;
    }
    if (p->pin_mode == PIN_ENTER) {
        if (priv_check_pin(d, p->digits)) {
            p->fails = 0;
            p->n_digits = 0;
            memset(p->digits, 0, sizeof p->digits);
            a->screen = APP_PRIV_HOME;
            redraw(a);
        } else if (++p->fails >= MAX_FAILS) {
            p->fails = 0;
            p->locked_until = now + LOCK_MS;
            pin_wrong(a, now, "Falsche PIN – 30 Sekunden gesperrt.");
        } else {
            pin_wrong(a, now, "Falsche PIN.");
        }
    } else if (p->pin_mode == PIN_SET1) {
        memcpy(p->first, p->digits, sizeof p->first);
        p->pin_mode = PIN_SET2;
        p->n_digits = 0;
        memset(p->digits, 0, sizeof p->digits);
        redraw(a);
    } else if (memcmp(p->first, p->digits, PRIV_PIN_DIGITS) != 0) {
        p->pin_mode = PIN_SET1;
        pin_wrong(a, now, "Die PINs sind verschieden. Bitte neu festlegen.");
    } else {
        priv_set_pin(d, p->digits);
        a->req_priv_save = true;
        memset(p->first, 0, sizeof p->first);
        p->n_digits = 0;
        memset(p->digits, 0, sizeof p->digits);
        a->screen = APP_PRIV_HOME;
        redraw(a);
        show_toast(a, "PIN gespeichert.", now);
    }
}

static void pin_event(app_t *a, ui_event_t ev, uint32_t now) {
    lay_t l = lay(a);
    priv_ui_t *p = &a->priv;
    const bool tap = ev.type == EV_TAP;
    if (p->prompt == PROMPT_FORGOT) {
        if (tap && hit(prompt_button(l, 1), ev.x, ev.y)) {
            priv_clear(a->pd);
            a->req_priv_save = true;
            open_pin(a, PIN_SET1, false);
            show_toast(a, "Daten gelöscht. Neue PIN festlegen.", now);
        } else if (tap) {
            p->prompt = PROMPT_NONE;
            redraw(a);
        }
        return;
    }
    if (tap && hit(close_box(l), ev.x, ev.y)) {
        if (p->changing) {
            a->screen = APP_PRIV_HOME;
            redraw(a);
        } else {
            leave(a);
        }
        return;
    }
    const bool locked = now < p->locked_until;
    for (int i = 0; i < 12; i++) {
        if (!hit(pin_key(l, i), ev.x, ev.y)) {
            continue;
        }
        if (ev.type == EV_LONG && i == 9 && p->pin_mode == PIN_ENTER && !p->changing) {
            p->prompt = PROMPT_FORGOT;  // ‹ lange halten: PIN vergessen
            redraw(a);
        } else if (tap && !locked) {
            if (i == 11) {
                pin_confirm(a, now);
            } else if (i == 9) {
                if (p->n_digits > 0) {
                    p->digits[--p->n_digits] = '\0';
                }
                redraw(a);
            } else if (p->n_digits < PRIV_PIN_DIGITS) {
                static const char D[10] = {'7', '8', '9', '4', '5', '6', '1', '2', '3'};
                p->digits[p->n_digits++] = i == 10 ? '0' : D[i];
                redraw(a);
            }
        }
        return;
    }
}

// ------------------------------------------------------------------ Übersicht
// 0 = Session, 1 = Verlauf, 2 = Anzeige, 3 = PIN ändern, 4 = Daten löschen
static box_t home_box(lay_t l, int i) {
    if (i == 0) {
        return l.portrait ? (box_t){12, 36, l.w - 24, 84} : (box_t){12, 34, l.w - 24, 70};
    }
    const int half = (l.w - 24 - 8) / 2;
    const int y0 = l.portrait ? 190 : 160, rh = l.portrait ? 46 : 40, h = l.portrait ? 38 : 32;
    const int row = (i - 1) / 2, col = (i - 1) % 2;
    return (box_t){12 + col * (half + 8), y0 + row * rh, half, h};
}

static bool session_ready(const app_t *a) {
    return !a->usb && a->mon->status != MON_LEAD_OFF;
}

static void draw_home(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    priv_ui_t *p = &a->priv;
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Privat", NULL);
    lock_icon(c, l.w - 52, 7, C_TEXT_FAINT);
    solo_build(a->pd, &p->model);
    const bool ready = session_ready(a);
    const box_t s = home_box(l, 0);
    gfx_round_rect(c, s.x, s.y, s.w, s.h, 12, ready ? C_ACCENT : gfx_mix(C_PANEL, C_TEXT, 14), OPAQUE);
    const uint32_t fg = ready ? 0xFFFFFFu : C_TEXT_DIM;
    const int my = s.y + s.h / 2;
    gfx_triangle(c, (float)s.x + 18.0f, (float)my - 10.0f, (float)s.x + 18.0f, (float)my + 10.0f,
                 (float)s.x + 34.0f, (float)my, fg, OPAQUE);
    gfx_text(c, &FONT_LABEL, s.x + 46, my - 4, "Solo-Session starten", fg, OPAQUE);
    char t[64];
    if (p->model.learned) {
        snprintf(t, sizeof t, "Gelernt aus %d Sessions", p->model.n);
    } else {
        snprintf(t, sizeof t, "Lernt noch: %d von %d Sessions", p->model.n, SOLO_MIN_SESSIONS);
    }
    gfx_text(c, &FONT_SMALL, s.x + 46, my + 15, t, fg, ready ? 210 : OPAQUE);
    const int cy = s.y + s.h + 22;
    check_row(c, cy, a->usb ? "USB steckt – nur mit Batterie" : "Batteriebetrieb, USB getrennt",
              !a->usb);
    check_row(c, cy + 20, a->mon->status == MON_LEAD_OFF ? "Elektroden ohne Kontakt"
                                                         : "Elektroden haben Kontakt",
              a->mon->status != MON_LEAD_OFF);
    button(c, home_box(l, 1), "Verlauf", false, p->model.n > 0 || a->pd->n > 0);
    button(c, home_box(l, 2),
           l.portrait ? (a->pd->display_mode ? "Nur Kurve" : "Mit Prozent")
                      : (a->pd->display_mode ? "Anzeige: Kurve" : "Anzeige: Prozent"),
           false, true);
    button(c, home_box(l, 3), "PIN ändern", false, true);
    button(c, home_box(l, 4), "Daten löschen", false, true);
    if (p->prompt == PROMPT_DELETE) {
        draw_prompt(a, "Sessions löschen?", "Alle gespeicherten Sessions und das Gelernte gehen verloren.",
                    "Löschen");
    }
}

static void session_start(app_t *a, uint32_t now) {
    priv_ui_t *p = &a->priv;
    solo_build(a->pd, &p->model);
    memset(p->hr, 0, sizeof p->hr);
    p->n_s = 0;
    p->sec_at = now;
    p->draw_at = 0;
    p->start_hr = NAN;
    p->percent = -1;
    a->screen = APP_PRIV_SESSION;
    a->mon->draw = false;
    redraw(a);
}

// ------------------------------------------------------------------ Session
static box_t done_button(lay_t l) {
    return l.portrait ? (box_t){12, l.h - 56, l.w - 24, 44} : (box_t){12, 192, l.w - 24, 40};
}

static void arc(canvas_t *c, float cx, float cy, float r, float a0, float a1, float width,
                uint32_t c0, uint32_t c1) {
    const int n = (int)(fabsf(a1 - a0) * r / 2.0f) + 2;
    for (int i = 0; i < n; i++) {
        const float t0 = (float)i / (float)n, t1 = (float)(i + 1) / (float)n;
        const float b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1;
        gfx_line(c, cx + r * cosf(b0), cy - r * sinf(b0), cx + r * cosf(b1), cy - r * sinf(b1),
                 width, gfx_mix(c0, c1, (int)(t0 * 256.0f)), OPAQUE);
    }
}

static void draw_curve(canvas_t *c, box_t r, const uint8_t *v, int n, int lo, int hi, float width) {
    for (int k = 1; k <= 3; k++) {
        gfx_fill(c, r.x, r.y + r.h * k / 4, r.w, 1, C_GRID);
    }
    float px = 0, py = 0;
    bool have = false;
    const float span = fmaxf((float)(hi - lo), 10.0f);
    for (int i = 0; i < n; i++) {
        if (!v[i]) {
            have = false;
            continue;
        }
        const float x = (float)r.x + (float)(r.w - 1) * (float)i / (float)(n > 1 ? n - 1 : 1);
        const float y = (float)r.y + (float)r.h - ((float)v[i] - (float)lo) / span * (float)r.h;
        if (have) {
            gfx_line(c, px, py, x, y, width, C_TRACE, OPAQUE);
        }
        px = x;
        py = y;
        have = true;
    }
}

static void draw_session(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    priv_ui_t *p = &a->priv;
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    char t[40];
    fmt_duration(t, sizeof t, p->n_s);
    header(a, "Solo-Session", t);
    const float now_hr = solo_median5(p->hr, p->n_s, p->n_s - 1);
    const bool show_pct = p->percent >= 0 && a->pd->display_mode == 0;

    const float cx = l.portrait ? (float)l.w / 2.0f : 86.0f, cy = l.portrait ? 112.0f : 116.0f;
    if (show_pct) {
        const float a0 = 3.6652f, a1 = -0.5236f;
        arc(c, cx, cy, 64.0f, a0, a1, 12.0f, C_PANEL_EDGE, C_PANEL_EDGE);
        const float e = a0 + (a1 - a0) * (float)p->percent / 100.0f;
        if (p->percent > 0) {
            arc(c, cx, cy, 64.0f, a0, e, 12.0f, C_ACCENT_2, C_ACCENT);
        }
        if (GLOW) {
            gfx_glow(c, cx + 64.0f * cosf(e), cy - 64.0f * sinf(e), 16.0f, C_ACCENT, 120);
        }
        gfx_disc(c, cx + 64.0f * cosf(e), cy - 64.0f * sinf(e), 8.0f, 0xFFFFFFu, OPAQUE);
        snprintf(t, sizeof t, "%d", p->percent);
        text_center(c, &FONT_HR, (int)cx, (int)cy + 18, t, C_TEXT);
        text_center(c, &FONT_SMALL, (int)cx, (int)cy + 40, "% Annäherung", C_TEXT_DIM);
    } else {
        const box_t cb = l.portrait ? (box_t){14, 56, l.w - 28, 112} : (box_t){14, 46, 146, 120};
        uint8_t v[240];
        const int n = p->n_s < 240 ? p->n_s : 240;  // letzte 4 min
        memcpy(v, p->hr + (p->n_s - n), (size_t)n);
        int lo = 999, hi = 0;
        for (int i = 0; i < n; i++) {
            if (v[i]) {
                lo = v[i] < lo ? v[i] : lo;
                hi = v[i] > hi ? v[i] : hi;
            }
        }
        if (hi > 0) {
            draw_curve(c, cb, v, n, lo - 4, hi + 4, 2.0f);
        }
        const char *why = a->pd->display_mode ? "nur Kurve (Anzeige umgestellt)"
                          : p->model.learned  ? "Anstiege noch zu verschieden"
                                              : "Lernt noch";
        gfx_text(c, &FONT_SMALL, 14, cb.y + cb.h + 16, why, C_WARN, OPAQUE);
    }

    const int rx = l.portrait ? 14 : 184, ry = l.portrait ? 196 : 48;
    gfx_text(c, &FONT_SMALL, rx, ry, "Puls", C_TEXT_DIM, OPAQUE);
    snprintf(t, sizeof t, "%d", a->mon->bpm > 0 ? a->mon->bpm : 0);
    int e = gfx_text(c, &FONT_VALUE, rx, ry + 28, a->mon->bpm > 0 ? t : "– –", C_TEXT, OPAQUE);
    gfx_text(c, &FONT_SMALL, e + 5, ry + 28, "/min", C_TEXT_DIM, OPAQUE);
    const int ry2 = l.portrait ? ry : ry + 52;
    const int rx2 = l.portrait ? 126 : rx;
    gfx_text(c, &FONT_SMALL, rx2, l.portrait ? ry : ry2, "Start", C_TEXT_DIM, OPAQUE);
    snprintf(t, sizeof t, isnan(p->start_hr) ? "–" : "%d /min", (int)lrintf(p->start_hr));
    gfx_text(c, &FONT_SMALL, l.portrait ? rx2 : l.w - 12 - gfx_text_width(&FONT_SMALL, t),
             l.portrait ? ry + 20 : ry2, t, C_TEXT, OPAQUE);
    if (p->model.learned && !l.portrait) {
        gfx_text(c, &FONT_SMALL, rx, ry + 72, "Höhepunkt", C_TEXT_DIM, OPAQUE);
        snprintf(t, sizeof t, "%d /min", (int)lrintf(p->model.peak));
        gfx_text(c, &FONT_SMALL, l.w - 12 - gfx_text_width(&FONT_SMALL, t), ry + 72, t, C_TEXT,
                 OPAQUE);
    }
    (void)now_hr;
    const bool good = a->mon->status == MON_GOOD || a->mon->status == MON_SEARCHING;
    gfx_text(c, &FONT_SMALL, rx, l.portrait ? ry + 50 : ry + 92, "Signal", C_TEXT_DIM, OPAQUE);
    gfx_text(c, &FONT_SMALL, l.portrait ? rx + 54 : l.w - 12 - gfx_text_width(&FONT_SMALL, good ? "gut" : "unruhig"),
             l.portrait ? ry + 50 : ry + 92, good ? "gut" : "unruhig", good ? C_OK : C_WARN, OPAQUE);
    button(c, done_button(l), "Fertig", true, true);
}

static void session_finish(app_t *a, uint32_t now) {
    priv_ui_t *p = &a->priv;
    if (!solo_summarize(p->hr, p->n_s, &p->last)) {
        a->screen = APP_PRIV_HOME;
        redraw(a);
        show_toast(a, "Zu wenige Messwerte – nichts gespeichert.", now);
        return;
    }
    p->t90_s = -1;
    if (p->model.learned) {
        p->t90_s = solo_t90(p->hr, p->n_s, (float)p->last.hr_start, p->model.gain);
        if (p->t90_s >= 0) {
            const int diff = (p->n_s - p->t90_s) / 10;
            p->last.err_10s = (int8_t)(diff < 1 ? 1 : diff > 127 ? 127 : diff);
        }
    }
    a->screen = APP_PRIV_DONE;
    redraw(a);
}

static box_t done_btn(lay_t l, int i) {  // 0 = Nicht werten, 1 = Speichern
    const int w = (l.w - 24 - 10) / 2, y = l.portrait ? l.h - 56 : l.h - 40;
    return (box_t){12 + i * (w + 10), y, w, l.portrait ? 44 : 32};
}

static void draw_done(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const priv_session_t *s = &a->priv.last;
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Solo-Session", "Ergebnis");
    char t[96];
    gfx_text(c, &FONT_SMALL, 14, 46, "Dauer", C_TEXT_DIM, OPAQUE);
    fmt_duration(t, sizeof t, s->duration_s);
    gfx_text(c, &FONT_VALUE, 14, 72, t, C_TEXT, OPAQUE);
    gfx_text(c, &FONT_SMALL, l.portrait ? 120 : 130, 46, "Puls", C_TEXT_DIM, OPAQUE);
    snprintf(t, sizeof t, "%d → %d", s->hr_start, s->hr_peak);
    gfx_text(c, &FONT_VALUE, l.portrait ? 120 : 130, 72, t, C_TEXT, OPAQUE);
    int lo = 999, hi = 0;
    for (int i = 0; i < PRIV_CURVE; i++) {
        if (s->curve[i]) {
            lo = s->curve[i] < lo ? s->curve[i] : lo;
            hi = s->curve[i] > hi ? s->curve[i] : hi;
        }
    }
    const box_t cb = l.portrait ? (box_t){14, 92, l.w - 28, 120} : (box_t){14, 90, l.w - 28, 74};
    if (hi > 0) {
        draw_curve(c, cb, s->curve, PRIV_CURVE, lo - 3, hi + 3, 1.8f);
    }
    gfx_line(c, (float)cb.x + (float)cb.w - 1.0f, (float)cb.y, (float)cb.x + (float)cb.w - 1.0f,
             (float)cb.y + (float)cb.h, 1.2f, C_ACCENT, OPAQUE);
    text_center(c, &FONT_SMALL, cb.x + cb.w - 22, cb.y + cb.h - 6, "Fertig", C_ACCENT);
    const int ty = cb.y + cb.h + 18;
    if (a->priv.model.learned && a->priv.t90_s >= 0) {
        char m1[24], m2[24];
        fmt_duration(m1, sizeof m1, a->priv.t90_s);
        fmt_duration(m2, sizeof m2, s->duration_s);
        snprintf(t, sizeof t, "90 %% bei %s, Fertig bei %s", m1, m2);
    } else if (a->priv.model.learned) {
        snprintf(t, sizeof t, "90 %% wurden nicht erreicht");
    } else {
        snprintf(t, sizeof t, "Wird gelernt (%d von %d)", a->priv.model.n + 1, SOLO_MIN_SESSIONS);
    }
    gfx_text_wrap(c, &FONT_SMALL, 14, ty, l.w - 28, 16, t, C_TEXT_DIM, 2);
    button(c, done_btn(l, 0), "Nicht werten", false, true);
    button(c, done_btn(l, 1), "Speichern", true, true);
}

static void save_session(app_t *a, bool counted, uint32_t now) {
    priv_session_t s = a->priv.last;
    if (!counted) {
        s.flags |= PRIV_EXCLUDED;
    }
    priv_add_session(a->pd, &s);
    a->req_priv_save = true;
    a->screen = APP_PRIV_HOME;
    redraw(a);
    show_toast(a, counted ? "Gespeichert." : "Gespeichert, nicht gewertet.", now);
}

// ------------------------------------------------------------------ Verlauf
static void draw_history(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const priv_data_t *d = a->pd;
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Verlauf", NULL);
    if (d->n == 0) {
        text_center(c, &FONT_SMALL, l.w / 2, l.h / 2, "noch keine Sessions", C_TEXT_DIM);
        return;
    }
    const int rh = l.portrait ? 24 : 20, rows = (l.h - 40) / rh;
    for (int r = 0; r < rows && r < d->n; r++) {
        const priv_session_t *s = &d->s[d->n - 1 - r];
        const int y = 46 + r * rh;
        char m[16], t[64];
        fmt_duration(m, sizeof m, s->duration_s);
        snprintf(t, sizeof t, "%d", r + 1);
        gfx_text(c, &FONT_SMALL, 14, y, t, C_TEXT_FAINT, OPAQUE);
        gfx_text(c, &FONT_SMALL, 40, y, m, C_TEXT, OPAQUE);
        snprintf(t, sizeof t, "%d → %d", s->hr_start, s->hr_peak);
        gfx_text(c, &FONT_SMALL, l.portrait ? 96 : 110, y, t, C_TEXT, OPAQUE);
        if (s->flags & PRIV_EXCLUDED) {
            gfx_text(c, &FONT_SMALL, l.w - 14 - gfx_text_width(&FONT_SMALL, "nicht gewertet"), y,
                     "nicht gewertet", C_TEXT_FAINT, OPAQUE);
        }
    }
}

// ------------------------------------------------------------------ Schnittstelle
void priv_draw(app_t *a) {
    switch (a->screen) {
        case APP_PRIV_PIN:
            draw_pin(a);
            break;
        case APP_PRIV_HOME:
            draw_home(a);
            break;
        case APP_PRIV_SESSION:
            draw_session(a);
            break;
        case APP_PRIV_DONE:
            draw_done(a);
            break;
        case APP_PRIV_HISTORY:
            draw_history(a);
            break;
        default:
            break;
    }
}

void priv_event(app_t *a, ui_event_t ev, uint32_t now) {
    lay_t l = lay(a);
    priv_ui_t *p = &a->priv;
    const bool tap = ev.type == EV_TAP;
    switch (a->screen) {
        case APP_PRIV_PIN:
            pin_event(a, ev, now);
            break;
        case APP_PRIV_HOME:
            if (p->prompt == PROMPT_DELETE) {
                if (tap && hit(prompt_button(l, 1), ev.x, ev.y)) {
                    a->pd->n = 0;
                    a->req_priv_save = true;
                    p->prompt = PROMPT_NONE;
                    redraw(a);
                    show_toast(a, "Sessions gelöscht.", now);
                } else if (tap) {
                    p->prompt = PROMPT_NONE;
                    redraw(a);
                }
                break;
            }
            if (ev.type == EV_LONG || (tap && hit(close_box(l), ev.x, ev.y))) {
                leave(a);
            } else if (tap && hit(home_box(l, 0), ev.x, ev.y)) {
                if (a->usb) {
                    show_toast(a, "Nur mit Batterie: bitte USB trennen.", now);
                } else if (a->mon->status == MON_LEAD_OFF) {
                    show_toast(a, "Die Elektroden haben keinen Kontakt.", now);
                } else {
                    session_start(a, now);
                }
            } else if (tap && hit(home_box(l, 1), ev.x, ev.y) && a->pd->n > 0) {
                a->screen = APP_PRIV_HISTORY;
                redraw(a);
            } else if (tap && hit(home_box(l, 2), ev.x, ev.y)) {
                a->pd->display_mode ^= 1u;
                a->req_priv_save = true;
                redraw(a);
            } else if (tap && hit(home_box(l, 3), ev.x, ev.y)) {
                open_pin(a, PIN_SET1, true);
            } else if (tap && hit(home_box(l, 4), ev.x, ev.y)) {
                p->prompt = PROMPT_DELETE;
                redraw(a);
            }
            break;
        case APP_PRIV_SESSION:
            if (tap && hit(done_button(l), ev.x, ev.y)) {
                session_finish(a, now);
            } else if (tap && hit(close_box(l), ev.x, ev.y)) {  // abbrechen, nichts wird gespeichert
                a->screen = APP_PRIV_HOME;
                redraw(a);
                show_toast(a, "Abgebrochen, nichts gespeichert.", now);
            }
            break;
        case APP_PRIV_DONE:
            if (tap && hit(done_btn(l, 1), ev.x, ev.y)) {
                save_session(a, true, now);
            } else if (tap && hit(done_btn(l, 0), ev.x, ev.y)) {
                save_session(a, false, now);
            } else if (tap && hit(close_box(l), ev.x, ev.y)) {
                a->screen = APP_PRIV_HOME;
                redraw(a);
                show_toast(a, "Verworfen.", now);
            }
            break;
        case APP_PRIV_HISTORY:
            if (ev.type == EV_LONG || (tap && hit(close_box(l), ev.x, ev.y))) {
                a->screen = APP_PRIV_HOME;
                redraw(a);
            }
            break;
        default:
            break;
    }
}

// Sekundentakt der Session; Bildschirm erneuern
void priv_frame(app_t *a, uint32_t now) {
    priv_ui_t *p = &a->priv;
    if (a->screen == APP_PRIV_PIN && now < p->locked_until && now >= p->draw_at) {
        p->draw_at = now + 1000u;  // Sperrzeit herunterzählen
        redraw(a);
        return;
    }
    if (a->screen != APP_PRIV_SESSION) {
        return;
    }
    if (a->usb) {  // USB gesteckt: Sicherheit geht vor, die Session endet ohne Speichern
        a->screen = APP_PRIV_HOME;
        redraw(a);
        show_toast(a, "USB erkannt – Session abgebrochen, nichts gespeichert.", now);
        return;
    }
    while (now - p->sec_at >= 1000u) {
        p->sec_at += 1000u;
        const bool ok = a->mon->bpm > 0 && a->mon->bpm < 250 && a->mon->status != MON_LEAD_OFF &&
                        a->mon->status != MON_CLIPPED;
        p->hr[p->n_s++] = ok ? (uint8_t)a->mon->bpm : 0;
        if (p->n_s >= SOLO_MAX_S) {
            session_finish(a, now);
            return;
        }
    }
    if (now >= p->draw_at) {
        p->draw_at = now + 1000u;
        p->start_hr = p->n_s >= 10 ? solo_start(p->hr, p->n_s) : NAN;
        const float cur = solo_median5(p->hr, p->n_s, p->n_s - 1);
        p->percent = isnan(p->start_hr) || isnan(cur) ? -1 : solo_percent(&p->model, p->start_hr, cur);
        redraw(a);
    }
}
