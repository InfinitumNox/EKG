// Gemeinsame Typen und Helfer der Bildschirme (app.c und die Dateien der Betriebsarten).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"

#define HEADER_H 26

typedef struct {
    int x, y, w, h;
} box_t;

// Maße des aktuellen Bildes: quer 320 × 240, hochkant 240 × 320
typedef struct {
    int w, h;
    bool portrait;
    int footer_y;  // Knopfleiste der Ergebnisseiten
} lay_t;

lay_t lay(const app_t *a);
bool hit(box_t b, int x, int y);
void fmt_num(char *buf, size_t n, double v, int digits);
void fmt_duration(char *buf, size_t n, double s);
void text_center(canvas_t *c, const font_t *f, int cx, int baseline, const char *s, uint32_t col);
void button(canvas_t *c, box_t b, const char *label, bool primary, bool enabled);
void close_icon(canvas_t *c, int cx, int cy, uint32_t col);
void dim_screen(canvas_t *c);
void header(app_t *a, const char *title, const char *sub);
box_t close_box(lay_t l);
void redraw(app_t *a);

// Atem-Modus (breath.c)
void breath_live_reset(app_t *a);
void breath_live_feed(app_t *a);
void breath_draw_live(app_t *a);
int breath_result_pages(void);
const char *breath_page_name(int page);
void breath_draw_result(app_t *a);
void breath_finish(app_t *a);
int result_pages(const app_t *a);

// Privater Bereich (privat.c)
box_t info_version_box(lay_t l);
void priv_tap_version(app_t *a, uint32_t now);
void priv_draw(app_t *a);
void priv_event(app_t *a, ui_event_t ev, uint32_t now);
void priv_frame(app_t *a, uint32_t now);

// Atem-Coach (coach.c)
void coach_open(app_t *a, uint32_t now);
void coach_draw(app_t *a);
bool coach_frame(app_t *a, uint32_t now);
bool coach_tap(app_t *a, int x, int y, uint32_t now);
void go_live(app_t *a);
void show_toast(app_t *a, const char *text, uint32_t now);
