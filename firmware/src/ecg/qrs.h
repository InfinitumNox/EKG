// QRS-Erkennung nach Pan & Tompkins als Streaming-Detektor.
//
// Zeile für Zeile wie host/src/ekg/dsp/qrs.py (gleiche Filter, Schwellen, Search-Back,
// T-Wellen-Unterscheidung und Lokalisierung); host/tests/test_firmware_ecg.py vergleicht
// beide mit identischen Eingangsblöcken. Arbeitet mit 500 Hz (ECG_FS).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecg_tables.h"

#define QRS_HIST_CAP 4096u  // Zweierpotenz >= QRS_KEEP + ein Block
#define QRS_MAX_BLOCK 64u   // höchstens so viele Samples je Aufruf (Gerät: 25)
#define QRS_MAX_CANDS 128

typedef struct {
    double index;         // R-Zacke als absoluter Sample-Index (mit Subsample-Anteil)
    double amplitude_mv;  // Ausschlag gegen die lokale Grundlinie, vorzeichenbehaftet
    bool searchback;
} qrs_beat_t;

typedef struct {
    int64_t index;
    double value;
    double slope;
    bool t_wave;
} qrs_cand_t;

typedef struct {
    double z[2][2];  // Zustand je Abschnitt
} qrs_sos_state_t;

typedef struct {
    qrs_sos_state_t bp, ref;
    bool primed;
    double dx[4];  // Bandpass-Ausgang x[n-1] … x[n-4] für die Ableitung
    double sq_win[QRS_W];
    unsigned sq_pos;
    double sq_sum;

    double mwi_h[QRS_HIST_CAP];
    double sq_h[QRS_HIST_CAP];
    double ref_h[QRS_HIST_CAP];

    bool has_origin;
    int64_t origin;
    int64_t next;
    int64_t hist_start;
    bool initialized;
    int64_t scan_from;
    int64_t init_index;
    int64_t last_stall_decay;

    double spki;
    double npki;
    bool has_last_qrs;
    int64_t last_qrs;
    double last_slope;
    int rr_recent[8];
    int n_recent;
    int rr_selected[8];
    int n_selected;
    qrs_cand_t cands[QRS_MAX_CANDS];
    int n_cands;
    int polarity;
} qrs_detector_t;

void qrs_init(qrs_detector_t *d);
// Neu lernen lassen; der nächste Block beginnt bei start_index.
void qrs_reset(qrs_detector_t *d, int64_t start_index);
// Verarbeitet einen Block (mV); liefert die Anzahl gefundener Schläge in out.
size_t qrs_process(qrs_detector_t *d, const double *x, size_t n, qrs_beat_t *out,
                   size_t max_out);
bool qrs_initialized(const qrs_detector_t *d);
int64_t qrs_next_index(const qrs_detector_t *d);
