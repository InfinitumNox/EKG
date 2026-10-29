// Touch-Kalibrierung: affine Abbildung Rohwert -> Bildschirm, ausgeglichen über mehrere
// Punkte. Erfasst Vertauschung und Spiegelung der Achsen ohne Annahmen über das Panel.

#pragma once

#include <stdbool.h>

typedef struct {
    float a, b, c;  // x = a·rx + b·ry + c
    float d, e, f;  // y = d·rx + e·ry + f
} touch_cal_t;

// Ausgleichsrechnung über n ≥ 3 Punkte; max_err: größte Abweichung in Pixeln
bool touchcal_solve(const float *sx, const float *sy, const float *rx, const float *ry, int n,
                    touch_cal_t *out, float *max_err);
void touchcal_apply(const touch_cal_t *cal, int rx, int ry, int *x, int *y);
