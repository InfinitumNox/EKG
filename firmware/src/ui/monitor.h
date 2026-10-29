// Live-Ansicht: laufende EKG-Kurve, Herzfrequenz, Signalstatus.
//
// Bekommt die Rohwerte blockweise wie der Laptop (25 Werte je 50 ms bei 500 Hz, 50 bei
// 1000 Hz) und rechnet Filter, QRS-Erkennung und HF selbst. Zeichnet in den Bildspeicher
// und meldet, welche Bereiche neu zum Display müssen. Hardwareunabhängig.
//
// Maßstab wie auf EKG-Papier: Raster 5 mm, Verstärkung in mm/mV, Vorschub in mm/s. Das
// 2,8″-Panel hat 0,18 mm Pixelabstand (320 Pixel auf 57,6 mm).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "hr.h"
#include "qrs.h"
#include "sosf.h"

#define MON_PX_PER_MM (320.0f / 57.6f)

typedef enum {
    MON_LEARNING,
    MON_SEARCHING,
    MON_GOOD,
    MON_CLIPPED,
    MON_LEAD_OFF,
} mon_status_t;

typedef enum {
    MON_GAIN_AUTO,
    MON_GAIN_5,   // 5 mm/mV
    MON_GAIN_10,  // 10 mm/mV
    MON_GAIN_20,  // 20 mm/mV
    MON_GAIN_COUNT,
} mon_gain_t;

typedef enum {
    MON_SPEED_12,  // 12,5 mm/s
    MON_SPEED_25,  // 25 mm/s
    MON_SPEED_50,  // 50 mm/s
    MON_SPEED_COUNT,
} mon_speed_t;

typedef struct {
    bool portrait;
    int top_h;  // Kopfleiste
    int trace_x0, trace_x1, trace_y0, trace_y1;
    int baseline_y;
    int bottom_y;  // Fußleiste ab hier
} mon_layout_t;

typedef struct {
    canvas_t *c;
    bool draw;  // false: nur rechnen, nicht zeichnen (Startbildschirm, andere Seiten)
    mon_layout_t L;
    uint8_t grid_x[320], grid_y[320];  // Rasterlinien (5 mm)

    qrs_detector_t qrs;
    hr_tracker_t hr;
    sosf_t filt;
    int64_t index;  // nächster Sample-Index (500 Hz)
    double last_mv;
    uint32_t clip_until;  // Blockzähler, bis zu dem „übersteuert“ angezeigt wird
    uint32_t blocks;

    int gain_mode;  // mon_gain_t
    int speed_mode;  // mon_speed_t
    float px_per_sample;
    float col_phase;
    float px_per_mv;
    float pending_px_per_mv;
    float amps[8];
    int n_amps;
    int amp_pos;

    int sweep_x;
    bool col_open;
    float col_min, col_max, col_last;
    float prev_y;
    bool have_prev;
    int dirty_from;  // fortlaufende Spaltennummern seit dem letzten Bild
    int dirty_to;
    int col_counter;

    mon_status_t status;
    int bpm;  // -1 = keine
    int rr_ms;
    uint32_t beat_at;
    bool beat_pending;
    bool test;
    bool usb, streaming;
    int battery_pct;  // -1: keine Anzeige (USB oder keine verlässliche Messung)
    bool measuring;   // Messung für die Auswertung läuft
    int meas_elapsed_s, meas_total_s;
    bool menu_button;  // Menü-Knopf oben rechts (nur mit Touch-Koordinaten)

    // zuletzt gezeichneter Zustand
    mon_status_t shown_status;
    int shown_bpm, shown_rr;
    bool shown_big_heart;
    bool shown_test, shown_usb, shown_streaming;
    int shown_battery;
    bool shown_measuring;
    int shown_elapsed;
    bool shown_menu_button;
    bool overlay;
    float shown_px_per_mv;
    bool need_full;
} monitor_t;

void monitor_init(monitor_t *m, canvas_t *c);
// nach einem Wechsel der Ausrichtung (c->w, c->h): Aufteilung neu berechnen
void monitor_layout(monitor_t *m);
void monitor_set_scale(monitor_t *m, int gain_mode, int speed_mode);
void monitor_draw_all(monitor_t *m);
void monitor_set_link(monitor_t *m, bool usb_mounted, bool streaming);
void monitor_set_battery(monitor_t *m, int pct);  // -1: ausblenden
void monitor_set_measure(monitor_t *m, bool active, int elapsed_s, int total_s);
// Erkennung neu lernen lassen (nach einer Auswertung, die den Detektor geliehen hat)
void monitor_relearn(monitor_t *m);
void monitor_feed(monitor_t *m, const uint32_t *raw, int n, uint8_t flags, uint32_t factor);
// Aktualisiert Anzeige-Elemente; schreibt die geänderten Bereiche nach dirty.
int monitor_frame(monitor_t *m, uint32_t now_ms, rect_t *dirty, int max_dirty);
// Bezeichnungen für Einstellungen und Anzeige
const char *monitor_gain_name(int gain_mode);
const char *monitor_speed_name(int speed_mode);
