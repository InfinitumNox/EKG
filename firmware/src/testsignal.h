// Synthetisches EKG zum Testen ohne Elektroden.
//
// Jeder Schlag besteht aus Gauß-Wellen (P, Q, R, S, T) relativ zur R-Zacke. Die
// RR-Intervalle schwanken mit der Atmung (respiratorische Sinusarrhythmie) und
// leicht zufällig, damit HF- und HRV-Anzeigen realistische Werte bekommen.
// Dieselbe Modellbeschreibung steckt im Host-Simulator (ekg/device/simulator.py).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float fs;
    uint32_t n;         // Sample-Zähler, läuft nach 1 h über (dann wird alles um 3600 s verschoben)
    float t;            // Zeit in s, aus n berechnet (kein Aufsummieren von Rundungsfehlern)
    float r_prev;       // Zeitpunkte der R-Zacken in s
    float r_cur;
    float r_next;
    float rr_prev;      // RR-Intervall, das zu r_prev/r_cur gehört (für die T-Welle)
    float rr_cur;
    float hr_bpm;
    bool mains;         // 50-Hz-Einstreuung hinzufügen
    uint32_t rng;
} ekg_testsig_t;

void ekg_testsig_init(ekg_testsig_t *s, float fs, float hr_bpm);
// Liefert das nächste Sample in mV (Elektrodenspannung).
float ekg_testsig_next_mv(ekg_testsig_t *s);
