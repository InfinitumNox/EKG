// Signal-Teil der Auswertung (intern, siehe ana_wave.c).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ana_int.h"

typedef struct {
    int s, e;  // Samples [s, e)
} ana_run_t;

typedef struct {
    double index;
    double amplitude_mv;
    bool searchback;
} ana_beat_t;

double ana_raw_mv(const ana_recording_t *rec, int i);  // NAN in Lücken
bool ana_mask_get(const uint8_t *mask, int i);
// Lead-off, Übersteuerung (samt 0,5 s Rand) und Lücken; false bei zu vielen Abschnitten
bool ana_invalid_mask(const ana_recording_t *rec, int n, uint8_t *mask, ana_run_t *runs,
                      int max_runs, int *n_runs);
bool ana_mains_level(const ana_recording_t *rec, int n, ana_arena_t *a, double *mains_uv,
                     bool *has_ratio, double *ratio);
int ana_detect(const ana_recording_t *rec, int n, const ana_run_t *runs, int n_runs,
               qrs_detector_t *det, ana_beat_t *beats, int max_beats, bool *truncated);
// überschreibt rec->raw4 mit dem gefilterten Signal (ana_filtered_mv)
bool ana_zero_phase(ana_recording_t *rec, int n, const uint8_t *mask, const ana_run_t *runs,
                    int n_runs, ana_arena_t *a);
void ana_refine(const ana_recording_t *rec, int n, ana_beat_t *beats, int nb, ana_arena_t *a);
bool ana_median_beat(const ana_recording_t *rec, int n, const double *r, int nr,
                     double *tmpl, int *n_good, double *mean_rr_s, ana_arena_t *a);
bool ana_delineate(const double *x, int len, int r, double mean_rr_s, ana_delineation_t *o,
                   ana_arena_t *a);
void ana_confirm_ectopics(const ana_recording_t *rec, int n, const double *r, int nr,
                          const int *cand, int n_cand, const double *tmpl, bool has_tmpl,
                          ana_ectopy_t *out, ana_arena_t *a);
