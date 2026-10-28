// Dezimation durch Blocksummen: je `factor` ADC-Werte ergeben einen Ausgabewert.
// Die Summe (statt des Mittelwerts) behält die zusätzliche Auflösung aus dem
// Oversampling als Ganzzahl, es wird nichts gerundet.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ADC-Werte bei bzw. jenseits dieser Grenzen gelten als übersteuert.
#define EKG_ADC_CLIP_LOW 8u
#define EKG_ADC_CLIP_HIGH 4087u

// Verarbeitet n_in Eingangswerte (Vielfaches von factor) und liefert die Anzahl der
// Ausgabewerte. *clip wird gesetzt, sobald ein Wert an der Aussteuerungsgrenze liegt.
size_t ekg_decimate(const uint16_t *in, size_t n_in, uint32_t factor, uint32_t *out, bool *clip);
