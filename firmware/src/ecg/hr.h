// Herzfrequenz aus R-Zacken wie host/src/ekg/dsp/hr.py (HeartRateTracker): Median der
// letzten acht plausiblen RR-Intervalle, veraltet nach einer langen Pause.

#pragma once

#include <stdbool.h>

#define HR_N_INTERVALS 8

typedef struct {
    double fs;
    double r[HR_N_INTERVALS + 1];
    int n;
} hr_tracker_t;

void hr_init(hr_tracker_t *h, double fs);
void hr_reset(hr_tracker_t *h);
void hr_add(hr_tracker_t *h, double r_index);
// Herzfrequenz in /min; false, solange zu wenige Schläge da sind oder der letzte zu alt ist.
bool hr_bpm(const hr_tracker_t *h, double now_index, double *bpm);
bool hr_last_rr_ms(const hr_tracker_t *h, double *rr_ms);
