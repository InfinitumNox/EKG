// Einstellungen des Displays (Ausrichtung, Anzeige, Maßstab der Kurve, Touch-Kalibrierung,
// Personenprofile) im letzten Flash-Sektor.
//
// Kern 1 meldet Änderungen an; Kern 0 schreibt sie erst, wenn sich drei Sekunden lang
// nichts mehr geändert hat. Beim Schreiben hält flash_safe_execute Kern 1 kurz an.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile.h"
#include "touchcal.h"

typedef struct {
    uint8_t rotation;  // 0 = quer, 1 = hochkant
    uint8_t look;      // 0 = Tag, 1 = Nacht (frühere Stufen 2, 3 gelten als Nacht)
    bool has_cal;      // Touch eingerichtet
    touch_cal_t cal;   // Rohwert -> Bildschirm bei Drehung 0
    uint8_t vsys;      // VSYS-Teiler: VSYS_UNKNOWN, VSYS_OK (wie Pico 2) oder VSYS_NONE
    uint8_t gain;      // Verstärkung der Kurve (mon_gain_t, 0 = Auto)
    uint8_t speed;     // Vorschub (mon_speed_t, 1 = 25 mm/s)
    uint8_t person;    // aktives Profil (0, 1) oder PERSON_GUEST
    uint8_t avatar;    // 0 = nach Profil (Größe, Gewicht), 1 = Standardfigur
    uint8_t cables;    // Farbschema der Elektrodenkabel (0 … CABLE_PRESETS - 1)
    profile_t profiles[PROFILE_COUNT];
} ui_settings_t;

enum { VSYS_UNKNOWN = 0, VSYS_OK = 1, VSYS_NONE = 2 };

#define CABLE_PRESETS 3

// Aktives Profil, NULL bei „Gast“ oder wenn das Profil nicht angelegt ist
static inline const profile_t *settings_person(const ui_settings_t *s) {
    return s->person < PROFILE_COUNT && profile_used(&s->profiles[s->person])
               ? &s->profiles[s->person]
               : NULL;
}

// Speicherformat ohne Hardware-Bezug (von den Host-Tests geprüft)
void settings_defaults(ui_settings_t *s);
// flash: gespeicherter Datensatz (SEK4, ältere Fassungen werden übernommen)
void settings_decode(const void *flash, ui_settings_t *s);
// schreibt den Datensatz in page (Rest 0xFF), liefert seine Größe
size_t settings_encode(const ui_settings_t *s, uint8_t *page, size_t page_size);
uint32_t settings_checksum(const void *p, size_t n_words);

void settings_load(ui_settings_t *s);
void settings_request_save(const ui_settings_t *s, uint32_t now_ms);  // Kern 1
void settings_service(uint32_t now_ms);                               // Kern 0
