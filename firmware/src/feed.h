// Übergabe der Messblöcke von Kern 0 (Erfassung, USB) an Kern 1 (Display).
//
// Ein Erzeuger, ein Verbraucher, ohne Sperren. Ist der Puffer voll, wird der Block für das
// Display verworfen; Erfassung und USB-Übertragung laufen davon unberührt weiter.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FEED_MAX_SAMPLES 50u  // 1000 Hz × 50 ms
#define FEED_BLOCKS 32u       // 1,6 s Vorrat

typedef struct {
    uint32_t raw[FEED_MAX_SAMPLES];
    uint32_t seq;  // zählt jeden Block, auch verworfene: Lücken sind so erkennbar
    uint8_t n;
    uint8_t flags;
    uint16_t factor;
} feed_block_t;

// Kern 0
bool feed_push(const uint32_t *raw, uint8_t n, uint8_t flags, uint16_t factor);
// Kern 1
bool feed_pop(feed_block_t *out);

// Zustand, den Kern 0 für die Anzeige bereitstellt
typedef struct {
    volatile bool usb_mounted;
    volatile bool streaming;
    volatile uint32_t display_id;  // vom Display gelesene Kennung (Kern 1 schreibt)
    volatile bool display_up;      // Display initialisiert, display_id gültig
    volatile uint16_t vsys_mv;     // letzte VSYS-Messung (0: noch keine)
    volatile bool no_pause;        // Kern 1 nimmt auf: Erfassung nicht unterbrechen
} link_state_t;

extern link_state_t g_link;
