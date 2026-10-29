#include "gesture.h"

#include <stdlib.h>
#include <string.h>

#define TAP_MIN_MS 30u
#define TAP_MAX_MS 900u
#define LONG_MS 1200u
#define MOVE_PX 22
#define SWIPE_PX 50
#define AVG_POS 3  // Tipp-Ort: Mittel der ersten Messungen (der allererste Wert streut)

void gesture_init(gesture_t *g) {
    memset(g, 0, sizeof *g);
}

static bool moved(const gesture_t *g) {
    return g->n_pos > 0 && (abs(g->x - g->x0) > MOVE_PX || abs(g->y - g->y0) > MOVE_PX);
}

ui_event_t gesture_feed(gesture_t *g, bool pressed, bool has_pos, int x, int y, int rx, int ry,
                        uint32_t now) {
    ui_event_t ev = {EV_NONE, 0, 0, 0, 0, false};
    if (pressed) {
        if (!g->down) {
            g->down = true;
            g->long_sent = false;
            g->t_down = now;
            g->n_pos = 0;
            g->sum_x = g->sum_y = g->sum_rx = g->sum_ry = 0;
        }
        g->up_count = 0;
        if (has_pos) {
            if (g->n_pos < AVG_POS) {
                g->sum_x += x;
                g->sum_y += y;
                g->sum_rx += rx;
                g->sum_ry += ry;
                g->n_pos++;
                g->x0 = (int)(g->sum_x / g->n_pos);
                g->y0 = (int)(g->sum_y / g->n_pos);
                g->rx0 = (int)(g->sum_rx / g->n_pos);
                g->ry0 = (int)(g->sum_ry / g->n_pos);
            }
            g->x = x;
            g->y = y;
        }
        if (!g->long_sent && now - g->t_down >= LONG_MS && !moved(g)) {
            g->long_sent = true;
            ev = (ui_event_t){EV_LONG, g->x0, g->y0, g->rx0, g->ry0, g->n_pos > 0};
        }
        return ev;
    }
    if (!g->down) {
        return ev;
    }
    if (++g->up_count < 2) {  // ein einzelner Aussetzer ist noch kein Loslassen
        return ev;
    }
    g->down = false;
    uint32_t dur = now - g->t_down;
    if (g->long_sent) {
        return ev;
    }
    int dx = g->x - g->x0, dy = g->y - g->y0;
    if (g->n_pos > 0 && abs(dx) >= SWIPE_PX && abs(dx) > 2 * abs(dy)) {
        ev.type = dx < 0 ? EV_SWIPE_LEFT : EV_SWIPE_RIGHT;
        return ev;
    }
    if (dur >= TAP_MIN_MS && dur <= TAP_MAX_MS && !moved(g)) {
        ev = (ui_event_t){EV_TAP, g->x0, g->y0, g->rx0, g->ry0, g->n_pos > 0};
    }
    return ev;
}
