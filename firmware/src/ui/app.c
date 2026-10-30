#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_int.h"
#include "assets.h"
#include "avatar.h"
#include "protocol.h"
#include "theme.h"
#include "touchcal.h"

typedef struct {
    const theme_t *theme;
    int backlight;  // Promille
    const char *name;
} look_t;

static const look_t LOOKS[] = {
    {&THEME_DAY, 1000, "Tag"},
    {&THEME_NIGHT, 1000, "Nacht"},
};
#define N_LOOKS (int)(sizeof LOOKS / sizeof LOOKS[0])

#define TOAST_MS 2600u

static const char *PAGE_NAMES[APP_PAGES] = {"Übersicht", "Check",      "Verlauf",
                                             "HRV",       "Schlagform", "Hinweise"};

bool hit(box_t b, int x, int y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

lay_t lay(const app_t *a) {
    lay_t l = {a->c->w, a->c->h, a->c->h > a->c->w, 0};
    l.footer_y = l.portrait ? l.h - 40 : l.h - 36;
    return l;
}

// ------------------------------------------------------------------ Formatierung
// Zahl mit Dezimalkomma; digits Nachkommastellen
void fmt_num(char *buf, size_t n, double v, int digits) {
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

void fmt_duration(char *buf, size_t n, double s) {
    int t = (int)lrint(s < 0 ? 0 : s > 5999 ? 5999 : s);
    snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

// ------------------------------------------------------------------ Bausteine
void text_center(canvas_t *c, const font_t *f, int cx, int baseline, const char *s,
                        uint32_t col) {
    gfx_text(c, f, cx - gfx_text_width(f, s) / 2, baseline, s, col, OPAQUE);
}

void button(canvas_t *c, box_t b, const char *label, bool primary, bool enabled) {
    if (primary) {
        gfx_round_rect(c, b.x, b.y, b.w, b.h, 11, C_ACCENT, OPAQUE);
    } else {
        gfx_round_rect(c, b.x, b.y, b.w, b.h, 10, gfx_mix(C_PANEL, C_TEXT, 14), OPAQUE);
        gfx_round_frame(c, b.x, b.y, b.w, b.h, 10, 1.0f, C_PANEL_EDGE);
    }
    uint32_t col = primary ? 0xFFFFFFu : enabled ? C_TEXT : C_TEXT_FAINT;
    text_center(c, &FONT_SMALL, b.x + b.w / 2, b.y + b.h / 2 + 5, label, col);
}

void close_icon(canvas_t *c, int cx, int cy, uint32_t col) {
    gfx_line(c, (float)cx - 5.0f, (float)cy - 5.0f, (float)cx + 5.0f, (float)cy + 5.0f, 2.0f, col,
             OPAQUE);
    gfx_line(c, (float)cx - 5.0f, (float)cy + 5.0f, (float)cx + 5.0f, (float)cy - 5.0f, 2.0f, col,
             OPAQUE);
}

void dim_screen(canvas_t *c) {
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            gfx_blend(c, x, y, C_BG, 170);
        }
    }
}

// Kopfleiste mit Titel, optional Untertitel und Schließen-Kreuz
void header(app_t *a, const char *title, const char *sub) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_fill(c, 0, 0, l.w, HEADER_H, C_PANEL);
    gfx_fill(c, 0, HEADER_H - 1, l.w, 1, C_PANEL_EDGE);
    int x = gfx_text(c, &FONT_LABEL, 10, 19, title, C_TEXT, OPAQUE);
    if (sub) {
        gfx_text(c, &FONT_SMALL, x + 8, 18, sub, C_TEXT_DIM, OPAQUE);
    }
    close_icon(c, l.w - 20, 13, C_TEXT_DIM);
}

box_t close_box(lay_t l) {
    return (box_t){l.w - 44, 0, 44, 36};
}

// mehrzeiliger Text, jede Zeile mittig
static void lines_center(canvas_t *c, int cx, int baseline, int line_h, const char *const *lines,
                         int n, uint32_t col) {
    for (int i = 0; i < n; i++) {
        text_center(c, &FONT_SMALL, cx, baseline + i * line_h, lines[i], col);
    }
}

// ------------------------------------------------------------------ Anzeige und Live
static void apply_look(app_t *a) {
    if (a->ui->look >= N_LOOKS) {
        a->ui->look = N_LOOKS - 1;  // frühere Dimmstufen werden zu „Nacht“
    }
    g_theme = LOOKS[a->ui->look].theme;
    a->req_backlight = LOOKS[a->ui->look].backlight;
}

int app_backlight(const app_t *a) {
    return LOOKS[a->ui->look < N_LOOKS ? a->ui->look : N_LOOKS - 1].backlight;
}

// Bildmaße zur Ausrichtung; die Live-Ansicht teilt sich neu auf
static void apply_orientation(app_t *a) {
    bool portrait = a->ui->rotation == 1;
    a->c->w = portrait ? SCREEN_H : SCREEN_W;
    a->c->h = portrait ? SCREEN_W : SCREEN_H;
    monitor_layout(a->mon);
}

static void status_icon(canvas_t *c, float cx, float cy, chk_status_t st);

void go_live(app_t *a) {
    a->screen = APP_LIVE;
    a->mon->draw = true;
    a->mon->menu_button = a->touch_xy;
    monitor_draw_all(a->mon);
    a->req_push_all = true;
}

void show_toast(app_t *a, const char *text, uint32_t now) {
    snprintf(a->toast, sizeof a->toast, "%s", text);
    a->toast_until = now + TOAST_MS;
    a->toast_shown = true;
    redraw(a);
}

static void draw_toast(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const int bw = l.w - 32;
    int lines = gfx_text_wrap(NULL, &FONT_SMALL, 0, 0, bw - 24, 16, a->toast, C_TEXT, 3);
    const box_t b = {16, a->screen == APP_LIVE ? 34 : l.h - 100, bw, 12 + lines * 16 + 12};
    gfx_round_rect(c, b.x, b.y, b.w, b.h, 12, C_PANEL, 245);
    gfx_round_frame(c, b.x, b.y, b.w, b.h, 12, 1.5f, C_ACCENT);
    gfx_text_wrap(c, &FONT_SMALL, b.x + 12, b.y + 24, bw - 24, 16, a->toast, C_TEXT, 3);
}

// ------------------------------------------------------------------ Menü
// Oben der Umschalter EKG | Atmung. Zeilen: 0 = Messen, Zeile 1 = Letztes Ergebnis (EKG: volle
// Breite, Atmung: links) und 5 = Atem-Coach (nur Atmung, rechts), Zeile 2 = 2 Einstellungen
// (links) und 4 Elektroden (rechts), Zeile 3 = 3 Person.
static box_t menu_panel(lay_t l) {
    return l.portrait ? (box_t){10, 40, l.w - 20, 244} : (box_t){12, 8, l.w - 24, 224};
}

static box_t menu_item(lay_t l, int i, app_mode_t mode) {
    box_t p = menu_panel(l);
    const int x = p.x + 12, w = p.w - 24, ph = l.portrait ? 50 : 46, h = l.portrait ? 40 : 36;
    if (i == 0) {
        return (box_t){x, p.y + 38, w, ph};
    }
    const int half = (w - 8) / 2;
    const int row = i == 5 ? 1 : i == 4 ? 2 : i;
    const int y = p.y + 38 + ph + 8 + (row - 1) * (h + 8);
    if (i == 1) {
        return (box_t){x, y, mode == MODE_EKG ? w : half, h};
    }
    if (i == 5 || i == 4) {
        return (box_t){x + half + 8, y, w - half - 8, h};
    }
    if (i == 2) {
        return (box_t){x, y, half, h};
    }
    return (box_t){x, y, w, h};
}

// Umschalter oben links im Menü: 0 = EKG, 1 = Atmung
static box_t menu_mode(lay_t l, int i) {
    box_t p = menu_panel(l);
    return (box_t){p.x + 12 + i * 75, p.y + 6, 75, 28};
}

static box_t menu_close(lay_t l) {
    box_t p = menu_panel(l);
    return (box_t){p.x + p.w - 50, p.y, 50, 36};
}

// fresh_bg: Live-Ansicht dahinter neu zeichnen (nach einem Wechsel der Anzeige); beim
// Öffnen bleibt das letzte Bild mit Kurve stehen
static void draw_menu(app_t *a, bool fresh_bg) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    if (fresh_bg) {
        monitor_draw_all(a->mon);
    }
    dim_screen(c);
    const box_t p = menu_panel(l);
    gfx_round_rect(c, p.x, p.y, p.w, p.h, 14, C_PANEL, OPAQUE);
    gfx_round_frame(c, p.x, p.y, p.w, p.h, 14, 1.0f, C_PANEL_EDGE);
    // Umschalter EKG | Atmung
    {
        const box_t e = menu_mode(l, 0), b = menu_mode(l, 1);
        gfx_round_rect(c, e.x, e.y, e.w + b.w, e.h, e.h / 2, gfx_mix(C_PANEL, C_BG, 120), OPAQUE);
        const box_t sel = a->mode == MODE_EKG ? e : b;
        gfx_round_rect(c, sel.x + 2, sel.y + 2, sel.w - 2, sel.h - 4, (sel.h - 4) / 2, C_ACCENT,
                       OPAQUE);
        text_center(c, &FONT_SMALL, e.x + e.w / 2 + 1, e.y + e.h / 2 + 5, "EKG",
                    a->mode == MODE_EKG ? 0xFFFFFFu : C_TEXT_DIM);
        text_center(c, &FONT_SMALL, b.x + b.w / 2 - 1, b.y + b.h / 2 + 5, "Atmung",
                    a->mode == MODE_BREATH ? 0xFFFFFFu : C_TEXT_DIM);
    }
    close_icon(c, p.x + p.w - 24, p.y + 19, C_TEXT_DIM);

    const box_t m = menu_item(l, 0, a->mode);
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 12, C_ACCENT, OPAQUE);
    float cy = (float)m.y + (float)m.h / 2.0f;
    if (a->mode == MODE_EKG) {
        gfx_triangle(c, (float)m.x + 18.0f, cy - 10.0f, (float)m.x + 18.0f, cy + 10.0f,
                     (float)m.x + 34.0f, cy, 0xFFFFFFu, OPAQUE);
    } else {
        gfx_disc(c, (float)m.x + 26.0f, cy, 11.0f, 0xFFFFFFu, 150);
        gfx_disc(c, (float)m.x + 26.0f, cy, 6.0f, 0xFFFFFFu, OPAQUE);
    }
    char dur[16], sub[40];
    fmt_duration(dur, sizeof dur, ANA_MAX_S);
    const char *title = a->mode == MODE_EKG ? "Messen & auswerten" : "Atmung messen";
    if (l.portrait) {  // zweizeilig, damit es in 200 Pixel passt
        gfx_text(c, &FONT_LABEL, m.x + 46, m.y + 22, a->mode == MODE_EKG ? "Messen &" : "Atmung",
                 0xFFFFFFu, OPAQUE);
        snprintf(sub, sizeof sub, "%s · %s", a->mode == MODE_EKG ? "auswerten" : "messen", dur);
        gfx_text(c, &FONT_SMALL, m.x + 46, m.y + 40, sub, 0xFFFFFFu, 220);
    } else {
        gfx_text(c, &FONT_LABEL, m.x + 46, m.y + m.h / 2 + 6, title, 0xFFFFFFu, OPAQUE);
        gfx_text(c, &FONT_SMALL, m.x + m.w - 12 - gfx_text_width(&FONT_SMALL, dur),
                 m.y + m.h / 2 + 5, dur, 0xFFFFFFu, 200);
    }
    const bool have = a->mode == MODE_EKG ? (a->has_result || a->result_status) : a->has_breath;
    button(c, menu_item(l, 1, a->mode), l.portrait && a->mode == MODE_BREATH ? "Ergebnis"
                                        : "Letztes Ergebnis", false, have);
    if (a->mode == MODE_BREATH) {
        const box_t cb = menu_item(l, 5, a->mode);
        button(c, cb, l.portrait ? "Coach" : "Atem-Coach", false, true);
        gfx_disc(c, (float)cb.x + 14.0f, (float)cb.y + (float)cb.h / 2.0f, 4.5f, C_ACCENT_2,
                 OPAQUE);
    }
    button(c, menu_item(l, 2, a->mode), "Einstellungen", false, true);
    button(c, menu_item(l, 4, a->mode), "Elektroden", false, true);
    const profile_t *who = settings_person(a->ui);
    char label[PROFILE_NAME_BYTES + 12];
    snprintf(label, sizeof label, "Person: %s", who ? who->name : "Gast");
    button(c, menu_item(l, 3, a->mode), label, false, true);
}

// ------------------------------------------------------------------ Einstellungen
enum { SET_LOOK, SET_ORIENT, SET_GAIN, SET_SPEED, SET_AVATAR, SET_CAL, SET_INFO, SET_ROWS };

static box_t settings_row(lay_t l, int i) {
    const int rh = l.portrait ? 40 : 30;
    return (box_t){0, HEADER_H + 2 + i * rh, l.w, rh};
}

static void draw_settings(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Einstellungen", NULL);
    for (int i = 0; i < SET_ROWS; i++) {
        box_t r = settings_row(l, i);
        static const char *LABELS[SET_ROWS] = {"Anzeige",  "Ausrichtung",      "Verstärkung",
                                               "Vorschub", "Avatar",           "Touch einrichten",
                                               "Info"};
        const char *value = "›";
        switch (i) {
            case SET_LOOK:
                value = LOOKS[a->ui->look].name;
                break;
            case SET_ORIENT:
                value = a->ui->rotation ? "Hochkant" : "Quer";
                break;
            case SET_GAIN:
                value = monitor_gain_name(a->ui->gain);
                break;
            case SET_SPEED:
                value = monitor_speed_name(a->ui->speed);
                break;
            case SET_AVATAR:
                value = a->ui->avatar ? "Standard" : "Profil";
                break;
            default:
                break;
        }
        int base = r.y + r.h / 2 + 6;
        gfx_text(c, &FONT_LABEL, 14, base, LABELS[i], C_TEXT, OPAQUE);
        const font_t *vf = i < SET_CAL ? &FONT_SMALL : &FONT_LABEL;
        uint32_t vc = i < SET_CAL ? C_ACCENT : C_TEXT_DIM;
        gfx_text(c, vf, l.w - 14 - gfx_text_width(vf, value), base - (i < SET_CAL ? 1 : 0), value,
                 vc, OPAQUE);
        gfx_fill(c, 10, r.y + r.h - 1, l.w - 20, 1, C_PANEL_EDGE);
    }
}

// ------------------------------------------------------------------ Profile
// Person wählen (Profil 1, Profil 2, Gast) und die Angaben eines Profils bearbeiten
enum { F_NAME, F_AGE, F_SEX, F_HEIGHT, F_WEIGHT, F_COUNT };

static box_t prof_row(lay_t l, int i) {
    const int rh = l.portrait ? 70 : 56;
    return (box_t){0, HEADER_H + 4 + i * rh, l.w, rh};
}

static box_t prof_edit_box(lay_t l, int i) {
    box_t r = prof_row(l, i);
    return (box_t){l.w - 84, r.y, 84, r.h};
}

static box_t edit_row(lay_t l, int i) {
    const int rh = l.portrait ? 40 : 30;
    return (box_t){0, HEADER_H + 2 + i * rh, l.w, rh};
}

// 0 = Löschen, 1 = Fertig
static box_t edit_button(lay_t l, int i) {
    const int w = (l.w - 30) / 2, h = l.portrait ? 36 : 32;
    return (box_t){10 + i * (w + 10), l.portrait ? l.h - 48 : l.h - 38, w, h};
}

static int active_person(const app_t *a) {
    return settings_person(a->ui) ? a->ui->person : PERSON_GUEST;
}

static void draw_profiles(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Person", NULL);
    for (int i = 0; i <= PERSON_GUEST; i++) {
        const box_t r = prof_row(l, i);
        const bool guest = i == PERSON_GUEST;
        const profile_t *p = guest ? NULL : &a->ui->profiles[i];
        const bool used = guest || profile_used(p);
        const bool active = active_person(a) == i;
        const float cx = 24.0f, cy = (float)r.y + (float)r.h / 2.0f;
        gfx_disc(c, cx, cy, 9.0f, active ? C_ACCENT : C_TEXT_FAINT, OPAQUE);
        gfx_disc(c, cx, cy, 7.0f, C_BG, OPAQUE);
        if (active) {
            gfx_disc(c, cx, cy, 4.2f, C_ACCENT, OPAQUE);
        }
        const int base = r.y + r.h / 2 - 3;
        if (!used) {
            gfx_text(c, &FONT_LABEL, 46, r.y + r.h / 2 + 6, "Profil anlegen", C_TEXT_DIM, OPAQUE);
            gfx_text(c, &FONT_LABEL, l.w - 26, r.y + r.h / 2 + 6, "+", C_ACCENT, OPAQUE);
        } else {
            char sum[64];
            if (guest) {
                snprintf(sum, sizeof sum, "ohne Angaben");
            } else {
                profile_summary(p, sum, sizeof sum);
            }
            gfx_text(c, &FONT_LABEL, 46, base, guest ? "Gast" : p->name, C_TEXT, OPAQUE);
            gfx_text(c, &FONT_SMALL, 46, base + 19, sum, C_TEXT_DIM, OPAQUE);
            if (!guest) {
                const char *edit = "Ändern ›";
                gfx_text(c, &FONT_SMALL, l.w - 12 - gfx_text_width(&FONT_SMALL, edit),
                         r.y + r.h / 2 + 5, edit, C_ACCENT, OPAQUE);
            }
        }
        gfx_fill(c, 10, r.y + r.h - 1, l.w - 20, 1, C_PANEL_EDGE);
    }
    const box_t last = prof_row(l, PERSON_GUEST);
    text_center(c, &FONT_SMALL, l.w / 2, last.y + last.h + 18, "Antippen: Person wählen",
                C_TEXT_FAINT);
}

static void draw_profile_edit(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const profile_t *p = &a->ui->profiles[a->edit_idx];
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    char title[24];
    snprintf(title, sizeof title, "Profil %d", a->edit_idx + 1);
    header(a, title, NULL);
    static const char *const LABELS[F_COUNT] = {"Name", "Alter", "Geschlecht", "Größe",
                                                "Gewicht"};
    for (int i = 0; i < F_COUNT; i++) {
        const box_t r = edit_row(l, i);
        char v[PROFILE_NAME_BYTES + 8] = "";
        switch (i) {
            case F_NAME:
                snprintf(v, sizeof v, "%s", p->name);
                break;
            case F_AGE:
                if (p->age) {
                    snprintf(v, sizeof v, "%u Jahre", p->age);
                }
                break;
            case F_SEX:
                snprintf(v, sizeof v, "%s",
                         p->sex == SEX_MALE ? "männlich" : p->sex == SEX_FEMALE ? "weiblich" : "");
                break;
            case F_HEIGHT:
                if (p->height_cm) {
                    snprintf(v, sizeof v, "%u cm", p->height_cm);
                }
                break;
            default:
                if (p->weight_kg) {
                    snprintf(v, sizeof v, "%u kg", p->weight_kg);
                }
                break;
        }
        const int base = r.y + r.h / 2 + 6;
        gfx_text(c, &FONT_LABEL, 14, base, LABELS[i], C_TEXT, OPAQUE);
        const bool set = v[0] != '\0';
        const char *shown = set ? v : "–";
        gfx_text(c, &FONT_SMALL, l.w - 14 - gfx_text_width(&FONT_SMALL, shown), base - 1, shown,
                 set ? C_ACCENT : C_TEXT_FAINT, OPAQUE);
        gfx_fill(c, 10, r.y + r.h - 1, l.w - 20, 1, C_PANEL_EDGE);
    }
    const int bmi = profile_bmi10(p);
    if (bmi >= 0) {
        char b[48];
        snprintf(b, sizeof b, "BMI %d,%d (nur zur Info)", bmi / 10, bmi % 10);
        gfx_text(c, &FONT_SMALL, 14, edit_row(l, F_COUNT).y + 16, b, C_TEXT_DIM, OPAQUE);
    }
    button(c, edit_button(l, 0), "Löschen", false, profile_used(p));
    button(c, edit_button(l, 1), "Fertig", true, true);
}

static void back_to_menu(app_t *a) {
    a->screen = APP_MENU;
    a->mon->draw = false;
    redraw(a);
}

static void open_field(app_t *a, int field) {
    const profile_t *p = &a->ui->profiles[a->edit_idx];
    switch (field) {
        case F_NAME:
            kb_open_text(&a->kb, "Name", p->name, PROFILE_NAME_CHARS);
            break;
        case F_AGE:
            kb_open_number(&a->kb, "Alter", "Jahre", p->age, 1, PROFILE_AGE_MAX);
            break;
        case F_HEIGHT:
            kb_open_number(&a->kb, "Größe", "cm", p->height_cm, PROFILE_HEIGHT_MIN,
                           PROFILE_HEIGHT_MAX);
            break;
        default:
            kb_open_number(&a->kb, "Gewicht", "kg", p->weight_kg, PROFILE_WEIGHT_MIN,
                           PROFILE_WEIGHT_MAX);
            break;
    }
    a->kb_field = field;
    a->screen = APP_KEYBOARD;
    redraw(a);
}

static void keyboard_done(app_t *a) {
    profile_t *p = &a->ui->profiles[a->edit_idx];
    switch (a->kb_field) {
        case F_NAME:
            snprintf(p->name, sizeof p->name, "%s", a->kb.buf);
            break;
        case F_AGE:
            p->age = (uint8_t)kb_value(&a->kb);
            break;
        case F_HEIGHT:
            p->height_cm = (uint8_t)kb_value(&a->kb);
            break;
        default:
            p->weight_kg = (uint8_t)kb_value(&a->kb);
            break;
    }
    profile_sanitize(p);
    a->req_save = true;
}

static void open_editor(app_t *a, int idx) {
    a->edit_idx = idx;
    a->edit_new = !profile_used(&a->ui->profiles[idx]);
    if (a->edit_new) {  // neues Profil: gleich nach dem Namen fragen
        profile_clear(&a->ui->profiles[idx]);
        open_field(a, F_NAME);
    } else {
        a->screen = APP_PROFILE_EDIT;
        redraw(a);
    }
}

// Editor verlassen: ohne Namen gibt es kein Profil; ein neues Profil wird gleich gewählt
static void close_editor(app_t *a) {
    profile_t *p = &a->ui->profiles[a->edit_idx];
    if (!profile_used(p)) {
        profile_clear(p);
        if (a->ui->person == a->edit_idx) {
            a->ui->person = PERSON_GUEST;
        }
    } else if (a->edit_new) {
        a->ui->person = (uint8_t)a->edit_idx;
    }
    a->edit_new = false;
    a->req_save = true;
    a->screen = APP_PROFILES;
    redraw(a);
}

static void select_person(app_t *a, int person) {
    a->ui->person = (uint8_t)person;
    a->req_save = true;
    back_to_menu(a);
}

// ------------------------------------------------------------------ Elektroden-Anleitung
#define LEADS_AUTO_MS 4000u  // so lange ohne Kontakt, bis die Anleitung von selbst erscheint
#define LEADS_OK_MS 1500u    // so lange mit Kontakt, bis sie sich wieder schließt

// Farben der Kabel (RA, LA, RL). Klon-Kabel sind nicht einheitlich gefärbt: das Schema lässt sich
// in der Anleitung umschalten.
typedef struct {
    const char *name[3];
    uint32_t rgb[3];
} cable_t;

static const cable_t CABLES[CABLE_PRESETS] = {
    {{"Rot", "Gelb", "Grün"}, {0xE53935u, 0xF2C200u, 0x2EAD4Bu}},
    {{"Weiß", "Schwarz", "Grün"}, {0xF5F5F5u, 0x202020u, 0x2EAD4Bu}},
    {{"Rot", "Gelb", "Schwarz"}, {0xE53935u, 0xF2C200u, 0x202020u}},
};

static const char *const SITE_NAME[3] = {"RA", "LA", "RL"};
static const char *const SITE_WHERE[3] = {"rechts unter dem Schlüsselbein",
                                          "links am unteren Rippenbogen",
                                          "rechts am unteren Rippenbogen"};

static box_t leads_bar(lay_t l) {
    const int h = l.portrait ? 44 : 36;
    return (box_t){0, l.h - h, l.w, h};
}

// 0 = Farben, 1 = Fertig
static box_t leads_button(lay_t l, int i) {
    const box_t bar = leads_bar(l);
    const int w = (l.w - 30) / 2;
    return (box_t){10 + i * (w + 10), bar.y + 5, w, bar.h - 10};
}

// Markierung auf der Figur: heller Rand für jeden Untergrund, roter Ring bei fehlendem Kontakt
static void site_marker(canvas_t *c, float x, float y, float r, uint32_t rgb, bool off) {
    if (off) {
        gfx_disc(c, x, y, r + 5.5f, C_ACCENT, OPAQUE);
        gfx_disc(c, x, y, r + 3.0f, C_BG, OPAQUE);
    }
    gfx_disc(c, x, y, r + 1.6f, C_TEXT, OPAQUE);
    gfx_disc(c, x, y, r, rgb, OPAQUE);
}

static void draw_leads(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const uint8_t lo = a->mon->lead_flags;
    const bool off[3] = {(lo & EKG_FLAG_LO_MINUS) != 0, (lo & EKG_FLAG_LO_PLUS) != 0, false};
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Elektroden", NULL);
    const char *state = off[0] && off[1] ? "RA und LA ab" : off[0] ? "RA ab" : off[1] ? "LA ab"
                        : a->mon->status == MON_LEAD_OFF ? "ohne Kontakt" : "Kontakt ok";
    gfx_text(c, &FONT_SMALL, 20 + gfx_text_width(&FONT_LABEL, "Elektroden") + 8, 18, state,
             lo || a->mon->status == MON_LEAD_OFF ? C_ACCENT : C_OK, OPAQUE);

    const box_t bar = leads_bar(l);
    const cable_t *cab = &CABLES[a->ui->cables < CABLE_PRESETS ? a->ui->cables : 0];
    const avatar_t av = avatar_for(settings_person(a->ui), a->ui->avatar);
    const int bx = 4, by = HEADER_H + 6, bw = l.portrait ? 108 : 124, bh = bar.y - by - 6;
    avatar_sites_t sites;
    avatar_draw(c, bx, by, bw, bh, &av, &sites);
    const avatar_pt_t pts[3] = {sites.ra, sites.la, sites.rl};
    for (int i = 0; i < 3; i++) {
        site_marker(c, pts[i].x, pts[i].y, sites.r, cab->rgb[i], off[i]);
    }

    // Legende: Farbe, Name und Stelle
    const int lx = bx + bw + 8, tx = lx + 22, tw = l.w - tx - 8;
    const int step = l.portrait ? 78 : 54, y0 = HEADER_H + 10;
    for (int i = 0; i < 3; i++) {
        const int y = y0 + i * step;
        site_marker(c, (float)lx + 8.0f, (float)y + 9.0f, 6.0f, cab->rgb[i], false);
        char t[32];
        snprintf(t, sizeof t, "%s · %s", cab->name[i], SITE_NAME[i]);
        gfx_text(c, &FONT_LABEL, tx, y + 15, t, off[i] ? C_ACCENT : C_TEXT, OPAQUE);
        gfx_text_wrap(c, &FONT_SMALL, tx, y + 32, tw, 16, SITE_WHERE[i], C_TEXT_DIM, 3);
        if (off[i]) {
            status_icon(c, (float)l.w - 16.0f, (float)y + 9.0f, CHK_ALERT);
        }
    }

    gfx_fill(c, 0, bar.y, l.w, bar.h, C_PANEL);
    gfx_fill(c, 0, bar.y, l.w, 1, C_PANEL_EDGE);
    if (a->touch_xy) {
        button(c, leads_button(l, 0), "Farben", false, true);
        button(c, leads_button(l, 1), "Fertig", true, true);
    } else {
        text_center(c, &FONT_SMALL, l.w / 2, bar.y + bar.h / 2 + 5, "Tippen: schließen",
                    C_TEXT_DIM);
    }
}

static void open_leads(app_t *a, bool automatic) {
    a->screen = APP_LEADS;
    a->leads_auto = automatic;
    a->lead_active = false;
    a->lead_ok_active = false;
    a->leads_shown = a->mon->lead_flags;
    a->mon->draw = false;
    redraw(a);
}

static void leave_leads(app_t *a) {
    a->lead_dismissed = a->mon->status == MON_LEAD_OFF;  // nicht gleich wieder öffnen
    a->leads_auto = false;
    go_live(a);
}

// ------------------------------------------------------------------ Auswertung läuft
static void draw_analyzing(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_vgradient(c, 0, 0, l.w, l.h, SPLASH_TOP, SPLASH_BOTTOM);
    const alpha_img_t *h = &IMG_HEART_58;
    const int hy = l.h / 2 - 36;
    if (GLOW) {
        gfx_glow(c, (float)l.w / 2.0f, (float)hy, 56.0f, C_ACCENT, 70);
    }
    gfx_image_hgrad(c, h, l.w / 2 - h->w / 2, hy - h->h / 2, 0, h->w, C_ACCENT, C_ACCENT_2,
                    OPAQUE);
    text_center(c, &FONT_LABEL, l.w / 2, l.h / 2 + 30, "Wird ausgewertet …", C_TEXT);
    text_center(c, &FONT_SMALL, l.w / 2, l.h / 2 + 52, "einen Moment bitte", C_TEXT_DIM);
}

// ------------------------------------------------------------------ Ergebnis
// Knopfleiste: 0 = ‹, 1 = Kurve, 2 = Neu messen, 3 = ›
static box_t res_button(lay_t l, int i) {
    static const int XL[4] = {6, 58, 156, 270}, WL[4] = {46, 92, 108, 44};
    static const int XP[4] = {4, 44, 110, 202}, WP[4] = {36, 62, 88, 34};
    int y = l.footer_y + (l.portrait ? 6 : 4);
    return l.portrait ? (box_t){XP[i], y, WP[i], 28} : (box_t){XL[i], y, WL[i], 28};
}

int result_pages(const app_t *a) {
    return a->result_kind ? breath_result_pages() : APP_PAGES;
}

static void result_frame(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Ergebnis", a->result_kind ? breath_page_name(a->page) : PAGE_NAMES[a->page]);
    for (int i = 0; i < result_pages(a); i++) {  // Seitenpunkte
        float cx = (float)(l.w - 80) + (float)i * 9.0f;
        gfx_disc(c, cx, 13.0f, i == a->page ? 3.2f : 2.4f, i == a->page ? C_ACCENT : C_TEXT_FAINT,
                 OPAQUE);
    }
    gfx_fill(c, 0, l.footer_y, l.w, l.h - l.footer_y, C_PANEL);
    gfx_fill(c, 0, l.footer_y, l.w, 1, C_PANEL_EDGE);
    if (a->touch_xy) {
        button(c, res_button(l, 0), "‹", false, a->page > 0);
        button(c, res_button(l, 1), "Kurve", false, true);
        button(c, res_button(l, 2), "Neu messen", true, true);
        button(c, res_button(l, 3), "›", false, a->page < result_pages(a) - 1);
    } else {
        text_center(c, &FONT_SMALL, l.w / 2, l.footer_y + 22,
                    l.portrait ? "Tippen: weiter · Halten: Kurve"
                               : "Tippen: weiter · Halten: zurück zur Kurve",
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
    const char *text;
    *col = check_quality(r, &text) == 0 ? C_OK : C_WARN;
    return text;
}

static uint32_t status_color(chk_status_t st) {
    switch (st) {
        case CHK_OK:
            return C_OK;
        case CHK_NOTE:
            return C_WARN;
        case CHK_ALERT:
            return C_ACCENT;
        default:
            return C_TEXT_DIM;
    }
}

// Zeile „Check“ der Übersicht beginnt hier (Tippen führt zur Check-Seite)
static int overview_check_y(lay_t l) {
    return l.portrait ? 180 : 124;
}

static void page_overview(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    lay_t l = lay(a);
    const int dy = l.portrait ? 6 : 0;  // hochkant etwas luftiger
    char buf[64], b2[32];
    gfx_text(c, &FONT_SMALL, 14, 46 + dy, "Herzfrequenz Ø", C_TEXT_DIM, OPAQUE);
    if (r->has_hrv) {
        fmt_num(buf, sizeof buf, r->hrv.mean_hr, 0);
        int e = gfx_text(c, &FONT_HR, 12, 96 + dy, buf, C_TEXT, OPAQUE);
        gfx_text(c, &FONT_SMALL, e + 6, 96 + dy, "/min", C_TEXT_DIM, OPAQUE);
        fmt_num(buf, sizeof buf, r->hrv.min_hr, 0);
        fmt_num(b2, sizeof b2, r->hrv.max_hr, 0);
        char line[128];
        snprintf(line, sizeof line, "min %s · max %s", buf, b2);
        gfx_text(c, &FONT_SMALL, 14, 116 + dy, line, C_TEXT_DIM, OPAQUE);
    } else {
        gfx_text(c, &FONT_HR, 12, 96 + dy, "– –", C_TEXT_FAINT, OPAQUE);
        gfx_text(c, &FONT_SMALL, 14, 116 + dy, "zu wenige saubere Schläge", C_TEXT_DIM, OPAQUE);
    }
    // Dauer und Schläge: quer rechts daneben, hochkant in einer Zeile darunter
    const int dx = l.portrait ? 14 : 200, sx = l.portrait ? 126 : 200;
    const int dyl = l.portrait ? 146 : 48, syl = l.portrait ? 146 : 90;
    fmt_duration(buf, sizeof buf, r->duration_s);
    gfx_text(c, &FONT_SMALL, dx, dyl, "Dauer", C_TEXT_DIM, OPAQUE);
    gfx_text(c, &FONT_LABEL, dx, dyl + 20, buf, C_TEXT, OPAQUE);
    snprintf(buf, sizeof buf, "%d", r->n_beats);
    gfx_text(c, &FONT_SMALL, sx, syl, "Schläge", C_TEXT_DIM, OPAQUE);
    gfx_text(c, &FONT_LABEL, sx, syl + 20, buf, C_TEXT, OPAQUE);

    const int ry = overview_check_y(l);  // Trennlinie, darunter Check und Signal
    gfx_fill(c, 12, ry, l.w - 24, 1, C_PANEL_EDGE);
    gfx_text(c, &FONT_SMALL, 14, ry + 18, "Check", C_TEXT_DIM, OPAQUE);
    if (a->touch_xy) {
        gfx_text(c, &FONT_LABEL, l.w - 22, ry + 20, "›", C_TEXT_DIM, OPAQUE);
    }
    const int ty = ry + (l.portrait ? 42 : 38);
    if (a->result_test) {
        gfx_text(c, &FONT_SMALL, 14, ty, "Testsignal – keine echte Messung", C_WARN, OPAQUE);
    } else {
        const check_summary_t sum = check_summarize(a->checks, a->n_checks);
        char line[64];
        check_summary_text(&sum, line, sizeof line);
        const uint32_t col = sum.rated < 3 ? C_TEXT_DIM
                             : sum.alert    ? C_ACCENT
                             : sum.note     ? C_WARN
                                            : C_OK;
        gfx_disc(c, 22.0f, (float)ty - 5.0f, 5.0f, col, OPAQUE);
        gfx_text_wrap(c, &FONT_LABEL, 34, ty, l.w - 48, 20, line, C_TEXT, 2);
    }
    uint32_t col;
    const char *q = quality_line(r, &col);
    const int qy = l.footer_y - 8;
    gfx_disc(c, 18.0f, (float)qy - 5.0f, 4.0f, col, OPAQUE);
    snprintf(buf, sizeof buf, "Signal: %s", q);
    gfx_text(c, &FONT_SMALL, 28, qy, buf, C_TEXT, OPAQUE);
}

static void page_trend(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    lay_t l = lay(a);
    const int px0 = l.portrait ? 40 : 44, px1 = l.w - 16, py0 = 38;
    const int py1 = l.portrait ? l.footer_y - 56 : l.footer_y - 34;
    if (r->n_beats < 3) {
        text_center(c, &FONT_SMALL, l.w / 2, l.h / 2, "zu wenige Schläge", C_TEXT_DIM);
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
    // Beschriftung: quer in einer Zeile, hochkant in zwei
    const int ly = l.footer_y - 6;
    int lx = 10;
    if (l.portrait) {
        gfx_text(c, &FONT_SMALL, 10, ly - 18, "Schlagabstand (ms) · Zeit (s)", C_TEXT_FAINT,
                 OPAQUE);
    } else {
        lx = gfx_text(c, &FONT_SMALL, 10, ly, "Schlagabstand (ms)", C_TEXT_FAINT, OPAQUE) + 14;
    }
    gfx_line(c, (float)lx, (float)ly - 4.0f, (float)lx + 12.0f, (float)ly - 4.0f, 2.0f, C_TRACE,
             OPAQUE);
    int e = gfx_text(c, &FONT_SMALL, lx + 16, ly, "gewertet", C_TEXT_DIM, OPAQUE);
    gfx_disc(c, (float)e + 12.0f, (float)ly - 4.0f, 2.6f, C_ACCENT, OPAQUE);
    gfx_text(c, &FONT_SMALL, e + 19, ly, "nicht gewertet", C_TEXT_DIM, OPAQUE);
}

static void page_hrv(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    lay_t l = lay(a);
    if (!r->has_hrv) {
        text_center(c, &FONT_SMALL, l.w / 2, l.h / 2, "Zu wenige saubere Schläge für die HRV.",
                    C_TEXT_DIM);
        return;
    }
    const ana_hrv_t *h = &r->hrv;
    const int x0 = l.portrait ? 12 : 14, x1 = l.portrait ? 126 : 166;
    const int y0 = 30, rh = l.portrait ? 64 : 52;
    char v[32], v2[16];
    fmt_num(v, sizeof v, h->rmssd_ms, 0);
    label_value(c, x0, y0, "RMSSD", v, "ms");
    fmt_num(v, sizeof v, h->sdnn_ms, 0);
    label_value(c, x1, y0, "SDNN", v, "ms");
    fmt_num(v, sizeof v, h->pnn50, 1);
    label_value(c, x0, y0 + rh, "pNN50", v, "%");
    fmt_num(v, sizeof v, h->sd1_ms, 0);
    fmt_num(v2, sizeof v2, h->sd2_ms, 0);
    char sd[64];
    snprintf(sd, sizeof sd, "%s / %s", v, v2);
    label_value(c, x1, y0 + rh, "SD1 / SD2", sd, "ms");
    if (h->has_lf_hf) {
        fmt_num(v, sizeof v, h->lf_hf, 2);
    } else {
        snprintf(v, sizeof v, "–");
    }
    label_value(c, x0, y0 + 2 * rh, "LF / HF", v, "");
    if (h->has_peak) {
        fmt_num(v, sizeof v, h->peak_hz * 60.0, 0);
        label_value(c, x1, y0 + 2 * rh, "Schwankung", v, "/min");
    } else {
        label_value(c, x1, y0 + 2 * rh, "Schwankung", "–", "");
    }
    const char *note = h->duration_s < 100.0 ? "Kurze Messung: HRV-Werte nur grob."
                                             : "LF/HF nach 2 min nur grob (Standard: 5 min).";
    int n = gfx_text_wrap(NULL, &FONT_SMALL, 0, 0, l.w - 28, 16, note, C_TEXT_FAINT, 2);
    gfx_text_wrap(c, &FONT_SMALL, 14, l.footer_y - 6 - (n - 1) * 16, l.w - 28, 16, note,
                  C_TEXT_FAINT, 2);
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
    lay_t l = lay(a);
    if (!r->has_median) {
        text_center(c, &FONT_SMALL, l.w / 2, l.h / 2, "Zu wenige saubere Schläge.", C_TEXT_DIM);
        return;
    }
    // quer: Kurve links, Werte rechts; hochkant: Kurve oben, Werte darunter
    const int px0 = 10, px1 = l.portrait ? l.w - 10 : 196, py0 = 48;
    const int py1 = l.portrait ? 166 : l.footer_y - 20;
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
    const struct {
        const char *label;
        double v;
    } rows[] = {
        {"PR", r->has_delineation ? d->pr_ms : NAN},
        {"QRS", r->has_delineation ? d->qrs_ms : NAN},
        {"QT", r->has_delineation ? d->qt_ms : NAN},
        {"QTc (Fridericia)", r->has_delineation ? d->qtc_fridericia_ms : NAN},
    };
    for (int i = 0; i < 4; i++) {
        int x = l.portrait ? (i % 2 ? 126 : 12) : 208;
        int y = l.portrait ? 172 + (i / 2) * 44 : 32 + i * 42;
        gfx_text(c, &FONT_SMALL, x, y + 14, rows[i].label, C_TEXT_DIM, OPAQUE);
        fmt_num(v, sizeof v, rows[i].v, 0);
        int e = gfx_text(c, &FONT_LABEL, x, y + 34, v, C_TEXT, OPAQUE);
        if (isfinite(rows[i].v)) {
            gfx_text(c, &FONT_SMALL, e + 4, y + 34, "ms", C_TEXT_DIM, OPAQUE);
        }
    }
    char n[48];
    snprintf(n, sizeof n, "Mittel aus %d Schlägen", r->median_beats);
    gfx_text(c, &FONT_SMALL, px0, l.footer_y - 6, n, C_TEXT_FAINT, OPAQUE);
}

static void page_notes(app_t *a) {
    canvas_t *c = a->c;
    const ana_result_t *r = a->res;
    lay_t l = lay(a);
    const char *foot = "Keine Diagnose. Intervalle sind Näherungen. Bei Brustschmerz, Atemnot "
                       "oder Ohnmacht: Notruf 112.";
    int foot_lines = gfx_text_wrap(NULL, &FONT_SMALL, 0, 0, l.w - 28, 16, foot, C_TEXT, 4);
    int y = 46;
    const int max_y = l.footer_y - 6 - foot_lines * 16 - 6;
    const char *texts[ANA_MAX_HINTS + ANA_MAX_QNOTES + 2];
    int nt = 0;
    if (a->result_test) {
        texts[nt++] = "Testsignal – keine echte Messung.";
    }
    for (int i = 0; i < r->n_hints; i++) {
        texts[nt++] = r->hints[i];
    }
    for (int i = 0; i < r->n_qnotes; i++) {
        texts[nt++] = r->qnotes[i];
    }
    if (nt == 0) {
        texts[nt++] = "Keine Auffälligkeiten bei Rhythmus und Signal.";
    }
    for (int i = 0; i < nt && y <= max_y; i++) {
        int room = (max_y - y) / 16 + 1;
        gfx_disc(c, 17.0f, (float)y - 4.0f, 2.5f, C_ACCENT, OPAQUE);
        int lines = gfx_text_wrap(c, &FONT_SMALL, 26, y, l.w - 40, 16, texts[i], C_TEXT, room);
        y += lines * 16 + 6;
    }
    gfx_text_wrap(c, &FONT_SMALL, 14, l.footer_y - 6 - (foot_lines - 1) * 16, l.w - 28, 16, foot,
                  C_TEXT_FAINT, 4);
}

// ------------------------------------------------------------------ Check-Liste
// Symbole unterscheiden sich auch in der Form (nicht nur in der Farbe)
static void status_icon(canvas_t *c, float cx, float cy, chk_status_t st) {
    const uint32_t col = status_color(st);
    switch (st) {
        case CHK_OK:
            gfx_disc(c, cx, cy, 6.5f, col, OPAQUE);
            gfx_line(c, cx - 3.2f, cy + 0.2f, cx - 0.8f, cy + 2.8f, 1.8f, 0xFFFFFFu, OPAQUE);
            gfx_line(c, cx - 0.8f, cy + 2.8f, cx + 3.4f, cy - 2.8f, 1.8f, 0xFFFFFFu, OPAQUE);
            break;
        case CHK_NOTE:
            gfx_triangle(c, cx, cy - 7.5f, cx - 7.5f, cy + 6.0f, cx + 7.5f, cy + 6.0f, col,
                         OPAQUE);
            gfx_fill(c, (int)cx - 1, (int)cy - 3, 2, 5, 0x202020u);
            gfx_fill(c, (int)cx - 1, (int)cy + 3, 2, 2, 0x202020u);
            break;
        case CHK_ALERT:
            gfx_disc(c, cx, cy, 6.8f, col, OPAQUE);
            gfx_fill(c, (int)cx - 1, (int)cy - 4, 3, 5, 0xFFFFFFu);
            gfx_fill(c, (int)cx - 1, (int)cy + 2, 3, 3, 0xFFFFFFu);
            break;
        case CHK_INFO:
            gfx_disc(c, cx, cy, 6.5f, col, OPAQUE);
            gfx_fill(c, (int)cx - 1, (int)cy - 4, 2, 2, C_BG);
            gfx_fill(c, (int)cx - 1, (int)cy - 1, 2, 5, C_BG);
            break;
        default:
            gfx_disc(c, cx, cy, 6.5f, C_TEXT_FAINT, OPAQUE);
            gfx_disc(c, cx, cy, 4.6f, C_BG, OPAQUE);
            break;
    }
}

static box_t check_row(lay_t l, int i) {
    const int y0 = l.portrait ? 50 : 28, rh = l.portrait ? 23 : 17;
    return (box_t){0, y0 + i * rh, l.w, rh};
}

static void page_check(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    if (l.portrait) {
        char who[PROFILE_NAME_BYTES + 12];
        snprintf(who, sizeof who, "Person: %s", a->meas_known ? a->meas_profile.name : "Gast");
        gfx_text(c, &FONT_SMALL, 14, 44, who, C_TEXT_DIM, OPAQUE);
    }
    for (int i = 0; i < a->n_checks; i++) {
        const check_item_t *it = &a->checks[i];
        const box_t r = check_row(l, i);
        const int mid = r.y + r.h / 2;
        status_icon(c, 20.0f, (float)mid, it->status);
        gfx_text(c, &FONT_SMALL, 36, mid + 5, it->label, C_TEXT, OPAQUE);
        gfx_text(c, &FONT_SMALL, l.w - 12 - gfx_text_width(&FONT_SMALL, it->value), mid + 5,
                 it->value, it->status == CHK_NA ? C_TEXT_FAINT : C_TEXT_DIM, OPAQUE);
        if (i + 1 < a->n_checks) {
            gfx_fill(c, 12, r.y + r.h - 1, l.w - 24, 1, C_PANEL_EDGE);
        }
    }
    if (a->touch_xy) {
        text_center(c, &FONT_SMALL, l.w / 2, l.footer_y - 5, "Antippen für die Erklärung",
                    C_TEXT_FAINT);
    }
}

static void draw_check_detail(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    const check_item_t *it = &a->checks[a->detail];
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, it->label, NULL);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d / %d", a->detail + 1, a->n_checks);
    gfx_text(c, &FONT_SMALL, l.w - 40 - gfx_text_width(&FONT_SMALL, cnt), 18, cnt, C_TEXT_FAINT,
             OPAQUE);
    const int foot = l.h - 10;  // Grundlinie der Fußzeile
    const bool alert = it->status == CHK_ALERT;
    const int e_lines = l.portrait ? 3 : 2;  // Zeilen des Notruf-Hinweises
    const int e_base = foot - (e_lines - 1) * 16;
    const int end = alert ? e_base - 16 : foot - 20;  // Platz für Text bis hier

    int y = 56;
    status_icon(c, 22.0f, (float)y - 5.0f, it->status);
    const uint32_t vcol = it->status == CHK_NOTE || it->status == CHK_ALERT ? status_color(it->status)
                                                                            : C_TEXT;
    int n = gfx_text_wrap(c, &FONT_LABEL, 40, y, l.w - 54, 20, it->verdict, vcol, 2);
    y += (n - 1) * 20 + 14;
    gfx_text(c, &FONT_VALUE, 14, y + 20, it->value, C_TEXT, OPAQUE);
    y += 40;
    int room = (end - y) / 16;
    n = gfx_text_wrap(c, &FONT_SMALL, 14, y + 12, l.w - 28, 16, it->advice, C_TEXT,
                      room < 1 ? 1 : room);
    y += n * 16 + 10;
    room = (end - y) / 16;
    if (room >= 1) {
        gfx_text_wrap(c, &FONT_SMALL, 14, y + 12, l.w - 28, 16, it->about, C_TEXT_FAINT, room);
    }
    if (alert) {  // statt der Wisch-Zeile
        gfx_text_wrap(c, &FONT_SMALL, 14, e_base, l.w - 28, 16,
                      "Bei Brustschmerz, Atemnot oder Ohnmacht sofort den Notruf 112 wählen.",
                      C_ACCENT, e_lines);
    } else if (a->touch_xy) {
        text_center(c, &FONT_SMALL, l.w / 2, foot, "Wischen: weiter · Tippen: zurück",
                    C_TEXT_FAINT);
    }
}

static void draw_result(app_t *a) {
    result_frame(a);
    lay_t l = lay(a);
    if (a->result_status != ANA_OK) {
        const char *why = a->result_status == ANA_TOO_SHORT ? "Messung zu kurz (mindestens 30 s)."
                          : a->result_status == ANA_TOO_MANY_GAPS ? "Zu viele Störungen im Signal."
                                                                  : "Interner Fehler (Speicher).";
        text_center(a->c, &FONT_LABEL, l.w / 2, l.h / 2 - 20, "Keine Auswertung möglich", C_TEXT);
        text_center(a->c, &FONT_SMALL, l.w / 2, l.h / 2 + 4, why, C_TEXT_DIM);
        return;
    }
    if (a->result_kind) {
        breath_draw_result(a);
        return;
    }
    switch (a->page) {
        case 0:
            page_overview(a);
            break;
        case PAGE_CHECK:
            page_check(a);
            break;
        case 2:
            page_trend(a);
            break;
        case 3:
            page_hrv(a);
            break;
        case 4:
            page_beat(a);
            break;
        default:
            page_notes(a);
            break;
    }
}

// ------------------------------------------------------------------ Touch einrichten
static void cal_target(lay_t l, int i, int *x, int *y) {
    const int m = 30;
    const int xs[5] = {m, l.w - m, l.w - m, m, l.w / 2};
    const int ys[5] = {m, m, l.h - m, l.h - m, l.h / 2};
    *x = xs[i];
    *y = ys[i];
}

static void draw_cal(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    int tx, ty;
    cal_target(l, a->cal_step, &tx, &ty);
    float x = (float)tx, y = (float)ty;
    gfx_line(c, x - 14.0f, y, x + 14.0f, y, 1.6f, C_ACCENT, OPAQUE);
    gfx_line(c, x, y - 14.0f, x, y + 14.0f, 1.6f, C_ACCENT, OPAQUE);
    gfx_round_frame(c, (int)x - 7, (int)y - 7, 14, 14, 7, 1.5f, C_ACCENT);
    text_center(c, &FONT_LABEL, l.w / 2, l.h / 2 - 36, "Touch einrichten", C_TEXT);
    static const char *const LAND[] = {"Genau auf die Mitte des Kreuzes tippen –",
                                       "am besten mit Fingernagel oder Stift."};
    static const char *const PORT[] = {"Genau auf die Kreuzmitte tippen,",
                                       "am besten mit Fingernagel", "oder Stift."};
    if (l.portrait) {
        lines_center(c, l.w / 2, l.h / 2 + 40, 16, PORT, 3, C_TEXT_DIM);
    } else {
        lines_center(c, l.w / 2, l.h / 2 + 40, 16, LAND, 2, C_TEXT_DIM);
    }
    char b[32];
    snprintf(b, sizeof b, "Punkt %d von 5", a->cal_step + 1);
    text_center(c, &FONT_SMALL, l.w / 2, l.h / 2 + (l.portrait ? 96 : 76), b, C_TEXT_FAINT);
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
    lay_t l = lay(a);
    float sx[5], sy[5];
    for (int i = 0; i < 5; i++) {  // Ziele in die Queransicht umrechnen (dort gilt die Kalibrierung)
        int x, y, xl, yl;
        cal_target(l, i, &x, &y);
        touch_screen_to_land(l.portrait, x, y, &xl, &yl);
        sx[i] = (float)xl;
        sy[i] = (float)yl;
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
        show_toast(a, "Nicht genau genug – bitte noch einmal.", now);
    }
}

// ------------------------------------------------------------------ Info
static int info_line(app_t *a, int y, const font_t *f, const char *text, uint32_t col) {
    lay_t l = lay(a);
    int n = gfx_text_wrap(a->c, f, 14, y, l.w - 28, 16, text, col, 5);
    return y + (n - 1) * 16 + 20;
}

static void draw_info(app_t *a) {
    canvas_t *c = a->c;
    lay_t l = lay(a);
    gfx_fill(c, 0, 0, l.w, l.h, C_BG);
    header(a, "Info", NULL);
    char b[96];
    snprintf(b, sizeof b, "SophiEKG %s", a->version ? a->version : "");
    gfx_text(c, &FONT_LABEL, 14, 52, b, C_TEXT, OPAQUE);
    int y = 78;
    snprintf(b, sizeof b, "1-Kanal-EKG, Messdauer %d min", ANA_MAX_S / 60);
    y = info_line(a, y, &FONT_SMALL, b, C_TEXT_DIM);
    if (a->display_id == 0x9341u) {
        snprintf(b, sizeof b, "Display: ILI9341");
    } else {
        snprintf(b, sizeof b, "Display: unbekannt (0x%04lx)", (unsigned long)a->display_id);
    }
    y = info_line(a, y, &FONT_SMALL, b, C_TEXT_DIM);
    y = info_line(a, y, &FONT_SMALL, a->touch_xy ? "Touch: eingerichtet" : "Touch: nur Tippen",
                  C_TEXT_DIM);
    if (a->usb) {
        snprintf(b, sizeof b, "Stromversorgung: USB");
    } else if (a->battery_pct >= 0) {
        char v[16];
        fmt_num(v, sizeof v, a->vsys_ema / 1000.0, 2);
        snprintf(b, sizeof b, "Batterie: %s V (%d %%)", v, a->battery_pct);
    } else if (a->ui->vsys == VSYS_NONE) {
        snprintf(b, sizeof b, "Batterie: auf diesem Board nicht messbar");
    } else {
        snprintf(b, sizeof b, "Batterie: noch keine verlässliche Messung");
    }
    y = info_line(a, y, &FONT_SMALL, b, C_TEXT_DIM);
    info_line(a, y + 10, &FONT_SMALL,
              "Eine Ableitung, 0,5–40 Hz: keine Beurteilung der ST-Strecke. Die Hinweise "
              "ersetzen keine ärztliche Untersuchung.",
              C_TEXT_FAINT);
}

// ------------------------------------------------------------------ Messung
static void start_measure(app_t *a) {
    const profile_t *who = settings_person(a->ui);
    a->meas_known = who != NULL;
    if (who) {
        a->meas_profile = *who;
    } else {
        profile_clear(&a->meas_profile);
    }
    ana_rec_clear(a->rec);
    a->measuring = true;
    a->measure_full = false;
    a->meas_usb_seen = a->usb;
    a->meas_test_blocks = 0;
    a->have_seq = false;
    a->meas_mode = a->mode;
    monitor_set_measure(a->mon, true, 0, ANA_MAX_S);
    if (a->mode == MODE_BREATH) {  // eigene Ansicht statt der laufenden Kurve
        breath_live_reset(a);
        a->screen = APP_BREATH_LIVE;
        a->mon->draw = false;
        redraw(a);
        return;
    }
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
        show_toast(a, "Zu kurz für eine Auswertung (mindestens 30 Sekunden).", now);
    }
}

static void run_analysis(app_t *a) {
    ana_power_t power = a->meas_usb_seen ? ANA_POWER_UNKNOWN : ANA_POWER_BATTERY;
    a->result_test = a->meas_test_blocks * 2 > (int)a->rec->n_blocks;
    a->result_status = ana_run(a->rec, power, &a->mon->qrs, a->scratch, a->scratch_size, a->res);
    monitor_relearn(a->mon);  // der Detektor wurde für die Auswertung geliehen
    a->has_result = a->result_status == ANA_OK;
    a->n_checks = a->has_result ? check_build(a->res, a->meas_known ? &a->meas_profile : NULL,
                                              a->result_test, a->checks, CHK_MAX)
                                : 0;
    a->result_kind = 0;
    if (a->meas_mode == MODE_BREATH) {
        breath_finish(a);  // setzt result_kind und ersetzt das letzte EKG-Ergebnis
        a->n_checks = 0;
        a->result_status = ANA_OK;
    }
    a->screen = APP_RESULT;
    a->page = 0;
    draw_result(a);
    a->req_push_all = true;
}

// ------------------------------------------------------------------ Bildaufbau
void redraw(app_t *a) {
    switch (a->screen) {
        case APP_LIVE:
            monitor_draw_all(a->mon);
            break;
        case APP_MENU:
            draw_menu(a, true);
            break;
        case APP_SETTINGS:
            draw_settings(a);
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
        case APP_PROFILES:
            draw_profiles(a);
            break;
        case APP_PROFILE_EDIT:
            draw_profile_edit(a);
            break;
        case APP_KEYBOARD:
            kb_draw(&a->kb, a->c);
            break;
        case APP_CHECK_DETAIL:
            draw_check_detail(a);
            break;
        case APP_LEADS:
            draw_leads(a);
            break;
        case APP_BREATH_LIVE:
            breath_draw_live(a);
            break;
        case APP_COACH:
            coach_draw(a);
            break;
        case APP_PRIV_PIN:
        case APP_PRIV_HOME:
        case APP_PRIV_SESSION:
        case APP_PRIV_DONE:
        case APP_PRIV_HISTORY:
            priv_draw(a);
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
    if (a->ui->rotation > 1) {
        a->ui->rotation = 0;
    }
    if (a->ui->person > PERSON_GUEST) {
        a->ui->person = PERSON_GUEST;
    }
    a->ui->avatar &= 1u;
    if (a->ui->cables >= CABLE_PRESETS) {
        a->ui->cables = 0;
    }
    apply_look(a);
    apply_orientation(a);
    monitor_set_scale(a->mon, a->ui->gain, a->ui->speed);
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
    if (a->measuring && a->meas_mode == MODE_BREATH) {
        breath_live_feed(a);
    }
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

// Anzeige nur, wenn die Messung verlässlich ist: Teiler bestätigt, oder (noch ungeprüft)
// eine Spannung, bei der Display und Pico überhaupt laufen
static void update_battery(app_t *a) {
    int pct = -1;
    if (!a->usb && a->vsys_ema > 0 && a->ui->vsys != VSYS_NONE) {
        bool plausible = a->ui->vsys == VSYS_OK || a->vsys_ema >= 3300.0f;
        int p = plausible ? app_battery_percent((int)a->vsys_ema) : -1;
        pct = p >= 0 ? (p + 2) / 5 * 5 : -1;  // in 5-%-Schritten, sonst springt die Anzeige
    }
    a->battery_pct = pct;
    monitor_set_battery(a->mon, pct);
}

void app_set_status(app_t *a, bool usb, bool streaming) {
    if (usb != a->usb) {
        a->vsys_ema = 0;  // andere Quelle: neu mitteln
    }
    a->usb = usb;
    a->streaming = streaming;
    monitor_set_link(a->mon, usb, streaming);
    update_battery(a);
}

void app_vsys_sample(app_t *a, int mv, bool on_usb) {
    if (on_usb) {
        uint8_t st = mv >= 4300 && mv <= 5300 ? VSYS_OK : VSYS_NONE;
        if (st != a->ui->vsys) {
            a->ui->vsys = st;
            a->req_save = true;
        }
        return;
    }
    if (mv > 0) {
        a->vsys_ema = a->vsys_ema > 0 ? 0.7f * a->vsys_ema + 0.3f * (float)mv : (float)mv;
    }
    update_battery(a);
}

static void next_look(app_t *a) {
    a->ui->look = (uint8_t)((a->ui->look + 1) % N_LOOKS);
    apply_look(a);
    a->req_save = true;
    redraw(a);
}

// Einstellungszeile antippen: Wert weiterschalten oder Unterseite öffnen
static void settings_tap(app_t *a, int row) {
    switch (row) {
        case SET_LOOK:
            next_look(a);
            return;
        case SET_ORIENT:
            a->ui->rotation ^= 1u;
            apply_orientation(a);
            a->req_rotate = true;
            break;
        case SET_GAIN:
            a->ui->gain = (uint8_t)((a->ui->gain + 1) % MON_GAIN_COUNT);
            monitor_set_scale(a->mon, a->ui->gain, a->ui->speed);
            break;
        case SET_SPEED:
            a->ui->speed = (uint8_t)((a->ui->speed + 1) % MON_SPEED_COUNT);
            monitor_set_scale(a->mon, a->ui->gain, a->ui->speed);
            break;
        case SET_AVATAR:
            a->ui->avatar ^= 1u;
            break;
        case SET_CAL:
            start_cal(a);
            return;
        case SET_INFO:
            a->screen = APP_INFO;
            redraw(a);
            return;
        default:
            return;
    }
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
    a->clock = now;
    if (ev.type == EV_NONE) {
        return;
    }
    const bool tap = ev.type == EV_TAP;
    const lay_t l = lay(a);
    switch (a->screen) {
        case APP_LIVE:
            if (!a->touch_xy) {  // ohne Koordinaten: Tippen = Tag/Nacht, Halten = Messung
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
                if (tap && ev.x >= l.w - 84 && ev.y <= 40) {  // „Stopp“
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
            if (hit(menu_mode(l, 0), ev.x, ev.y) || hit(menu_mode(l, 1), ev.x, ev.y)) {
                a->mode = hit(menu_mode(l, 1), ev.x, ev.y) ? MODE_BREATH : MODE_EKG;
                redraw(a);
            } else if (hit(menu_item(l, 0, a->mode), ev.x, ev.y)) {
                start_measure(a);
            } else if (hit(menu_item(l, 1, a->mode), ev.x, ev.y) &&
                       (a->mode == MODE_EKG ? (a->has_result || a->result_status) : a->has_breath)) {
                a->result_kind = a->mode == MODE_BREATH;
                a->screen = APP_RESULT;
                a->page = 0;
                redraw(a);
            } else if (a->mode == MODE_BREATH && hit(menu_item(l, 5, a->mode), ev.x, ev.y)) {
                coach_open(a, now);
                redraw(a);
            } else if (hit(menu_item(l, 2, a->mode), ev.x, ev.y)) {
                a->screen = APP_SETTINGS;
                redraw(a);
            } else if (hit(menu_item(l, 4, a->mode), ev.x, ev.y)) {
                open_leads(a, false);
            } else if (hit(menu_item(l, 3, a->mode), ev.x, ev.y)) {
                a->screen = APP_PROFILES;
                redraw(a);
            } else if (hit(menu_close(l), ev.x, ev.y) || !hit(menu_panel(l), ev.x, ev.y)) {
                go_live(a);
            }
            break;
        case APP_SETTINGS:
            if (ev.type == EV_LONG || (tap && hit(close_box(l), ev.x, ev.y))) {
                go_live(a);
                break;
            }
            if (tap) {
                for (int i = 0; i < SET_ROWS; i++) {
                    if (hit(settings_row(l, i), ev.x, ev.y)) {
                        settings_tap(a, i);
                        break;
                    }
                }
            }
            break;
        case APP_RESULT:
            if (!a->touch_xy) {
                if (tap) {
                    a->page = (a->page + 1) % result_pages(a);
                    redraw(a);
                } else if (ev.type == EV_LONG) {
                    go_live(a);
                }
                break;
            }
            if (tap && a->page == 0 && ev.y >= overview_check_y(l) && ev.y < l.footer_y &&
                a->has_result) {
                a->page = PAGE_CHECK;
                redraw(a);
            } else if (tap && a->page == PAGE_CHECK && a->has_result && ev.y < l.footer_y &&
                       ev.y >= check_row(l, 0).y &&
                       (ev.y - check_row(l, 0).y) / check_row(l, 0).h < a->n_checks) {
                a->detail = (ev.y - check_row(l, 0).y) / check_row(l, 0).h;
                a->screen = APP_CHECK_DETAIL;
                redraw(a);
            } else if (ev.type == EV_SWIPE_LEFT || (tap && hit(res_button(l, 3), ev.x, ev.y))) {
                if (a->page < result_pages(a) - 1) {
                    a->page++;
                    redraw(a);
                }
            } else if (ev.type == EV_SWIPE_RIGHT || (tap && hit(res_button(l, 0), ev.x, ev.y))) {
                if (a->page > 0) {
                    a->page--;
                    redraw(a);
                }
            } else if (tap && hit(res_button(l, 2), ev.x, ev.y)) {
                a->mode = a->result_kind ? MODE_BREATH : MODE_EKG;
                start_measure(a);
            } else if ((tap && (hit(res_button(l, 1), ev.x, ev.y) ||
                                hit(close_box(l), ev.x, ev.y))) ||
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
                               "Touch meldet keine Position – Kabel T_DO (GP12) prüfen. "
                               "Tippen: Tag/Nacht, Halten: Messung.",
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
            if (tap && hit(info_version_box(l), ev.x, ev.y)) {
                priv_tap_version(a, now);  // Zugang zu einem versteckten Bereich, ohne Hinweis
            } else if (tap || ev.type == EV_LONG) {
                a->screen = APP_SETTINGS;
                redraw(a);
            }
            break;
        case APP_PRIV_PIN:
        case APP_PRIV_HOME:
        case APP_PRIV_SESSION:
        case APP_PRIV_DONE:
        case APP_PRIV_HISTORY:
            priv_event(a, ev, now);
            break;
        case APP_PROFILES:
            if (ev.type == EV_LONG || (tap && hit(close_box(l), ev.x, ev.y))) {
                back_to_menu(a);
                break;
            }
            for (int i = 0; tap && i <= PERSON_GUEST; i++) {
                if (!hit(prof_row(l, i), ev.x, ev.y)) {
                    continue;
                }
                if (i == PERSON_GUEST) {
                    select_person(a, i);
                } else if (!profile_used(&a->ui->profiles[i]) ||
                           hit(prof_edit_box(l, i), ev.x, ev.y)) {
                    open_editor(a, i);
                } else {
                    select_person(a, i);
                }
                break;
            }
            break;
        case APP_PROFILE_EDIT:
            if (ev.type == EV_LONG || (tap && hit(close_box(l), ev.x, ev.y))) {
                close_editor(a);
                break;
            }
            if (!tap) {
                break;
            }
            if (hit(edit_button(l, 1), ev.x, ev.y)) {
                close_editor(a);
            } else if (hit(edit_button(l, 0), ev.x, ev.y)) {
                if (profile_used(&a->ui->profiles[a->edit_idx])) {
                    profile_clear(&a->ui->profiles[a->edit_idx]);
                    if (a->ui->person == a->edit_idx) {
                        a->ui->person = PERSON_GUEST;
                    }
                    a->edit_new = false;
                    a->req_save = true;
                    a->screen = APP_PROFILES;
                    redraw(a);
                }
            } else {
                for (int i = 0; i < F_COUNT; i++) {
                    if (!hit(edit_row(l, i), ev.x, ev.y)) {
                        continue;
                    }
                    if (i == F_SEX) {
                        profile_t *p = &a->ui->profiles[a->edit_idx];
                        p->sex = (uint8_t)((p->sex + 1) % 3);
                        a->req_save = true;
                        redraw(a);
                    } else {
                        open_field(a, i);
                    }
                    break;
                }
            }
            break;
        case APP_KEYBOARD:
            if (tap) {
                kb_result_t kr = kb_tap(&a->kb, l.w, l.h, ev.x, ev.y);
                if (kr == KB_OK) {
                    keyboard_done(a);
                }
                a->screen = kr == KB_NONE ? APP_KEYBOARD : APP_PROFILE_EDIT;
                redraw(a);
            }
            break;
        case APP_BREATH_LIVE:
            if ((tap && ev.x >= l.w - 84 && ev.y <= 40) || (!a->touch_xy && ev.type == EV_LONG)) {
                stop_measure(a, true, now);
            }
            break;
        case APP_COACH:
            if (ev.type == EV_LONG || (tap && coach_tap(a, ev.x, ev.y, now))) {
                a->screen = APP_MENU;
                redraw(a);
            }
            break;
        case APP_LEADS:
            if (!a->touch_xy) {
                if (tap || ev.type == EV_LONG) {
                    leave_leads(a);
                }
            } else if (ev.type == EV_LONG ||
                       (tap && (hit(leads_button(l, 1), ev.x, ev.y) || hit(close_box(l), ev.x, ev.y)))) {
                leave_leads(a);
            } else if (tap && hit(leads_button(l, 0), ev.x, ev.y)) {
                a->ui->cables = (uint8_t)((a->ui->cables + 1) % CABLE_PRESETS);
                a->req_save = true;
                redraw(a);
            }
            break;
        case APP_CHECK_DETAIL:
            if (ev.type == EV_SWIPE_LEFT) {
                if (a->detail < a->n_checks - 1) {
                    a->detail++;
                    redraw(a);
                }
            } else if (ev.type == EV_SWIPE_RIGHT) {
                if (a->detail > 0) {
                    a->detail--;
                    redraw(a);
                }
            } else if (tap || ev.type == EV_LONG) {
                a->screen = APP_RESULT;
                a->page = PAGE_CHECK;
                redraw(a);
            }
            break;
        case APP_ANALYZING:
            break;
    }
}

int app_frame(app_t *a, uint32_t now, rect_t *dirty, int max_dirty) {
    a->clock = now;
    if (a->screen == APP_BREATH_LIVE && now >= a->bl_draw_at && !a->toast_shown) {
        a->bl_draw_at = now + 500;  // zweimal pro Sekunde genügt für die langsame Kurve
        redraw(a);
    }
    if (a->toast_shown && (int32_t)(now - a->toast_until) >= 0) {
        a->toast_shown = false;
        redraw(a);
        return 0;
    }
    if (a->screen >= APP_PRIV_PIN && a->screen <= APP_PRIV_HISTORY) {
        priv_frame(a, now);
        return 0;
    }
    if (a->screen == APP_COACH) {
        if (coach_frame(a, now)) {  // Übung zu Ende
            char msg[48];
            snprintf(msg, sizeof msg, "%d min geübt.", a->coach.dur_min);
            a->screen = APP_MENU;
            show_toast(a, msg, now);
        }
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
    if (a->screen == APP_LIVE && !a->measuring && !a->toast_shown) {
        if (a->mon->status == MON_LEAD_OFF) {
            if (!a->lead_active) {
                a->lead_active = true;
                a->lead_off_since = now;
            } else if (!a->lead_dismissed && now - a->lead_off_since >= LEADS_AUTO_MS) {
                open_leads(a, true);
                return 0;
            }
        } else {
            a->lead_active = false;
            a->lead_dismissed = false;
        }
    }
    if (a->screen == APP_LEADS) {
        if (a->mon->lead_flags != a->leads_shown) {  // andere Elektrode ab: Markierung nachführen
            a->leads_shown = a->mon->lead_flags;
            redraw(a);
        } else if (a->leads_auto && a->mon->status != MON_LEAD_OFF) {
            if (!a->lead_ok_active) {
                a->lead_ok_active = true;
                a->lead_ok_since = now;
            } else if (now - a->lead_ok_since >= LEADS_OK_MS) {
                a->leads_auto = false;
                go_live(a);
                show_toast(a, "Elektroden haben Kontakt.", now);
            }
        } else {
            a->lead_ok_active = false;
        }
        return 0;
    }
    if (!a->low_warned && a->ui->vsys == VSYS_OK && a->battery_pct >= 0 && a->battery_pct <= 10) {
        a->low_warned = true;
        show_toast(a, "Batterien fast leer – bald tauschen.", now);
        return 0;
    }
    if (a->screen == APP_LIVE && !a->toast_shown) {
        return monitor_frame(a->mon, now, dirty, max_dirty);
    }
    return 0;
}
