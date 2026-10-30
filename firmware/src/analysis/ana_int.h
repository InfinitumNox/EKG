// Interne Schnittstellen der Auswertung (nur für ana*.c und die Host-Tests).

#pragma once

#include <stdbool.h>

#include "ana.h"
#include "ana_math.h"

#define ANA_RR_MIN_S 0.25
#define ANA_RR_MAX_S 2.5

typedef struct {
    double s, e;  // Sekunden
} ana_interval_t;

typedef struct {
    const ana_interval_t *iv;
    int n;
} ana_invalid_t;

// RR-Klassifikation nach Lipponen & Tarvainen (ekg.dsp.artifacts)
typedef struct {
    int n;
    int *ectopic, n_ectopic;
    int *missed, n_missed;
    int *extra, n_extra;
    int *longshort, n_longshort;
} ana_rrclass_t;

bool ana_classify_rr(const double *rr, int n, ana_rrclass_t *cls, ana_arena_t *a);
void ana_valid_mask(const ana_rrclass_t *cls, bool *ok);
// gültige NN-Intervalle; bad darf NULL sein
bool ana_nn_mask(const double *rr, int n, const bool *bad, bool *ok, ana_rrclass_t *cls,
                 ana_arena_t *a);
void ana_rr_overlap_mask(const double *t, int n_t, ana_invalid_t inv, bool *bad);
// ok_out (darf NULL sein): n_t - 1 Einträge, gültige NN-Intervalle
bool ana_compute_hrv(const double *t, int n_t, ana_invalid_t inv, ana_hrv_t *out,
                     uint8_t *ok_out, ana_arena_t *a);

typedef struct {
    int normal_qrs;
    int other_qrs;
    int not_premature;
} ana_ectopy_t;

typedef struct {
    int windows;
    int irregular_windows;
    double nrmssd;
    double sd1_sd2;
    double tpr;
} ana_irregularity_t;

bool ana_irregularity(const double *rr, int n, const bool *bad, ana_irregularity_t *out,
                      ana_arena_t *a);
// Hinweise wie ekg.dsp.hrv.rhythm_hints; ectopy und rh dürfen NULL sein
int ana_rhythm_hints(const double *t, int n_t, ana_invalid_t inv, const ana_ectopy_t *ectopy,
                     char hints[][ANA_TEXT], int max_hints, ana_arena_t *a, ana_rhythm_t *rh);
