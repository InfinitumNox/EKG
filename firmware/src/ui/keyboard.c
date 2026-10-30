#include "keyboard.h"

#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "theme.h"

typedef enum { K_CHAR, K_SHIFT, K_BACK, K_SPACE, K_OK } kind_t;

typedef struct {
    kind_t kind;
    char s[4];  // Zeichen (UTF-8) bei K_CHAR
    int x, y, w, h;
} kkey_t;

#define MAX_KEYS 48
#define HEADER 26

static const char ROW1[] = "qwertzuiop";
static const char ROW2[] = "asdfghjkl";
static const char ROW3[] = "yxcvbnm";
static const char *const ROW4[] = {"ä", "ö", "ü", "ß", "-"};

static int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++) {
        n += ((unsigned char)*s & 0xC0u) != 0x80u;
    }
    return n;
}

static void backspace(char *s) {
    size_t n = strlen(s);
    while (n > 0 && ((unsigned char)s[n - 1] & 0xC0u) == 0x80u) {
        n--;
    }
    if (n > 0) {
        n--;
    }
    s[n] = '\0';
}

static char last_byte(const char *s) {
    size_t n = strlen(s);
    return n ? s[n - 1] : '\0';
}

// nach jedem Zeichen: am Anfang, nach Leerzeichen und Bindestrich groß schreiben
static void auto_shift(kb_t *k) {
    char l = last_byte(k->buf);
    k->shift = k->mode == KB_TEXT && (l == '\0' || l == ' ' || l == '-');
}

void kb_open_text(kb_t *k, const char *title, const char *initial, int max_chars) {
    memset(k, 0, sizeof *k);
    k->mode = KB_TEXT;
    k->title = title;
    k->max_chars = max_chars;
    snprintf(k->buf, sizeof k->buf, "%s", initial ? initial : "");
    auto_shift(k);
}

void kb_open_number(kb_t *k, const char *title, const char *unit, int initial, int min_val,
                    int max_val) {
    memset(k, 0, sizeof *k);
    k->mode = KB_NUMBER;
    k->title = title;
    k->unit = unit;
    k->min_val = min_val;
    k->max_val = max_val;
    if (initial > 0) {
        snprintf(k->buf, sizeof k->buf, "%d", initial);
    }
}

int kb_value(const kb_t *k) {
    int v = 0;
    for (const char *p = k->buf; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
    }
    return v;
}

bool kb_can_ok(const kb_t *k) {
    if (k->mode == KB_NUMBER) {
        return k->buf[0] == '\0' || kb_value(k) >= k->min_val;
    }
    for (const char *p = k->buf; *p; p++) {
        if (*p != ' ') {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ Tastenfeld
static void add_key(kkey_t *keys, int *n, kind_t kind, const char *s, int x, int y, int w,
                    int h) {
    if (*n >= MAX_KEYS) {
        return;
    }
    kkey_t *kk = &keys[(*n)++];
    kk->kind = kind;
    snprintf(kk->s, sizeof kk->s, "%s", s ? s : "");
    kk->x = x;
    kk->y = y;
    kk->w = w;
    kk->h = h;
}

// Position in Zehnteln der Breite: from/to, Rest der Pixel geht an die letzte Taste
static void add_span(kkey_t *keys, int *n, kind_t kind, const char *s, int w, int y, int h,
                     int from_x2, int to_x2) {
    // Maße in halben Tastenbreiten (x2), 20 = volle Breite
    int x0 = w * from_x2 / 20, x1 = w * to_x2 / 20;
    add_key(keys, n, kind, s, x0, y, x1 - x0, h);
}

static int layout(const kb_t *k, int w, int h, kkey_t *keys) {
    int n = 0;
    char one[2] = {0, 0};
    if (k->mode == KB_NUMBER) {
        const bool portrait = h > w;
        const int margin = 24, kh = portrait ? 50 : 40;
        const int gw = w - 2 * margin, y0 = h - 4 * kh;
        static const char *const PAD[4][3] = {
            {"7", "8", "9"}, {"4", "5", "6"}, {"1", "2", "3"}, {"<", "0", "OK"}};
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 3; c++) {
                const char *lab = PAD[r][c];
                int x0 = margin + gw * c / 3, x1 = margin + gw * (c + 1) / 3;
                kind_t kind = lab[0] == '<' ? K_BACK : lab[0] == 'O' ? K_OK : K_CHAR;
                add_key(keys, &n, kind, kind == K_CHAR ? lab : "", x0, y0 + r * kh, x1 - x0, kh);
            }
        }
        return n;
    }
    const int kh = h > w ? 40 : 34, y0 = h - 5 * kh;
    for (int i = 0; ROW1[i]; i++) {
        one[0] = ROW1[i];
        add_span(keys, &n, K_CHAR, one, w, y0, kh, 2 * i, 2 * i + 2);
    }
    for (int i = 0; ROW2[i]; i++) {
        one[0] = ROW2[i];
        add_span(keys, &n, K_CHAR, one, w, y0 + kh, kh, 1 + 2 * i, 3 + 2 * i);
    }
    add_span(keys, &n, K_SHIFT, "", w, y0 + 2 * kh, kh, 0, 3);
    for (int i = 0; ROW3[i]; i++) {
        one[0] = ROW3[i];
        add_span(keys, &n, K_CHAR, one, w, y0 + 2 * kh, kh, 3 + 2 * i, 5 + 2 * i);
    }
    add_span(keys, &n, K_BACK, "", w, y0 + 2 * kh, kh, 17, 20);
    for (int i = 0; i < 5; i++) {
        add_span(keys, &n, K_CHAR, ROW4[i], w, y0 + 3 * kh, kh, 4 * i, 4 * i + 4);
    }
    add_span(keys, &n, K_SPACE, "", w, y0 + 4 * kh, kh, 0, 12);
    add_span(keys, &n, K_OK, "", w, y0 + 4 * kh, kh, 12, 20);
    return n;
}

static bool is_label(const kkey_t *kk, const char *label) {
    switch (kk->kind) {
        case K_SHIFT:
            return strcmp(label, "^") == 0;
        case K_BACK:
            return strcmp(label, "<") == 0;
        case K_SPACE:
            return strcmp(label, "Leer") == 0;
        case K_OK:
            return strcmp(label, "OK") == 0;
        default:
            return strcmp(kk->s, label) == 0;
    }
}

bool kb_key_center(const kb_t *k, int w, int h, const char *label, int *x, int *y) {
    kkey_t keys[MAX_KEYS];
    int n = layout(k, w, h, keys);
    for (int i = 0; i < n; i++) {
        if (is_label(&keys[i], label)) {
            *x = keys[i].x + keys[i].w / 2;
            *y = keys[i].y + keys[i].h / 2;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ Eingabe
static void upper(char *s) {
    if (s[0] >= 'a' && s[0] <= 'z') {
        s[0] = (char)(s[0] - 32);
    } else if ((unsigned char)s[0] == 0xC3u &&
               ((unsigned char)s[1] == 0xA4u || (unsigned char)s[1] == 0xB6u ||
                (unsigned char)s[1] == 0xBCu)) {
        s[1] = (char)((unsigned char)s[1] - 0x20u);  // ä ö ü -> Ä Ö Ü
    }
}

static void append_text(kb_t *k, const char *s) {
    size_t len = strlen(k->buf), add = strlen(s);
    if (utf8_chars(k->buf) >= k->max_chars || len + add + 1 > sizeof k->buf) {
        return;
    }
    memcpy(k->buf + len, s, add + 1);
}

static void append_digit(kb_t *k, char d) {
    size_t len = strlen(k->buf);
    if ((len == 0 && d == '0') || len >= 3) {
        return;  // keine führende Null: 0 heißt „keine Angabe“
    }
    if (kb_value(k) * 10 + (d - '0') > k->max_val) {
        return;
    }
    k->buf[len] = d;
    k->buf[len + 1] = '\0';
}

kb_result_t kb_tap(kb_t *k, int w, int h, int x, int y) {
    if (x >= w - 44 && y < 36) {
        return KB_CANCEL;
    }
    kkey_t keys[MAX_KEYS];
    int n = layout(k, w, h, keys);
    for (int i = 0; i < n; i++) {
        const kkey_t *kk = &keys[i];
        if (x < kk->x || x >= kk->x + kk->w || y < kk->y || y >= kk->y + kk->h) {
            continue;
        }
        switch (kk->kind) {
            case K_CHAR:
                if (k->mode == KB_NUMBER) {
                    append_digit(k, kk->s[0]);
                } else {
                    char s[4];
                    snprintf(s, sizeof s, "%s", kk->s);
                    if (k->shift) {
                        upper(s);
                    }
                    append_text(k, s);
                    auto_shift(k);
                }
                break;
            case K_SHIFT:
                k->shift = !k->shift;
                break;
            case K_BACK:
                backspace(k->buf);
                auto_shift(k);
                break;
            case K_SPACE:
                if (k->buf[0] && last_byte(k->buf) != ' ') {
                    append_text(k, " ");
                    auto_shift(k);
                }
                break;
            case K_OK:
                if (!kb_can_ok(k)) {
                    break;
                }
                while (k->mode == KB_TEXT && last_byte(k->buf) == ' ') {
                    backspace(k->buf);
                }
                return KB_OK;
        }
        break;
    }
    return KB_NONE;
}

// ------------------------------------------------------------------ Zeichnen
static void key_face(canvas_t *c, const kkey_t *kk, uint32_t fill, bool framed) {
    gfx_round_rect(c, kk->x + 1, kk->y + 1, kk->w - 2, kk->h - 2, 7, fill, OPAQUE);
    if (framed) {
        gfx_round_frame(c, kk->x + 1, kk->y + 1, kk->w - 2, kk->h - 2, 7, 1.0f, C_PANEL_EDGE);
    }
}

static void centered(canvas_t *c, const font_t *f, const kkey_t *kk, const char *s,
                     uint32_t col) {
    gfx_text(c, f, kk->x + (kk->w - gfx_text_width(f, s)) / 2, kk->y + kk->h / 2 + 6, s, col,
             OPAQUE);
}

void kb_draw(const kb_t *k, canvas_t *c) {
    const int w = c->w, h = c->h;
    gfx_fill(c, 0, 0, w, h, C_BG);
    gfx_fill(c, 0, 0, w, HEADER, C_PANEL);
    gfx_fill(c, 0, HEADER - 1, w, 1, C_PANEL_EDGE);
    gfx_text(c, &FONT_LABEL, 10, 19, k->title ? k->title : "", C_TEXT, OPAQUE);
    gfx_line(c, (float)w - 25.0f, 8.0f, (float)w - 15.0f, 18.0f, 2.0f, C_TEXT_DIM, OPAQUE);
    gfx_line(c, (float)w - 25.0f, 18.0f, (float)w - 15.0f, 8.0f, 2.0f, C_TEXT_DIM, OPAQUE);

    // Eingabefeld
    const bool portrait = h > w;
    const int fy = HEADER + (portrait ? 14 : 8), fh = 34;
    gfx_round_rect(c, 10, fy, w - 20, fh, 9, gfx_mix(C_PANEL, C_TEXT, 6), OPAQUE);
    gfx_round_frame(c, 10, fy, w - 20, fh, 9, 1.0f, C_ACCENT);
    if (k->buf[0]) {
        int e = gfx_text(c, &FONT_VALUE, 20, fy + 25, k->buf, C_TEXT, OPAQUE);
        if (k->mode == KB_NUMBER && k->unit) {
            e = gfx_text(c, &FONT_SMALL, e + 6, fy + 24, k->unit, C_TEXT_DIM, OPAQUE);
        } else {
            gfx_fill(c, e + 2, fy + 8, 2, 20, C_ACCENT);  // Schreibmarke
        }
    } else if (k->mode == KB_NUMBER) {
        gfx_text(c, &FONT_SMALL, 20, fy + 23, "keine Angabe", C_TEXT_FAINT, OPAQUE);
    }
    // Hinweis: quer im Feld (unter dem Feld beginnen schon die Tasten), hochkant darunter
    char hint[48];
    if (k->mode == KB_TEXT) {
        snprintf(hint, sizeof hint, portrait ? "%d/%d Zeichen" : "%d/%d", utf8_chars(k->buf),
                 k->max_chars);
    } else if (portrait) {
        snprintf(hint, sizeof hint, "%d bis %d%s%s", k->min_val, k->max_val, k->unit ? " " : "",
                 k->unit ? k->unit : "");
    } else {
        snprintf(hint, sizeof hint, "%d–%d", k->min_val, k->max_val);
    }
    gfx_text(c, &FONT_SMALL, w - (portrait ? 14 : 22) - gfx_text_width(&FONT_SMALL, hint),
             portrait ? fy + fh + 18 : fy + 23, hint, C_TEXT_FAINT, OPAQUE);

    kkey_t keys[MAX_KEYS];
    int n = layout(k, w, h, keys);
    const bool ok_on = kb_can_ok(k);
    for (int i = 0; i < n; i++) {
        const kkey_t *kk = &keys[i];
        const uint32_t fill = gfx_mix(C_PANEL, C_TEXT, 14);
        switch (kk->kind) {
            case K_CHAR: {
                key_face(c, kk, fill, true);
                char s[4];
                snprintf(s, sizeof s, "%s", kk->s);
                if (k->mode == KB_TEXT && k->shift) {
                    upper(s);
                }
                centered(c, &FONT_LABEL, kk, s, C_TEXT);
                break;
            }
            case K_SHIFT: {
                key_face(c, kk, k->shift ? C_ACCENT : C_PANEL, true);
                const uint32_t col = k->shift ? 0xFFFFFFu : C_TEXT_DIM;
                const float cx = (float)kk->x + (float)kk->w / 2.0f, cy = (float)kk->y + (float)kk->h / 2.0f;
                gfx_triangle(c, cx, cy - 9.0f, cx - 8.0f, cy + 1.0f, cx + 8.0f, cy + 1.0f, col,
                             OPAQUE);
                gfx_fill(c, (int)cx - 3, (int)cy + 1, 6, 7, col);
                break;
            }
            case K_BACK: {
                key_face(c, kk, k->mode == KB_NUMBER ? fill : C_PANEL, true);
                const float cx = (float)kk->x + (float)kk->w / 2.0f, cy = (float)kk->y + (float)kk->h / 2.0f;
                gfx_triangle(c, cx - 11.0f, cy, cx - 2.0f, cy - 8.0f, cx - 2.0f, cy + 8.0f,
                             C_TEXT_DIM, OPAQUE);
                gfx_fill(c, (int)cx - 3, (int)cy - 4, 14, 9, C_TEXT_DIM);
                gfx_line(c, cx + 1.0f, cy - 2.0f, cx + 7.0f, cy + 3.0f, 1.6f, C_PANEL, OPAQUE);
                gfx_line(c, cx + 1.0f, cy + 3.0f, cx + 7.0f, cy - 2.0f, 1.6f, C_PANEL, OPAQUE);
                break;
            }
            case K_SPACE:
                key_face(c, kk, fill, true);
                centered(c, &FONT_SMALL, kk, "Leer", C_TEXT_DIM);
                break;
            case K_OK:
                key_face(c, kk, ok_on ? C_ACCENT : C_PANEL, !ok_on);
                centered(c, &FONT_LABEL, kk, "OK", ok_on ? 0xFFFFFFu : C_TEXT_FAINT);
                break;
        }
    }
}
