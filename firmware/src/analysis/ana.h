// Auswertung einer Messung direkt auf dem Gerät.
//
// Dieselbe Rechnung wie ekg.analysis.analyse() am Laptop: phasenfreie Filterung,
// R-Zacken mit dem Live-Detektor, HRV (Zeitbereich und Lomb-Scargle), Extraschläge nach
// Lipponen & Tarvainen mit Bestätigung über die QRS-Form, Rhythmus-Hinweise, Median-Schlag
// mit PR/QRS/QT und die Signalqualität (Ausblendung, 50-Hz-Brumm). Nur die Atemschätzung
// fehlt; sie braucht mehr als drei Minuten. host/tests/test_firmware_analysis.py prüft
// C und Python mit denselben Daten.
//
// Speicher: Die Aufnahme liegt als 16-Bit-Rohwert vor (Rohsumme / 4). Während der
// Auswertung wird der Puffer mit dem gefilterten Signal überschrieben; für Zwischenwerte
// bekommt ana_run einen Arbeitsspeicher (im Gerät der Bildspeicher, solange das Display
// das letzte Bild hält).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qrs.h"

#define ANA_FS 500
#define ANA_BLOCK 25                               // Samples je Block (50 ms)
#define ANA_MAX_S 120                              // Messdauer 2 Minuten
#define ANA_MAX_SAMPLES (ANA_FS * ANA_MAX_S)       // 60 000
#define ANA_MAX_BLOCKS (ANA_MAX_SAMPLES / ANA_BLOCK)
#define ANA_MIN_S 30                               // kürzere Messungen werden nicht ausgewertet
#define ANA_MAX_BEATS 640                          // > 300/min über 2 min
#define ANA_MAX_INVALID 256                        // ausgeblendete Abschnitte
#define ANA_RAW_GAP 0xFFFFu                        // Rohwert für fehlende Samples
#define ANA_RAW_MAX (4095u * 64u)                  // größte Rohsumme (12 Bit, 64-fach)
#define ANA_TMPL_PRE 175                           // Median-Schlag: 350 ms vor R
#define ANA_TMPL_POST 300                          // … und 600 ms danach
#define ANA_TMPL_LEN (ANA_TMPL_PRE + ANA_TMPL_POST)
#define ANA_MAX_HINTS 8
#define ANA_MAX_QNOTES 3
#define ANA_TEXT 200

// Aufnahme: Rohsumme (64-fach, 12 Bit) / 4, gerundet; ANA_RAW_GAP für Lücken
typedef struct {
    uint16_t raw4[ANA_MAX_SAMPLES];
    uint8_t flags[ANA_MAX_BLOCKS];  // EKG_FLAG_* je Block
    uint32_t n_blocks;
} ana_recording_t;

typedef enum {
    ANA_POWER_UNKNOWN,  // z. B. am USB des Laptops
    ANA_POWER_BATTERY,  // Gerät lief durchgehend auf Batterie
} ana_power_t;

typedef struct {
    int n_beats;
    int n_excluded;
    int n_ectopic;
    int n_missed;
    int n_extra;
    double duration_s;
    double mean_rr_ms;
    double mean_hr;
    double min_hr;
    double max_hr;
    double sdnn_ms;
    double rmssd_ms;  // NAN, wenn keine Paare
    double pnn50;
    double sd1_ms;
    double sd2_ms;
    bool has_lf;  // ab 60 s
    double lf_ms2;
    double hf_ms2;
    bool has_lf_hf;
    double lf_hf;
    bool has_nu;
    double lf_nu;
    double hf_nu;
    bool has_peak;
    double peak_hz;
} ana_hrv_t;

typedef struct {
    double baseline_mv;
    int p_onset;  // -1: nicht bestimmbar
    int p_peak;
    int qrs_onset;
    int qrs_offset;
    int t_peak;
    int t_end;
    double pr_ms;  // NAN: nicht bestimmbar
    double qrs_ms;
    double qt_ms;
    double qtc_bazett_ms;
    double qtc_fridericia_ms;
    double p_amp_mv;
    double r_amp_mv;
    double t_amp_mv;
} ana_delineation_t;

// Rhythmus in Zahlen (Grundlage der Hinweise; die Check-Seite des Geräts bewertet damit)
typedef struct {
    bool valid;  // genug Schläge (mind. 20, davon 15 gültige Intervalle)
    double median_hr;  // Herzfrequenz aus dem Median der Intervalle
    bool has_irr;  // Unregelmäßigkeit bestimmbar
    int windows;  // Fenster, die auf Unregelmäßigkeit geprüft wurden
    int irregular_windows;  // davon zufällig unregelmäßig
    int n_ect;  // vorzeitige Schläge (nach QRS-Form bestätigt)
    int n_err;  // vermutete verpasste und zusätzliche Schläge
    bool resp_variation;  // gleichmäßige, atemabhängige Schwankung
} ana_rhythm_t;

typedef struct {
    bool ok;  // Aufnahme lang genug
    int n_samples;
    double duration_s;
    double lead_off_s;
    double gap_s;
    double clipped_s;
    double masked_s;
    bool has_mains;
    double mains_uv;
    bool has_mains_ratio;
    double mains_ratio;

    int n_beats;
    double r_index[ANA_MAX_BEATS];  // R-Zacken (Sample-Index mit Subsample-Anteil)
    bool beats_truncated;

    bool has_hrv;
    ana_hrv_t hrv;
    uint8_t rr_ok[ANA_MAX_BEATS];  // je RR-Intervall: 1 = gültiges NN-Intervall der HRV

    int ect_normal_qrs;
    int ect_other_qrs;
    int ect_not_premature;

    bool has_median;
    int median_beats;
    double mean_rr_s;
    float tmpl[ANA_TMPL_LEN];  // mV, R bei Index ANA_TMPL_PRE
    bool has_delineation;
    ana_delineation_t d;

    ana_rhythm_t rhythm;
    int n_hints;
    char hints[ANA_MAX_HINTS][ANA_TEXT];
    int n_qnotes;
    char qnotes[ANA_MAX_QNOTES][ANA_TEXT];
    uint32_t scratch_peak;  // benötigter Arbeitsspeicher in Byte
} ana_result_t;

typedef enum {
    ANA_OK = 0,
    ANA_TOO_SHORT,
    ANA_NO_MEMORY,
    ANA_TOO_MANY_GAPS,
} ana_status_t;

void ana_rec_clear(ana_recording_t *rec);
// Ein Block mit 25 Rohsummen (64-fach) und seinen Flags; false, wenn die Aufnahme voll ist.
bool ana_rec_add(ana_recording_t *rec, const uint32_t *raw64, int n, uint8_t flags);
// Fehlender Block (Display kam nicht hinterher)
bool ana_rec_add_gap(ana_recording_t *rec);
double ana_rec_seconds(const ana_recording_t *rec);

// Rechnet die Auswertung. det: Arbeitsbereich für die R-Zacken-Erkennung (der Detektor der
// Live-Ansicht kann geliehen werden und muss danach neu lernen). rec wird überschrieben.
ana_status_t ana_run(ana_recording_t *rec, ana_power_t power, qrs_detector_t *det,
                     void *scratch, size_t scratch_size, ana_result_t *out);

// Gefiltertes Signal nach ana_run (mV, NAN in ausgeblendeten Abschnitten)
double ana_filtered_mv(const ana_recording_t *rec, int i);
