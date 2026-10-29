// Gesten aus abgetasteten Touch-Werten (~50 Hz): Tippen, langes Drücken, Wischen.
// Ohne Hardware-Bezug, damit die Vorschau am Laptop dieselbe Logik nutzt.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EV_NONE,
    EV_TAP,
    EV_LONG,
    EV_SWIPE_LEFT,
    EV_SWIPE_RIGHT,
} ev_type_t;

typedef struct {
    ev_type_t type;
    int x, y;    // Bildschirm (nur mit Kalibrierung)
    int rx, ry;  // Rohwerte des Touch-Controllers
    bool has_pos;
} ui_event_t;

typedef struct {
    bool down;
    bool long_sent;
    int up_count;
    uint32_t t_down;
    int n_pos;
    long sum_x, sum_y, sum_rx, sum_ry;  // Mittel der ersten Positionen = Tipp-Ort
    int x0, y0, x, y;
    int rx0, ry0;
} gesture_t;

void gesture_init(gesture_t *g);
// pressed: Stift/Finger auf dem Panel; has_pos: Koordinaten gültig
ui_event_t gesture_feed(gesture_t *g, bool pressed, bool has_pos, int x, int y, int rx, int ry,
                        uint32_t now_ms);
