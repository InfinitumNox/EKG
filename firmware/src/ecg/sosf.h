// Kausaler Anzeige-Filter (Hochpass 0,5 Hz, Tiefpass 40 Hz, Notch 50 Hz) wie die
// Live-Ansicht am Laptop; Koeffizienten aus ecg_tables.h.

#pragma once

#include <stdbool.h>

#define SOSF_SECTIONS 4

typedef struct {
    float z[SOSF_SECTIONS][2];
    bool primed;
} sosf_t;

void sosf_reset(sosf_t *f);
float sosf_step(sosf_t *f, float x);
