// Daten des privaten Bereichs: PIN (nur als Prüfwert) und die letzten Sessions.
// Ohne Hardware-Bezug; das Speichern im Flash steht in display/private_store.c.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRIV_SESSIONS 20
#define PRIV_CURVE 56
#define PRIV_PIN_DIGITS 4
#define PRIV_EXCLUDED 1u  // Session wurde nicht gewertet (zählt nicht fürs Lernen)

typedef struct {
    uint16_t duration_s;
    uint8_t hr_start, hr_peak;
    uint8_t flags;
    int8_t err_10s;  // Anzeige erreichte 90 % so viele 10 s vor Fertig (0: nicht erreicht)
    uint8_t reserved[2];
    uint8_t curve[PRIV_CURVE];  // Puls über die Session, 0 = Lücke
} priv_session_t;

typedef struct {
    bool has_pin;
    uint8_t display_mode;  // 0 = Prozent, 1 = nur Kurve
    uint8_t n;             // Anzahl gespeicherter Sessions
    uint8_t salt[16];
    uint8_t pin_hash[32];
    priv_session_t s[PRIV_SESSIONS];  // älteste zuerst
} priv_data_t;

#define PRIV_RECORD_BYTES 1536u  // Platz im Flash (Vielfaches der Seitengröße 256)

void priv_clear(priv_data_t *d);
// Datensatz mit Prüfsumme; liefert PRIV_RECORD_BYTES (buf mit 0xFF aufgefüllt) oder 0
size_t priv_encode(const priv_data_t *d, uint8_t *buf, size_t cap);
// leere Daten, wenn der Satz fehlt oder beschädigt ist
void priv_decode(const void *flash, priv_data_t *d);

void priv_sha256(const uint8_t *msg, size_t n, uint8_t out[32]);
bool priv_check_pin(const priv_data_t *d, const char *digits);
void priv_set_pin(priv_data_t *d, const char *digits);  // neuer Salt aus priv_random_bytes
// vom Gerät (Zufallsgenerator) bzw. vom Test geliefert
void priv_random_bytes(uint8_t *out, size_t n);

// hängt eine Session an; die älteste fällt heraus, wenn der Platz voll ist
void priv_add_session(priv_data_t *d, const priv_session_t *s);
