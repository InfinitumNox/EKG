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
#include "private_data.h"
#include "profile.h"
#include "settings.h"
#include "solo.h"

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
    APP_LEADS,         // Anleitung: welche Elektrode wohin
    APP_BREATH_LIVE,   // Atem-Messung läuft
    APP_COACH,         // Atem-Coach
    APP_PRIV_PIN,      // PIN eingeben oder festlegen
    APP_PRIV_HOME,
    APP_PRIV_SESSION,
    APP_PRIV_DONE,
    APP_PRIV_HISTORY,
} app_screen_t;

// Übersicht, Check, Verlauf, HRV, Schlagform, Hinweise
#define APP_PAGES 6
#define PAGE_CHECK 1

typedef enum { MODE_EKG, MODE_BREATH } app_mode_t;

// Zustand des privaten Bereichs (nur im RAM, bis gespeichert wird)
typedef struct {
    uint8_t pin_mode;  // 0 eingeben, 1 neu festlegen, 2 Wiederholung
    bool changing;     // PIN ändern aus dem Bereich heraus
    char digits[PRIV_PIN_DIGITS + 1];
    char first[PRIV_PIN_DIGITS + 1];
    int n_digits;
    uint8_t fails;
    uint32_t locked_until;
    uint8_t prompt;    // 0 keine Rückfrage, 1 Sessions löschen, 2 PIN vergessen
    uint32_t tap_at[5];
    int n_taps;
    // Session
    solo_model_t model;
    uint8_t hr[SOLO_MAX_S];  // Puls je Sekunde, 0 = Lücke
    int n_s;
    uint32_t sec_at, draw_at;
    float start_hr;
    int percent;             // -1: nicht gezeigt
    priv_session_t last;     // Ergebnis der Session
    int t90_s;               // Sekunde, in der die Anzeige 90 % erreichte, -1 nie
} priv_ui_t;

// Atem-Coach (eine Übung, nichts wird gespeichert)
typedef struct {
    uint8_t tempo;     // Atemzüge pro Minute, 4 bis 10
    uint8_t dur_min;   // 2 oder 5
    uint32_t t0;       // Beginn (oder letzte Änderung) in ms
    uint32_t drawn_at, pulse_at;
    int n_pulse;
    uint8_t pulse[60];  // Puls je Sekunde der letzten Minute, 0 = fehlt
} coach_t;

// Reihe der Atem-Messung (ein Wert je Schlag)
#define BL_MAX 360
typedef struct {
    int n;
    double t[BL_MAX];  // Zeit des Schlags in s
    double a[BL_MAX];  // QRS-Amplitude in mV
    float hr[BL_MAX];  // Herzfrequenz beim Schlag
    bool has_est;
    double est_hz;     // laufende Schätzung (nur Amplitude, ab etwa 45 s)
} breath_live_t;

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
    app_mode_t mode;  // Betriebsart im Menü; nach dem Einschalten immer EKG (nicht gespeichert)
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

    // Atmung: laufende Messung und letztes Ergebnis (die Auswertung belegt die Aufnahme, ein
    // Atem-Ergebnis ersetzt deshalb das letzte EKG-Ergebnis)
    app_mode_t meas_mode;    // Betriebsart der laufenden Messung
    uint8_t result_kind;     // gezeigtes Ergebnis: 0 EKG, 1 Atmung
    bool has_breath;
    ana_breath_t breath;
    double breath_rmssd, breath_hr;
    int breath_signal;       // 0 gut, 1 leicht, 2 stark gestört (check_quality)
    bool breath_test;
    breath_live_t bl;
    coach_t coach;
    priv_ui_t priv;
    priv_data_t *pd;      // vom Display-Task: Daten des privaten Bereichs
    bool req_priv_save;
    uint32_t seen_beat_seq;
    uint32_t bl_draw_at;     // nächste Aktualisierung der Atem-Ansicht
    uint32_t clock;          // letzte bekannte Zeit (ms), für Ansichten ohne eigenen Zeitparameter
    uint32_t bl_est_at;

    // Elektroden-Anleitung: öffnet sich von selbst, wenn die Elektroden lange keinen Kontakt haben
    bool leads_auto;        // von selbst geöffnet (schließt sich bei Kontakt wieder)
    bool lead_active;       // Kontaktverlust läuft
    bool lead_dismissed;    // wurde geschlossen, solange der Kontakt fehlt
    bool lead_ok_active;
    uint8_t leads_shown;    // gezeigter Zustand (EKG_FLAG_LO_*)
    uint32_t lead_off_since;
    uint32_t lead_ok_since;

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
