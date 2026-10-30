// Bedienung auf dem Gerät: Live-Ansicht, Menü, Personenprofile mit Tastatur, Einstellungen,
// Messung mit Auswertung, Ergebnisseiten mit Check-Liste, Touch-Einrichtung und Info, quer
// (320 × 240) und hochkant (240 × 320). Ohne Hardware-Bezug: Der Display-Task liefert
// Messblöcke und Touch-Ereignisse und führt die Anforderungen (req_*) aus; die Vorschau am
// Laptop nutzt denselben Code.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ana.h"
#include "check.h"
#include "gesture.h"
#include "gfx.h"
#include "keyboard.h"
#include "monitor.h"
#include "profile.h"
#include "settings.h"

typedef enum {
    APP_LIVE,
    APP_MENU,
    APP_SETTINGS,
    APP_ANALYZING,
    APP_RESULT,
    APP_CAL,
    APP_INFO,
    APP_PROFILES,      // Person wählen
    APP_PROFILE_EDIT,  // Angaben eines Profils
    APP_KEYBOARD,      // Eingabe für ein Feld des Profils
    APP_CHECK_DETAIL,  // Erklärung zu einem Punkt der Check-Liste
} app_screen_t;

// Übersicht, Check, Verlauf, HRV, Schlagform, Hinweise
#define APP_PAGES 6
#define PAGE_CHECK 1

typedef struct {
    // vom Display-Task gesetzt
    canvas_t *c;
    monitor_t *mon;
    ana_recording_t *rec;
    ana_result_t *res;
    ui_settings_t *ui;
    void *scratch;  // Arbeitsspeicher der Auswertung (Bildspeicher)
    size_t scratch_size;
    const char *version;
    uint32_t display_id;

    app_screen_t screen;
    bool touch_xy;  // Koordinaten nutzbar (eingerichtet)

    // Messung
    bool measuring;
    bool measure_full;
    bool meas_usb_seen;
    int meas_test_blocks;
    uint32_t last_seq;
    bool have_seq;
    int analyze_in;  // Bilder bis zur Auswertung (erst „läuft“ zeigen)
    bool has_result;
    ana_status_t result_status;
    bool result_test;
    int page;

    // Person der laufenden Messung (Schnappschuss, damit die Bewertung zu ihr passt)
    profile_t meas_profile;
    bool meas_known;  // false: Gast
    check_item_t checks[CHK_MAX];
    int n_checks;
    int detail;  // gewählter Punkt der Check-Liste

    // Profile
    int edit_idx;
    bool edit_new;
    int kb_field;
    kb_t kb;

    // Status
    bool usb, streaming;
    float vsys_ema;  // geglättete Batteriespannung in mV (0: keine Messung)
    int battery_pct;  // -1: keine verlässliche Anzeige
    bool low_warned;

    // Einrichtung des Touch
    int cal_step;
    int cal_bad;
    float cal_rx[5], cal_ry[5];

    // Hinweis unten (verschwindet nach kurzer Zeit)
    char toast[96];
    uint32_t toast_until;
    bool toast_shown;

    // Anforderungen an den Display-Task (nach jedem Aufruf prüfen und zurücksetzen)
    bool req_push_all;
    bool req_save;
    bool req_rotate;
    int req_backlight;  // Promille, -1 = unverändert
} app_t;

void app_init(app_t *a);
// nach dem Startbildschirm: Touch einrichten (falls nötig) oder Live-Ansicht
void app_start(app_t *a, bool touch_hw_ok, uint32_t now_ms);
// Messblock wie vom Gerät (25 Werte bei 500 Hz oder 50 bei 1000 Hz); seq zählt Blöcke
void app_feed(app_t *a, const uint32_t *raw, int n, uint8_t flags, uint32_t factor, uint32_t seq);
void app_event(app_t *a, ui_event_t ev, uint32_t now_ms);
// Touch neu einrichten (Notausgang, wenn die Kalibrierung nicht mehr stimmt)
void app_calibrate(app_t *a);
void app_set_status(app_t *a, bool usb, bool streaming);
// neue VSYS-Messung von Kern 0; on_usb: gemessen, während USB verbunden war. Am USB liegt
// VSYS bei 4,6–4,9 V; so zeigt sich, ob das Board den Teiler des Pico 2 (VSYS/3) hat.
void app_vsys_sample(app_t *a, int vsys_mv, bool on_usb);
// regelmäßig aufrufen (~30/s); liefert die neu zu übertragenden Bereiche
int app_frame(app_t *a, uint32_t now_ms, rect_t *dirty, int max_dirty);
// Helligkeit der aktuellen Stufe (Promille)
int app_backlight(const app_t *a);
// Batterie in Prozent aus VSYS (3 Alkali-Zellen), -1 wenn unplausibel
int app_battery_percent(int vsys_mv);
