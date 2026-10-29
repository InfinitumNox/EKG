// Ablauf der Auswertung wie ekg.analysis.analyse().

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ana_wave.h"
#include "protocol.h"

void ana_rec_clear(ana_recording_t *rec) {
    rec->n_blocks = 0;
}

bool ana_rec_add(ana_recording_t *rec, const uint32_t *raw64, int n, uint8_t flags) {
    if (rec->n_blocks >= ANA_MAX_BLOCKS || n != ANA_BLOCK) {
        return false;
    }
    uint16_t *dst = rec->raw4 + rec->n_blocks * ANA_BLOCK;
    for (int i = 0; i < ANA_BLOCK; i++) {
        uint32_t v = raw64[i] > ANA_RAW_MAX ? ANA_RAW_MAX : raw64[i];
        dst[i] = (uint16_t)((v + 2u) / 4u);
    }
    rec->flags[rec->n_blocks++] = flags;
    return true;
}

bool ana_rec_add_gap(ana_recording_t *rec) {
    if (rec->n_blocks >= ANA_MAX_BLOCKS) {
        return false;
    }
    uint16_t *dst = rec->raw4 + rec->n_blocks * ANA_BLOCK;
    for (int i = 0; i < ANA_BLOCK; i++) {
        dst[i] = ANA_RAW_GAP;
    }
    rec->flags[rec->n_blocks++] = EKG_FLAG_OVERRUN;
    return true;
}

double ana_rec_seconds(const ana_recording_t *rec) {
    return (double)rec->n_blocks * ANA_BLOCK / ANA_FS;
}

static void add_qnote(ana_result_t *out, const char *text) {
    if (out->n_qnotes < ANA_MAX_QNOTES) {
        snprintf(out->qnotes[out->n_qnotes++], ANA_TEXT, "%s", text);
    }
}

// wie ekg.dsp.signal_quality.assess_recording, Texte für das Gerät
static void quality_notes(ana_result_t *out, ana_power_t power) {
    if (out->has_mains_ratio) {
        if (out->mains_ratio > 0.5 && power == ANA_POWER_BATTERY) {
            add_qnote(out,
                      "Sehr starker 50-Hz-Brumm (vor dem Filtern so groß wie das EKG), obwohl das "
                      "Gerät auf Batterie lief: Elektroden (frisch, Gel, Haut entfettet) und "
                      "Kabelführung prüfen.");
        } else if (out->mains_ratio > 0.5) {
            add_qnote(out,
                      "Sehr starker 50-Hz-Brumm (vor dem Filtern so groß wie das EKG): "
                      "Elektrodenkontakt prüfen – und sicherstellen, dass der Laptop nicht am "
                      "Netzteil hing.");
        } else if (out->mains_ratio > 0.1) {
            add_qnote(out, "Erhöhter 50-Hz-Brumm (wird herausgefiltert): Elektrodenkontakt prüfen.");
        }
    }
    if (out->masked_s > 1.0) {
        char buf[ANA_TEXT];
        snprintf(buf, sizeof buf,
                 "%ld s ausgeblendet (Übersteuerung durch Bewegung oder fehlender Kontakt).",
                 (long)ana_round(out->masked_s));
        add_qnote(out, buf);
    }
}

ana_status_t ana_run(ana_recording_t *rec, ana_power_t power, qrs_detector_t *det,
                     void *scratch, size_t scratch_size, ana_result_t *out) {
    memset(out, 0, sizeof *out);
    const double fs = ANA_FS;
    const int n = (int)rec->n_blocks * ANA_BLOCK;
    out->n_samples = n;
    out->duration_s = (double)n / fs;
    if (out->duration_s < ANA_MIN_S) {
        return ANA_TOO_SHORT;
    }
    ana_arena_t a = {(uint8_t *)scratch, scratch_size, 0, false, 0};
    uint8_t *mask = ANA_NEW(&a, uint8_t, (n + 7) / 8);
    ana_run_t *runs = ANA_NEW(&a, ana_run_t, ANA_MAX_INVALID);
    ana_interval_t *iv = ANA_NEW(&a, ana_interval_t, ANA_MAX_INVALID);
    if (a.failed) {
        return ANA_NO_MEMORY;
    }

    // Lead-off, Übersteuerung und Lücken samt Rand ausblenden
    int n_runs = 0;
    if (!ana_invalid_mask(rec, n, mask, runs, ANA_MAX_INVALID, &n_runs)) {
        return ANA_TOO_MANY_GAPS;
    }
    for (int k = 0; k < n_runs; k++) {
        iv[k].s = (double)runs[k].s / fs;
        iv[k].e = (double)runs[k].e / fs;
    }
    ana_invalid_t inv = {iv, n_runs};
    long lead_off = 0, gaps = 0, clipped = 0, masked = 0;
    for (uint32_t b = 0; b < rec->n_blocks; b++) {
        uint8_t f = rec->flags[b];
        lead_off += (f & (EKG_FLAG_LO_PLUS | EKG_FLAG_LO_MINUS)) ? ANA_BLOCK : 0;
        clipped += (f & EKG_FLAG_CLIP) ? ANA_BLOCK : 0;
        gaps += rec->raw4[b * ANA_BLOCK] == ANA_RAW_GAP ? ANA_BLOCK : 0;
    }
    for (int i = 0; i < n; i++) {
        masked += ana_mask_get(mask, i);
    }
    out->lead_off_s = (double)lead_off / fs;
    out->gap_s = (double)gaps / fs;
    out->clipped_s = (double)clipped / fs;
    out->masked_s = (double)masked / fs;

    out->has_mains = ana_mains_level(rec, n, &a, &out->mains_uv, &out->has_mains_ratio,
                                     &out->mains_ratio);
    if (a.failed) {
        return ANA_NO_MEMORY;
    }
    quality_notes(out, power);

    // erst nach der Brumm-Messung anlegen: die FFT braucht den meisten Platz
    ana_beat_t *beats = ANA_NEW(&a, ana_beat_t, ANA_MAX_BEATS);
    double *t = ANA_NEW(&a, double, ANA_MAX_BEATS);
    double *tmpl = ANA_NEW(&a, double, ANA_TMPL_LEN);
    if (a.failed) {
        return ANA_NO_MEMORY;
    }

    // R-Zacken mit dem Live-Detektor; Schläge in ausgeblendeten Abschnitten verwerfen
    int nb = ana_detect(rec, n, runs, n_runs, det, beats, ANA_MAX_BEATS, &out->beats_truncated);
    int k = 0;
    for (int i = 0; i < nb; i++) {
        double idx_f = beats[i].index;
        int idx = (int)idx_f;  // int() schneidet ab wie in Python
        if (idx_f >= 0 && idx < n && !ana_mask_get(mask, idx)) {
            beats[k++] = beats[i];
        }
    }
    nb = k;

    if (!ana_zero_phase(rec, n, mask, runs, n_runs, &a)) {
        return ANA_NO_MEMORY;
    }
    ana_refine(rec, n, beats, nb, &a);
    out->n_beats = nb;
    for (int i = 0; i < nb; i++) {
        out->r_index[i] = beats[i].index;
        t[i] = beats[i].index / fs;
    }

    out->has_hrv = ana_compute_hrv(t, nb, inv, &out->hrv, out->rr_ok, &a);

    out->has_median = ana_median_beat(rec, n, out->r_index, nb, tmpl, &out->median_beats,
                                      &out->mean_rr_s, &a);
    if (out->has_median) {
        for (int i = 0; i < ANA_TMPL_LEN; i++) {
            out->tmpl[i] = (float)tmpl[i];
        }
        out->has_delineation =
            ana_delineate(tmpl, ANA_TMPL_LEN, ANA_TMPL_PRE, out->mean_rr_s, &out->d, &a);
    }

    // Extraschläge: Zeitpunkt und QRS-Form prüfen
    ana_ectopy_t ect = {0, 0, 0};
    if (nb > 1) {
        size_t mark = a.used;
        double *rr = ANA_NEW(&a, double, nb - 1);
        bool *bad = ANA_NEW(&a, bool, nb - 1);
        bool *ok = ANA_NEW(&a, bool, nb - 1);
        if (a.failed) {
            return ANA_NO_MEMORY;
        }
        for (int i = 0; i + 1 < nb; i++) {
            rr[i] = t[i + 1] - t[i];
        }
        ana_rr_overlap_mask(t, nb, inv, bad);
        ana_rrclass_t cls;
        if (ana_nn_mask(rr, nb - 1, bad, ok, &cls, &a)) {
            ana_confirm_ectopics(rec, n, out->r_index, nb, cls.ectopic, cls.n_ectopic, tmpl,
                                 out->has_median, &ect, &a);
        }
        a.used = mark;
    }
    out->ect_normal_qrs = ect.normal_qrs;
    out->ect_other_qrs = ect.other_qrs;
    out->ect_not_premature = ect.not_premature;

    out->n_hints = ana_rhythm_hints(t, nb, inv, &ect, out->hints, ANA_MAX_HINTS, &a);
    out->ok = !a.failed;
    out->scratch_peak = (uint32_t)a.peak;
    return a.failed ? ANA_NO_MEMORY : ANA_OK;
}
