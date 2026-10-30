// Atmung aus dem EKG (EDR) für die Messung von 2 Minuten.
//
// Beim Atmen schwankt die QRS-Amplitude im Atemrhythmus, und der Puls schwankt mit
// (Atemrhythmus der Herzfrequenz). Ein Wert zählt nur, wenn die stärkste Spitze der Amplitude
// mit einer deutlichen Spitze im RR-Spektrum übereinstimmt; sonst bleibt er leer. Dasselbe
// Verfahren steht in ekg.dsp.respiration.estimate_breathing_window; host/tests prüft beide.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ana_int.h"

#define BAND_LO 0.1
#define BAND_HI 0.5
#define MATCH_HZ 0.02
#define RATE_LO 0.13  // plausible Atemfrequenz in Ruhe: 8 bis 27 pro Minute
#define RATE_HI 0.45
#define MIN_RR_PEAK 0.25
#define MIN_BEATS 60
#define MIN_SPAN_S 90.0
#define TREND_S 12.0  // Grundlinie der Kurve: gleitendes Mittel über 12 s

// scipy.signal.find_peaks (nur die Lage; ohne Randpunkte, Plateaus in der Mitte)
static int find_peaks(const double *p, int n, int *out, int max) {
    int k = 0;
    for (int i = 1; i + 1 < n && k < max; i++) {
        if (p[i - 1] < p[i]) {
            int j = i;
            while (j + 1 < n && p[j + 1] == p[i]) {
                j++;
            }
            if (j + 1 < n && p[j + 1] < p[i]) {
                out[k++] = (i + j) / 2;
            }
            i = j;
        }
    }
    return k;
}

// Spitze im Atembereich wie ekg.dsp.respiration (Ausreißer der Amplitude entfernt)
static bool amp_peak(const double *t, const double *a, int n, double *hz, ana_arena_t *ar) {
    size_t mark = ar->used;
    double *tg = ANA_NEW(ar, double, n);
    double *ag = ANA_NEW(ar, double, n);
    double *tmp = ANA_NEW(ar, double, n);
    double *f = ANA_NEW(ar, double, LOMB_N);
    double *p = ANA_NEW(ar, double, LOMB_N);
    if (ar->failed) {
        return false;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (isfinite(a[i])) {
            tg[m] = t[i];
            ag[m] = a[i];
            m++;
        }
    }
    bool ok = false;
    if (m >= 20) {
        double med = ana_median(ag, m, tmp);
        for (int i = 0; i < m; i++) {
            tmp[i] = fabs(ag[i] - med);
        }
        double mad = ana_median(tmp, m, tmp);
        if (mad == 0.0) {
            mad = 1e-9;
        }
        int k = 0;
        for (int i = 0; i < m; i++) {
            if (fabs(ag[i] - med) < 5.0 * 1.4826 * mad) {
                tg[k] = tg[i];
                ag[k] = ag[i];
                k++;
            }
        }
        if (k >= 20 && ana_lomb_psd(tg, ag, k, f, p, ar)) {
            ok = ana_dominant_frequency(f, p, BAND_LO, BAND_HI, hz, ar);
        }
    }
    ar->used = mark;
    return ok;
}

bool ana_breath_peak(const double *t, const double *a, int n, double *hz, void *scratch,
                     size_t scratch_size) {
    ana_arena_t ar = {(uint8_t *)scratch, scratch_size, 0, false, 0};
    return amp_peak(t, a, n, hz, &ar);
}

// Spitzen des RR-Spektrums im Atembereich (mindestens 25 % der stärksten)
static int rr_peaks(const double *t, const double *rr_ms, int n, double *out, int max,
                    ana_arena_t *ar) {
    size_t mark = ar->used;
    double *f = ANA_NEW(ar, double, LOMB_N);
    double *p = ANA_NEW(ar, double, LOMB_N);
    int *pk = ANA_NEW(ar, int, LOMB_N);
    if (ar->failed || !ana_lomb_psd(t, rr_ms, n, f, p, ar)) {
        ar->used = mark;
        return 0;
    }
    int i0 = -1, m = 0;
    for (int i = 0; i < LOMB_N; i++) {
        if (f[i] >= BAND_LO && f[i] <= BAND_HI) {
            if (i0 < 0) {
                i0 = i;
            }
            m++;
        }
    }
    int np = i0 < 0 ? 0 : find_peaks(p + i0, m, pk, LOMB_N);
    double top = 0.0;
    for (int i = 0; i < np; i++) {
        top = fmax(top, p[i0 + pk[i]]);
    }
    int k = 0;
    for (int i = 0; i < np && k < max; i++) {
        if (p[i0 + pk[i]] >= MIN_RR_PEAK * top) {
            out[k++] = f[i0 + pk[i]];
        }
    }
    ar->used = mark;
    return k;
}

// ------------------------------------------------------------------ Kurve und Kennwerte
// Reihe auf das 4-Hz-Raster legen (linear), dann Grundlinie (12 s) abziehen und leicht glätten.
// valid[i] = false, wo keine Stützstellen in der Nähe liegen.
static void to_grid(const double *t, const double *v, int n, double t0, int ng, double *g,
                    bool *valid, double max_gap) {
    int j = 0;
    for (int i = 0; i < ng; i++) {
        double tt = t0 + (double)i / ANA_BREATH_HZ;
        while (j + 1 < n && t[j + 1] < tt) {
            j++;
        }
        valid[i] = false;
        g[i] = 0.0;
        if (j + 1 < n && t[j] <= tt && t[j + 1] >= tt && t[j + 1] - t[j] <= max_gap) {
            double u = (tt - t[j]) / (t[j + 1] - t[j]);
            g[i] = v[j] + u * (v[j + 1] - v[j]);
            valid[i] = true;
        }
    }
}

static void bandpass(double *g, const bool *valid, int ng, double *tmp) {
    const int half = (int)(TREND_S * ANA_BREATH_HZ / 2.0);
    for (int i = 0; i < ng; i++) {
        double s = 0.0;
        int c = 0;
        for (int k = i - half; k <= i + half; k++) {
            if (k >= 0 && k < ng && valid[k]) {
                s += g[k];
                c++;
            }
        }
        tmp[i] = valid[i] && c ? g[i] - s / c : 0.0;
    }
    for (int i = 0; i < ng; i++) {  // Boxcar 3 gegen Rauschen
        double s = 0.0;
        int c = 0;
        for (int k = i - 1; k <= i + 1; k++) {
            if (k >= 0 && k < ng && valid[k]) {
                s += tmp[k];
                c++;
            }
        }
        g[i] = valid[i] && c ? s / c : 0.0;
    }
}

void ana_breath_series(const double *t, const double *v, int n, double t0, int ng, double *g,
                       bool *valid, double *tmp) {
    to_grid(t, v, n, t0, ng, g, valid, 3.0);
    bandpass(g, valid, ng, tmp);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

bool ana_breath(const ana_recording_t *rec, const ana_result_t *res, ana_breath_t *out,
                void *scratch, size_t scratch_size) {
    memset(out, 0, sizeof *out);
    out->cycle_cv = -1.0;
    const int nb = res->n_beats;
    if (!res->has_hrv || nb < MIN_BEATS ||
        (res->r_index[nb - 1] - res->r_index[0]) / ANA_FS < MIN_SPAN_S) {
        out->why = BR_TOO_SHORT;
        return true;
    }
    ana_arena_t ar = {(uint8_t *)scratch, scratch_size, 0, false, 0};
    double *t = ANA_NEW(&ar, double, nb);
    double *amp = ANA_NEW(&ar, double, nb);
    double *tmp = ANA_NEW(&ar, double, nb);
    if (ar.failed) {
        return false;
    }
    const int n = rec->n_blocks * ANA_BLOCK;
    const int half = (int)ana_round(0.06 * ANA_FS);
    for (int i = 0; i < nb; i++) {
        t[i] = res->r_index[i] / ANA_FS;
        int c = (int)ana_round(res->r_index[i]);
        int lo = c - half > 0 ? c - half : 0, hi = c + half + 1 < n ? c + half + 1 : n;
        double mx = -1e9, mn = 1e9;
        bool finite = hi - lo > 3;
        for (int k = lo; k < hi && finite; k++) {
            double v = ana_filtered_mv(rec, k);
            if (isnan(v)) {
                finite = false;
            } else {
                mx = fmax(mx, v);
                mn = fmin(mn, v);
            }
        }
        amp[i] = finite ? mx - mn : NAN;
    }

    // Spitze der Amplitude
    double edr = 0.0;
    out->has_edr = amp_peak(t, amp, nb, &edr, &ar);
    out->edr_hz = out->has_edr ? edr : 0.0;
    if (!out->has_edr) {
        out->why = BR_NO_PEAK;
    }

    // Gegenprobe: Spitze im RR-Spektrum bei derselben Frequenz
    int n_rr = 0;
    double *t_rr = ANA_NEW(&ar, double, nb), *rr_ms = ANA_NEW(&ar, double, nb);
    double *hr_t = ANA_NEW(&ar, double, nb), *hr_v = ANA_NEW(&ar, double, nb);
    if (ar.failed) {
        return false;
    }
    int n_hr = 0;
    for (int i = 0; i + 1 < nb; i++) {
        if (res->rr_ok[i]) {
            double rr = t[i + 1] - t[i];
            t_rr[n_rr] = t[i + 1];
            rr_ms[n_rr] = rr * 1000.0;
            n_rr++;
            hr_t[n_hr] = 0.5 * (t[i] + t[i + 1]);
            hr_v[n_hr] = 60.0 / rr;
            n_hr++;
        }
    }
    if (out->has_edr) {
        if (n_rr < MIN_BEATS) {
            out->why = BR_TOO_SHORT;
        } else {
            double *peaks = ANA_NEW(&ar, double, LOMB_N);
            if (ar.failed) {
                return false;
            }
            int np = rr_peaks(t_rr, rr_ms, n_rr, peaks, LOMB_N, &ar);
            int best = -1;
            for (int i = 0; i < np; i++) {
                if (fabs(peaks[i] - edr) <= MATCH_HZ &&
                    (best < 0 || fabs(peaks[i] - edr) < fabs(peaks[best] - edr))) {
                    best = i;
                }
            }
            const double rate = best >= 0 ? 0.5 * (edr + peaks[best]) : 0.0;
            if (best >= 0 && rate >= RATE_LO && rate <= RATE_HI) {
                out->why = BR_OK;
                out->rate_hz = rate;
            } else {
                out->why = BR_MISMATCH;
            }
        }
    }

    // Atemkurve (aus der Amplitude), Pulsschwankung (aus den RR-Intervallen), Zyklen
    const double t0 = t[0];
    int ng = (int)((t[nb - 1] - t0) * ANA_BREATH_HZ) + 1;
    if (ng > ANA_BREATH_CURVE) {
        ng = ANA_BREATH_CURVE;
    }
    double *g = ANA_NEW(&ar, double, ng), *g2 = ANA_NEW(&ar, double, ng);
    double *w = ANA_NEW(&ar, double, ng);
    bool *valid = ANA_NEW(&ar, bool, ng), *valid2 = ANA_NEW(&ar, bool, ng);
    double *ta = ANA_NEW(&ar, double, nb), *aa = ANA_NEW(&ar, double, nb);
    if (ar.failed) {
        return false;
    }
    int na = 0;
    {
        // Ausreißer der Amplitude wie oben herausnehmen
        double *tm = tmp;
        int m = 0;
        for (int i = 0; i < nb; i++) {
            if (isfinite(amp[i])) {
                aa[m++] = amp[i];
            }
        }
        double med = m ? ana_median(aa, m, tm) : 0.0;
        for (int i = 0; i < m; i++) {
            tm[i] = fabs(aa[i] - med);
        }
        double mad = m ? ana_median(tm, m, tm) : 1e-9;
        if (mad == 0.0) {
            mad = 1e-9;
        }
        for (int i = 0; i < nb; i++) {
            if (isfinite(amp[i]) && fabs(amp[i] - med) < 5.0 * 1.4826 * mad) {
                ta[na] = t[i];
                aa[na] = amp[i];
                na++;
            }
        }
    }
    if (na >= 20) {
        to_grid(ta, aa, na, t0, ng, g, valid, 3.0);
        bandpass(g, valid, ng, w);
        for (int i = 0; i < ng; i++) {
            w[i] = fabs(g[i]);
        }
        qsort(w, (size_t)ng, sizeof *w, cmp_double);
        double scale = w[(int)(0.95 * (ng - 1))];
        if (scale <= 0.0) {
            scale = 1.0;
        }
        for (int i = 0; i < ng; i++) {
            double q = valid[i] ? g[i] / scale * 90.0 : 0.0;
            out->curve[i] = (int8_t)(q > 100.0 ? 100 : q < -100.0 ? -100 : lrint(q));
        }
        out->curve_n = ng;

        // Zyklen: Maxima der Kurve, mindestens 1,5 s Abstand
        double thr = 0.0, peaks_t[ANA_BREATH_CURVE / 4 + 2];
        for (int i = 0; i < ng; i++) {
            thr = fmax(thr, g[i]);
        }
        thr *= 0.25;
        int npk = 0, last = -100;
        double last_v = 0.0;
        for (int i = 1; i + 1 < ng; i++) {
            if (!valid[i] || g[i] < thr || g[i] < g[i - 1] || g[i] <= g[i + 1]) {
                continue;
            }
            if (i - last < 6 && npk > 0) {  // zu dicht: das höhere behalten
                if (g[i] > last_v) {
                    peaks_t[npk - 1] = (double)i / ANA_BREATH_HZ;
                    last = i;
                    last_v = g[i];
                }
            } else if (npk < ANA_BREATH_CURVE / 4) {
                peaks_t[npk++] = (double)i / ANA_BREATH_HZ;
                last = i;
                last_v = g[i];
            }
        }
        out->n_cycles = npk > 0 ? npk - 1 : 0;
        if (npk >= 6) {
            double *d = ANA_NEW(&ar, double, npk), *d2 = ANA_NEW(&ar, double, npk);
            if (ar.failed) {
                return false;
            }
            for (int i = 0; i + 1 < npk; i++) {
                d[i] = peaks_t[i + 1] - peaks_t[i];
            }
            double med = ana_median(d, npk - 1, d2);
            for (int i = 0; i + 1 < npk; i++) {
                d2[i] = fabs(d[i] - med);
            }
            double mad = ana_median(d2, npk - 1, d);
            out->cycle_cv = med > 0.0 ? 1.4826 * mad / med : -1.0;
        }
    }

    // Pulsschwankung (Spitze-Spitze der atemnahen Schwankung der Herzfrequenz)
    if (n_hr >= MIN_BEATS) {
        to_grid(hr_t, hr_v, n_hr, t0, ng, g2, valid2, 3.0);
        bandpass(g2, valid2, ng, w);
        double s = 0.0, s2 = 0.0;
        int c = 0;
        for (int i = 0; i < ng; i++) {
            if (valid2[i]) {
                s += g2[i];
                s2 += g2[i] * g2[i];
                c++;
            }
        }
        if (c >= 60) {
            double mean = s / c;
            out->has_swing = true;
            out->swing_bpm = 2.0 * sqrt(2.0) * sqrt(fmax(s2 / c - mean * mean, 0.0));
        }
    }
    return true;
}
