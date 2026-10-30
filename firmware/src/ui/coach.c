// Atem-Coach: ein Kreis atmet vor (Ein- und Ausatmen gleich lang), dazu läuft der Puls mit.
// Eine Übung, keine Messung: Es wird nichts gespeichert.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_int.h"
#include "assets.h"
#include "theme.h"

#define TEMPO_MIN 4
#define TEMPO_MAX 10
#define FRAME_MS 100u

void coach_open(app_t *a, uint32_t now) {
    coach_t *k = &a->coach;
    if (k->tempo < TEMPO_MIN || k->tempo > TEMPO_MAX) {
        k->tempo = 6;
    }
    if (k->dur_min != 2 && k->dur_min != 5) {
        k->dur_min = 2;
    }
    k->t0 = now;
    k->drawn_at = 0;
    k->pulse_at = now;
    k->n_pulse = 0;
    memset(k->pulse, 0, sizeof k->pulse);
    a->screen = APP_COACH;
    a->mon->draw = false;
}

// Gerüst der Anordnung: Kreis links (quer) oder oben (hochkant), Einstellungen daneben/darunter
typedef struct {
    float cx, cy, r;
    box_t minus, plus, seg2, seg5, curve;
    int tempo_x, tempo_y, dauer_y, puls_y, foot_y;
} coach_lay_t;

static coach_lay_t layout(lay_t l) {
    coach_lay_t c;
    if (l.portrait) {
        c.cx = (float)l.w / 2.0f;
        c.cy = 104.0f;
        c.r = 66.0f;
        c.foot_y = 190;
        c.minus = (box_t){92, 202, 34, 30};
        c.plus = (box_t){l.w - 14 - 34, 202, 34, 30};
        c.tempo_x = (c.minus.x + c.minus.w + c.plus.x) / 2;
        c.tempo_y = 223;
        c.seg2 = (box_t){92, 244, 62, 26};
        c.seg5 = (box_t){154, 244, 62, 26};
        c.dauer_y = 262;
        c.curve = (box_t){92, 280, l.w - 92 - 14, 32};
        c.puls_y = 300;
    } else {
        c.cx = 92.0f;
        c.cy = 126.0f;
        c.r = 66.0f;
        c.foot_y = 218;
        c.minus = (box_t){190, 54, 34, 30};
        c.plus = (box_t){280, 54, 34, 30};
        c.tempo_x = 252;
        c.tempo_y = 75;
        c.seg2 = (box_t){190, 110, 62, 24};
        c.seg5 = (box_t){252, 110, 62, 24};
        c.dauer_y = 104;
        c.curve = (box_t){190, 162, 124, 32};
        c.puls_y = 156;
    }
    return c;
}

static void segment(canvas_t *c, box_t a, box_t b, int sel, const char *la, const char *lb) {
    gfx_round_rect(c, a.x, a.y, a.w + b.w, a.h, a.h / 2, gfx_mix(C_PANEL, C_BG, 120), OPAQUE);
    const box_t s = sel == 0 ? a : b;
    gfx_round_rect(c, s.x + 2, s.y + 2, s.w - 2, s.h - 4, (s.h - 4) / 2, C_ACCENT, OPAQUE);
    text_center(c, &FONT_SMALL, a.x + a.w / 2 + 1, a.y + a.h / 2 + 5, la,
                sel == 0 ? 0xFFFFFFu : C_TEXT_DIM);
    text_center(c, &FONT_SMALL, b.x + b.w / 2 - 1, b.y + b.h / 2 + 5, lb,
                sel == 1 ? 0xFFFFFFu : C_TEXT_DIM);
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

void coach_draw(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    coach_t *k = &a->coach;
    const coach_lay_t cl = layout(l);
    const float elapsed = (float)(a->clock - k->t0) / 1000.0f;
    const float period = 60.0f / (float)k->tempo, half = period / 2.0f;
    const float pos = fmodf(elapsed, period);
    const bool inhale = pos < half;
    const float x = (inhale ? pos : pos - half) / half;
    const float ease = 0.5f - 0.5f * cosf(3.14159265f * x);
    const float size = inhale ? ease : 1.0f - ease;  // 0 klein, 1 groß
    const int left = (int)ceilf(half - (inhale ? pos : pos - half));

    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    char t[24];
    const int total = k->dur_min * 60;
    fmt_duration(t, sizeof t, (double)(total - (int)elapsed) > 0 ? (double)(total - (int)elapsed) : 0);
    header(a, "Atem-Coach", t);

    // Kreis: Rand = größter Umfang, innen die aktuelle Größe, Bogen = Fortschritt der Übung
    gfx_round_frame(c, (int)(cl.cx - cl.r), (int)(cl.cy - cl.r), (int)(2 * cl.r), (int)(2 * cl.r),
                    (int)cl.r, 1.5f, C_PANEL_EDGE);
    gfx_round_frame(c, (int)(cl.cx - 30), (int)(cl.cy - 30), 60, 60, 30, 1.0f, C_PANEL_EDGE);
    const float rad = 30.0f + (cl.r - 8.0f - 30.0f) * size;
    if (GLOW) {
        gfx_glow(c, cl.cx, cl.cy, rad + 22.0f, C_ACCENT_2, 60);
    }
    gfx_disc(c, cl.cx, cl.cy, rad, gfx_mix(C_BG, C_ACCENT_2, 205), OPAQUE);
    const float done = fminf(elapsed / (float)total, 1.0f);
    if (done > 0.04f) {
        arc(c, cl.cx, cl.cy, cl.r, 1.5708f, 1.5708f - done * 2.0f * 3.14159265f, 3.0f, C_ACCENT_2,
            C_ACCENT);
    }
    text_center(c, &FONT_LABEL, (int)cl.cx, (int)cl.cy - 2, inhale ? "Einatmen" : "Ausatmen",
                0xFFFFFFu);
    snprintf(t, sizeof t, "%d", left > 0 ? left : 1);
    text_center(c, &FONT_VALUE, (int)cl.cx, (int)cl.cy + 26, t, 0xFFFFFFu);

    // Einstellungen
    const int lx = l.portrait ? 14 : 190;
    gfx_text(c, &FONT_SMALL, lx, l.portrait ? cl.tempo_y + 5 : 46, "Tempo", C_TEXT_DIM, OPAQUE);
    button(c, cl.minus, "–", false, k->tempo > TEMPO_MIN);
    button(c, cl.plus, "+", false, k->tempo < TEMPO_MAX);
    snprintf(t, sizeof t, "%d /min", k->tempo);
    text_center(c, &FONT_LABEL, cl.tempo_x, cl.tempo_y + (l.portrait ? 0 : 0), t, C_TEXT);
    gfx_text(c, &FONT_SMALL, lx, l.portrait ? cl.dauer_y : cl.dauer_y, "Dauer", C_TEXT_DIM,
             OPAQUE);
    segment(c, cl.seg2, cl.seg5, k->dur_min == 5, "2 min", "5 min");

    // Puls der letzten Minute
    gfx_text(c, &FONT_SMALL, lx, cl.puls_y, "Puls", C_TEXT_DIM, OPAQUE);
    int lo = 999, hi = 0;
    for (int i = 0; i < k->n_pulse; i++) {
        if (k->pulse[i]) {
            lo = k->pulse[i] < lo ? k->pulse[i] : lo;
            hi = k->pulse[i] > hi ? k->pulse[i] : hi;
        }
    }
    if (hi > 0) {
        if (lo == hi) {
            snprintf(t, sizeof t, "%d", lo);
        } else {
            snprintf(t, sizeof t, "%d–%d", lo, hi);
        }
        if (l.portrait) {  // links unter dem Wort, die Kurve liegt rechts davon
            gfx_text(c, &FONT_SMALL, lx, cl.puls_y + 18, t, C_TEXT, OPAQUE);
        } else {
            gfx_text(c, &FONT_SMALL, 314 - gfx_text_width(&FONT_SMALL, t), cl.puls_y, t, C_TEXT,
                     OPAQUE);
        }
        const float span = fmaxf((float)(hi - lo), 6.0f), mid = (float)(hi + lo) / 2.0f;
        float px0 = 0, py0 = 0;
        bool have = false;
        for (int i = 0; i < k->n_pulse; i++) {
            if (!k->pulse[i]) {
                have = false;
                continue;
            }
            const float xx = (float)cl.curve.x + (float)(cl.curve.w - 1) * (float)i /
                                                     (float)(k->n_pulse > 1 ? k->n_pulse - 1 : 1);
            const float yy = (float)cl.curve.y + (float)cl.curve.h / 2.0f -
                             ((float)k->pulse[i] - mid) / span * (float)cl.curve.h * 0.8f;
            if (have) {
                gfx_line(c, px0, py0, xx, yy, 1.6f, C_TRACE, OPAQUE);
            }
            px0 = xx;
            py0 = yy;
            have = true;
        }
    }
    snprintf(t, sizeof t, "Ein %d s · Aus %d s", (int)lrintf(half), (int)lrintf(half));
    text_center(c, &FONT_SMALL, l.portrait ? l.w / 2 : 160, cl.foot_y, t, C_TEXT_DIM);
    if (!l.portrait) {
        text_center(c, &FONT_SMALL, 160, 234, "Der Puls folgt dem Atem", C_TEXT_FAINT);
    }
}

// Zeitschritt: Puls je Sekunde aufnehmen, Ende erkennen, Bild alle 100 ms erneuern.
// Liefert true, wenn die Übung zu Ende ist.
bool coach_frame(app_t *a, uint32_t now) {
    coach_t *k = &a->coach;
    if (now - k->pulse_at >= 1000u) {
        k->pulse_at += 1000u;
        if (k->n_pulse >= (int)sizeof k->pulse) {
            memmove(k->pulse, k->pulse + 1, sizeof k->pulse - 1);
            k->n_pulse--;
        }
        k->pulse[k->n_pulse++] = (uint8_t)(a->mon->bpm > 0 && a->mon->bpm < 250 ? a->mon->bpm : 0);
    }
    if ((now - k->t0) / 1000u >= (uint32_t)k->dur_min * 60u) {
        return true;
    }
    if (now - k->drawn_at >= FRAME_MS) {
        k->drawn_at = now;
        a->req_push_all = true;
        coach_draw(a);
    }
    return false;
}

// Tippen: Tempo, Dauer; true = Bildschirm verlassen
bool coach_tap(app_t *a, int x, int y, uint32_t now) {
    lay_t l = lay(a);
    coach_t *k = &a->coach;
    const coach_lay_t cl = layout(l);
    if (hit(close_box(l), x, y)) {
        return true;
    }
    if (hit(cl.minus, x, y) && k->tempo > TEMPO_MIN) {
        k->tempo--;
        k->t0 = now;
    } else if (hit(cl.plus, x, y) && k->tempo < TEMPO_MAX) {
        k->tempo++;
        k->t0 = now;
    } else if (hit(cl.seg2, x, y) && k->dur_min != 2) {
        k->dur_min = 2;
        k->t0 = now;
    } else if (hit(cl.seg5, x, y) && k->dur_min != 5) {
        k->dur_min = 5;
        k->t0 = now;
    }
    return false;
}
