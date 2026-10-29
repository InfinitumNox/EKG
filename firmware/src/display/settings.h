// Einstellungen des Displays (Drehung, Helligkeit) im letzten Flash-Sektor.
//
// Kern 1 meldet Änderungen an; Kern 0 schreibt sie erst, wenn sich drei Sekunden lang
// nichts mehr geändert hat. Beim Schreiben hält flash_safe_execute Kern 1 kurz an.

#pragma once

#include <stdint.h>

typedef struct {
    uint8_t rotation;    // 0 oder 1 (180° gedreht)
    uint8_t brightness;  // Stufe 0 … 2
} ui_settings_t;

void settings_load(ui_settings_t *s);
void settings_request_save(const ui_settings_t *s, uint32_t now_ms);  // Kern 1
void settings_service(uint32_t now_ms);                               // Kern 0
