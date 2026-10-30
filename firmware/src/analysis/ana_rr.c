// RR-Reihe: Artefakt-Klassifikation, HRV, Unregelmäßigkeit und Rhythmus-Hinweise.
// Zeile für Zeile wie host/src/ekg/dsp/artifacts.py und hrv.py.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ana_int.h"

#define C1 0.13
#define C2 0.17
#define ALPHA 5.2
#define WINDOW 91
#define MEDFILT 11

#define LF_LO 0.04
#define LF_HI 0.15
#define HF_LO 0.15
#define HF_HI 0.4
#define PEAK_LO 0.08
#define PEAK_HI 0.5

static double fmax2(double a, double b) {
    return a > b ? a : b;
}

static double fmin2(double a, double b) {
    return a < b ? a : b;
}

// ------------------------------------------------------------------ Klassifikation
static void threshold(const double *x, int n, double *th, ana_arena_t *a) {
    double *abs_x = ANA_NEW(a, double, n);
    double *q1 = ANA_NEW(a, double, n);
    double *q3 = ANA_NEW(a, double, n);
    double *tmp = ANA_NEW(a, double, WINDOW);
    if (a->failed) {
        return;
    }
    for (int i = 0; i < n; i++) {
        abs_x[i] = fabs(x[i]);
    }
    int size = n % 2 ? n : n - 1;
    if (size > WINDOW) {
        size = WINDOW;
    }
    if (size < 1) {
        size = 1;
    }
    ana_percentile_filter(abs_x, n, 25.0, size, q1, tmp);
    ana_percentile_filter(abs_x, n, 75.0, size, q3, tmp);
    for (int i = 0; i < n; i++) {
        double t = ALPHA * (q3[i] - q1[i]) / 2.0;
        th[i] = fmax2(t, 0.01);  // Untergrenze wie im Python-Code
    }
}

static bool contains(const int *v, int n, int x) {
    for (int i = 0; i < n; i++) {
        if (v[i] == x) {
            return true;
        }
    }
    return false;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int sort_unique(int *v, int n) {
    qsort(v, (size_t)n, sizeof(int), cmp_int);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k == 0 || v[k - 1] != v[i]) {
            v[k++] = v[i];
        }
    }
    return k;
}

// Ergänzt vorzeitige Schläge mit Pause, die das adaptive Verfahren übersieht
static void compensatory_pauses(const double *rr, int n, const double *medrr, ana_rrclass_t *c,
                                ana_arena_t *a) {
    int *known = ANA_NEW(a, int, 3 * n + 4);
    if (!known) {
        return;
    }
    int n_known = 0;
    for (int i = 0; i < c->n_ectopic; i++) {
        known[n_known++] = c->ectopic[i];
    }
    for (int k = 1; k < n - 1; k++) {
        double m = medrr[k];
        double s = rr[k], l = rr[k + 1];
        if (s < 0.85 * m && l > 1.1 * m && fabs(s + l - 2 * m) < 0.2 * m && rr[k - 1] > 1.1 * s &&
            !contains(known, n_known, k) && !contains(known, n_known, k + 1)) {
            c->ectopic[c->n_ectopic++] = k + 1;  // Index des langen Intervalls
            known[n_known++] = k;
            known[n_known++] = k + 1;
        }
    }
}

bool ana_classify_rr(const double *rr_in, int n, ana_rrclass_t *c, ana_arena_t *a) {
    memset(c, 0, sizeof *c);
    c->n = n;
    // Jede Liste kann höchstens 2 Einträge je Intervall bekommen (Kandidaten i und i + 1)
    c->ectopic = ANA_NEW(a, int, 2 * n + 4);
    c->missed = ANA_NEW(a, int, 2 * n + 4);
    c->extra = ANA_NEW(a, int, 2 * n + 4);
    c->longshort = ANA_NEW(a, int, 2 * n + 4);
    if (a->failed) {
        return false;
    }
    if (n < 6) {
        return true;
    }
    size_t mark = a->used;
    double *rr = ANA_NEW(a, double, n);
    double *drrs = ANA_NEW(a, double, n);
    double *th1 = ANA_NEW(a, double, n);
    double *pad = ANA_NEW(a, double, n + 4);
    double *s12 = ANA_NEW(a, double, n);
    double *s22 = ANA_NEW(a, double, n);
    double *medrr = ANA_NEW(a, double, n);
    double *mrrs = ANA_NEW(a, double, n);
    double *th2 = ANA_NEW(a, double, n);
    double *tmp = ANA_NEW(a, double, MEDFILT);
    if (a->failed) {
        return false;
    }
    memcpy(rr, rr_in, sizeof(double) * (size_t)n);

    drrs[0] = 0.0;
    for (int i = 1; i < n; i++) {
        drrs[i] = rr[i] - rr[i - 1];
    }
    drrs[0] = ana_mean(drrs + 1, n - 1);
    threshold(drrs, n, th1, a);
    if (a->failed) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        drrs[i] = drrs[i] / th1[i];
    }
    // numpy.pad(..., 2, mode="reflect")
    pad[0] = drrs[2];
    pad[1] = drrs[1];
    for (int i = 0; i < n; i++) {
        pad[i + 2] = drrs[i];
    }
    pad[n + 2] = drrs[n - 2];
    pad[n + 3] = drrs[n - 3];
    for (int j = 0; j < n; j++) {
        double d = pad[j + 2];
        double prev = pad[j + 1], next = pad[j + 3], next2 = pad[j + 4];
        s12[j] = 0.0;
        if (d > 0) {
            s12[j] = fmax2(prev, next);
        } else if (d < 0) {
            s12[j] = fmin2(prev, next);
        }
        s22[j] = d >= 0 ? fmin2(next, next2) : fmax2(next, next2);
    }

    ana_median_filter(rr, n, MEDFILT, medrr, tmp);
    for (int i = 0; i < n; i++) {
        mrrs[i] = rr[i] - medrr[i];
        if (mrrs[i] < 0) {
            mrrs[i] *= 2.0;  // kurze Intervalle stärker gewichten
        }
    }
    threshold(mrrs, n, th2, a);
    if (a->failed) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        mrrs[i] = mrrs[i] / th2[i];
    }

    int i = 0;
    while (i < n - 2) {
        if (fabs(drrs[i]) <= 1) {
            i++;
            continue;
        }
        bool eq1 = drrs[i] > 1 && s12[i] < -C1 * drrs[i] - C2;
        bool eq2 = drrs[i] < -1 && s12[i] > -C1 * drrs[i] + C2;
        if (eq1 || eq2) {
            c->ectopic[c->n_ectopic++] = i;
            i++;
            continue;
        }
        int cand[2] = {i, i + 1};
        int n_cand = fabs(drrs[i + 1]) < fabs(drrs[i + 2]) ? 2 : 1;
        for (int q = 0; q < n_cand; q++) {
            int j = cand[q];
            bool lng = drrs[j] > 1 && s22[j] < -1;
            bool longshort = fabs(mrrs[j]) > 3;
            bool shrt = drrs[j] < -1 && s22[j] > 1;
            if (!(lng || longshort || shrt)) {
                continue;
            }
            bool missed = fabs(rr[j] / 2 - medrr[j]) < th2[j] && rr[j] > 1.5 * medrr[j];
            bool extra = j + 1 < n && fabs(rr[j] + rr[j + 1] - medrr[j]) < th2[j] &&
                         rr[j] < 0.75 * medrr[j];
            if (shrt && extra) {
                c->extra[c->n_extra++] = j;
            } else if (lng && missed) {
                c->missed[c->n_missed++] = j;
            } else {
                c->longshort[c->n_longshort++] = j;
            }
        }
        i++;
    }
    compensatory_pauses(rr, n, medrr, c, a);
    c->n_ectopic = sort_unique(c->ectopic, c->n_ectopic);
    c->n_missed = sort_unique(c->missed, c->n_missed);
    c->n_extra = sort_unique(c->extra, c->n_extra);
    // „longshort“ direkt neben einem eindeutig zugeordneten Ereignis zählt nicht extra
    int k = 0;
    int nl = sort_unique(c->longshort, c->n_longshort);
    for (int q = 0; q < nl; q++) {
        int v = c->longshort[q];
        bool taken = false;
        const int *lists[3] = {c->ectopic, c->missed, c->extra};
        const int counts[3] = {c->n_ectopic, c->n_missed, c->n_extra};
        for (int L = 0; L < 3 && !taken; L++) {
            for (int e = 0; e < counts[L]; e++) {
                int d = v - lists[L][e];
                if (d >= -1 && d <= 1) {
                    taken = true;
                    break;
                }
            }
        }
        if (!taken) {
            c->longshort[k++] = v;
        }
    }
    c->n_longshort = k;
    a->used = mark;  // Zwischenfelder freigeben (Listen liegen davor)
    return !a->failed;
}

void ana_valid_mask(const ana_rrclass_t *c, bool *ok) {
    for (int i = 0; i < c->n; i++) {
        ok[i] = true;
    }
#define DROP(idx)                     \
    do {                              \
        int _i = (idx);               \
        if (_i >= 0 && _i < c->n) {   \
            ok[_i] = false;           \
        }                             \
    } while (0)
    for (int q = 0; q < c->n_ectopic; q++) {
        DROP(c->ectopic[q] - 1);
        DROP(c->ectopic[q]);
        DROP(c->ectopic[q] + 1);
    }
    for (int q = 0; q < c->n_missed; q++) {
        DROP(c->missed[q]);
    }
    for (int q = 0; q < c->n_extra; q++) {
        DROP(c->extra[q]);
        DROP(c->extra[q] + 1);
    }
    for (int q = 0; q < c->n_longshort; q++) {
        DROP(c->longshort[q]);
    }
#undef DROP
}

bool ana_nn_mask(const double *rr, int n, const bool *bad, bool *ok, ana_rrclass_t *cls,
                 ana_arena_t *a) {
    bool *hard = ANA_NEW(a, bool, n);
    double *filled = ANA_NEW(a, double, n);
    double *tmp = ANA_NEW(a, double, n);
    if (a->failed) {
        return false;
    }
    int n_hard = 0;
    for (int i = 0; i < n; i++) {
        hard[i] = rr[i] >= ANA_RR_MIN_S && rr[i] <= ANA_RR_MAX_S && !(bad && bad[i]);
        if (hard[i]) {
            tmp[n_hard++] = rr[i];
        }
        filled[i] = rr[i];
    }
    if (n_hard) {
        double med = ana_median(tmp, n_hard, tmp);
        for (int i = 0; i < n; i++) {
            if (!hard[i]) {
                filled[i] = med;  // damit Lücken die Schwellen nicht verzerren
            }
        }
    }
    if (!ana_classify_rr(filled, n, cls, a)) {
        return false;
    }
    ana_valid_mask(cls, ok);
    for (int i = 0; i < n; i++) {
        ok[i] = ok[i] && hard[i];
    }
    return true;
}

void ana_rr_overlap_mask(const double *t, int n_t, ana_invalid_t inv, bool *bad) {
    for (int i = 0; i + 1 < n_t; i++) {
        bad[i] = false;
        for (int k = 0; k < inv.n; k++) {
            if (t[i] < inv.iv[k].e && t[i + 1] > inv.iv[k].s) {
                bad[i] = true;
                break;
            }
        }
    }
}

// ------------------------------------------------------------------ HRV

void ana_lomb_freqs(double *f) {
    const double start = 0.001, delta = (start + 0.001) - start;
    for (int i = 0; i < LOMB_N; i++) {
        f[i] = start + (double)i * delta;
    }
}

// Einseitige Leistungsdichte (ms²/Hz), Integral = Varianz
bool ana_lomb_psd(const double *t, const double *y_in, int n, double *freqs, double *psd,
                     ana_arena_t *a) {
    double *y = ANA_NEW(a, double, n);
    if (!y) {
        return false;
    }
    ana_detrend_linear(y_in, n, y);
    ana_lomb_freqs(freqs);
    ana_lombscargle(t, y, n, freqs, LOMB_N, psd);
    double area = ana_trapezoid(psd, freqs, LOMB_N);
    double var = ana_var(y, n, 0);
    if (area > 0) {
        double f = var / area;
        for (int i = 0; i < LOMB_N; i++) {
            psd[i] = psd[i] * f;
        }
    }
    return true;
}

static double band_power(const double *f, const double *p, double lo, double hi) {
    int i0 = -1, i1 = -1;
    for (int i = 0; i < LOMB_N; i++) {
        if (f[i] >= lo && f[i] < hi) {
            if (i0 < 0) {
                i0 = i;
            }
            i1 = i;
        }
    }
    int m = i0 < 0 ? 0 : i1 - i0 + 1;
    return m > 1 ? ana_trapezoid(p + i0, f + i0, m) : 0.0;
}

bool ana_dominant_frequency(const double *f, const double *p, double lo, double hi,
                               double *out, ana_arena_t *a) {
    int i0 = -1, m = 0;
    for (int i = 0; i < LOMB_N; i++) {
        if (f[i] >= lo && f[i] <= hi) {
            if (i0 < 0) {
                i0 = i;
            }
            m++;
        }
    }
    if (m < 5) {
        return false;
    }
    int k = 0;
    for (int i = 1; i < m; i++) {
        if (p[i0 + i] > p[i0 + k]) {
            k = i;
        }
    }
    double *tmp = ANA_NEW(a, double, m);
    if (!tmp) {
        return false;
    }
    double med = ana_median(p + i0, m, tmp);
    if (p[i0 + k] < 3.0 * med) {
        return false;
    }
    *out = f[i0 + k];
    return true;
}

bool ana_compute_hrv(const double *t, int n_t, ana_invalid_t inv, ana_hrv_t *out,
                     uint8_t *ok_out, ana_arena_t *a) {
    if (n_t < 10) {
        return false;
    }
    size_t mark = a->used;
    int n = n_t - 1;
    double *rr_s = ANA_NEW(a, double, n);
    bool *bad = ANA_NEW(a, bool, n);
    bool *ok = ANA_NEW(a, bool, n);
    double *rr = ANA_NEW(a, double, n);
    double *rr_ok = ANA_NEW(a, double, n);
    double *t_ok = ANA_NEW(a, double, n);
    double *diffs = ANA_NEW(a, double, n);
    double *hr = ANA_NEW(a, double, n);
    if (a->failed) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        rr_s[i] = t[i + 1] - t[i];
    }
    ana_rr_overlap_mask(t, n_t, inv, bad);
    ana_rrclass_t cls;
    if (!ana_nn_mask(rr_s, n, bad, ok, &cls, a)) {
        return false;
    }
    int n_ok = 0, n_excl = 0;
    for (int i = 0; i < n; i++) {
        rr[i] = rr_s[i] * 1000.0;
        if (ok_out) {
            ok_out[i] = ok[i] ? 1 : 0;
        }
    }
    for (int i = 0; i < n; i++) {
        if (ok[i]) {
            rr_ok[n_ok] = rr[i];
            t_ok[n_ok] = t[i + 1];
            n_ok++;
        } else {
            n_excl++;
        }
    }
    if (n_ok < 8) {
        a->used = mark;
        return false;
    }
    int n_d = 0;
    for (int i = 0; i + 1 < n; i++) {
        if (ok[i + 1] && ok[i]) {  // nur Paare aus zwei gültigen, direkt folgenden RR
            diffs[n_d++] = rr[i + 1] - rr[i];
        }
    }
    for (int i = 0; i < n_ok; i++) {
        hr[i] = 60000.0 / rr_ok[i];
    }
    memset(out, 0, sizeof *out);
    out->n_beats = n_t;
    out->n_excluded = n_excl;
    out->n_ectopic = cls.n_ectopic;
    out->n_missed = cls.n_missed;
    out->n_extra = cls.n_extra;
    out->duration_s = t[n_t - 1] - t[0];
    out->mean_rr_ms = ana_mean(rr_ok, n_ok);
    out->mean_hr = ana_mean(hr, n_ok);
    double mn = hr[0], mx = hr[0];
    for (int i = 1; i < n_ok; i++) {
        mn = hr[i] < mn ? hr[i] : mn;
        mx = hr[i] > mx ? hr[i] : mx;
    }
    out->min_hr = mn;
    out->max_hr = mx;
    double sd1 = n_d > 1 ? sqrt(0.5 * ana_var(diffs, n_d, 1)) : NAN;
    double sdnn = sqrt(ana_var(rr_ok, n_ok, 1));
    out->sd1_ms = sd1;
    out->sdnn_ms = sdnn;
    out->sd2_ms = isfinite(sd1) ? sqrt(fmax2(0.0, 2 * sdnn * sdnn - sd1 * sd1)) : NAN;
    if (n_d) {
        double sq = 0;
        double *d2 = ANA_NEW(a, double, n_d);
        if (!d2) {
            return false;
        }
        int over = 0;
        for (int i = 0; i < n_d; i++) {
            d2[i] = diffs[i] * diffs[i];
            over += fabs(diffs[i]) > 50;
        }
        sq = ana_mean(d2, n_d);
        out->rmssd_ms = sqrt(sq);
        out->pnn50 = 100.0 * ((double)over / (double)n_d);
    } else {
        out->rmssd_ms = NAN;
        out->pnn50 = NAN;
    }
    if (out->duration_s >= 60) {
        double *f = ANA_NEW(a, double, LOMB_N);
        double *p = ANA_NEW(a, double, LOMB_N);
        if (!f || !p || !ana_lomb_psd(t_ok, rr_ok, n_ok, f, p, a)) {
            return false;
        }
        double lf = band_power(f, p, LF_LO, LF_HI);
        double hf = band_power(f, p, HF_LO, HF_HI);
        out->has_lf = true;
        out->lf_ms2 = lf;
        out->hf_ms2 = hf;
        if (hf > 0) {
            out->has_lf_hf = true;
            out->lf_hf = lf / hf;
        }
        if (lf + hf > 0) {
            out->has_nu = true;
            out->lf_nu = 100 * lf / (lf + hf);
            out->hf_nu = 100 * hf / (lf + hf);
        }
        out->has_peak = ana_dominant_frequency(f, p, PEAK_LO, PEAK_HI, &out->peak_hz, a);
    }
    a->used = mark;
    return !a->failed;
}

// ------------------------------------------------------------------ Unregelmäßigkeit
static double turning_point_ratio(const double *x, int n) {
    if (n < 3) {
        return NAN;
    }
    int tp = 0;
    for (int i = 1; i + 1 < n; i++) {
        double l = x[i - 1], m = x[i], r = x[i + 1];
        tp += (m > l && m > r) || (m < l && m < r);
    }
    return (double)tp / (double)(n - 2);
}

bool ana_irregularity(const double *rr, int n, const bool *bad, ana_irregularity_t *out,
                      ana_arena_t *a) {
    const int window = 64;
    size_t mark = a->used;
    bool *keep = ANA_NEW(a, bool, n);
    double *x = ANA_NEW(a, double, n);
    double *d = ANA_NEW(a, double, window);
    double *d2 = ANA_NEW(a, double, window);
    if (a->failed) {
        return false;
    }
    ana_rrclass_t cls;
    if (!ana_nn_mask(rr, n, bad, keep, &cls, a)) {
        return false;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (keep[i]) {
            x[m++] = rr[i];
        }
    }
    if (m < window) {
        a->used = mark;
        return false;
    }
    int n_win = 0, n_irr = 0;
    int max_win = (m - window) / (window / 2) + 1;
    double *s0 = ANA_NEW(a, double, max_win);
    double *s1 = ANA_NEW(a, double, max_win);
    double *s2 = ANA_NEW(a, double, max_win);
    double *tmp = ANA_NEW(a, double, max_win);
    if (a->failed) {
        return false;
    }
    for (int s = 0; s <= m - window; s += window / 2) {
        const double *w = x + s;
        for (int i = 0; i + 1 < window; i++) {
            d[i] = w[i + 1] - w[i];
            d2[i] = d[i] * d[i];
        }
        double nrmssd = sqrt(ana_mean(d2, window - 1)) / ana_mean(w, window);
        double sd1 = sqrt(0.5 * ana_var(d, window - 1, 1));
        double sd2 = sqrt(fmax2(1e-12, 2 * ana_var(w, window, 1) - sd1 * sd1));
        double tpr = turning_point_ratio(w, window);
        s0[n_win] = nrmssd;
        s1[n_win] = sd1 / sd2;
        s2[n_win] = tpr;
        n_win++;
        if (nrmssd > 0.08 && sd1 / sd2 > 0.7 && 0.55 <= tpr && tpr <= 0.8) {
            n_irr++;
        }
    }
    out->windows = n_win;
    out->irregular_windows = n_irr;
    out->nrmssd = ana_median(s0, n_win, tmp);
    out->sd1_sd2 = ana_median(s1, n_win, tmp);
    out->tpr = ana_median(s2, n_win, tmp);
    a->used = mark;
    return true;
}

// ------------------------------------------------------------------ Hinweise
static void add_hint(char hints[][ANA_TEXT], int *n, int max, const char *text) {
    if (*n < max) {
        snprintf(hints[*n], ANA_TEXT, "%s", text);
        (*n)++;
    }
}

int ana_rhythm_hints(const double *t, int n_t, ana_invalid_t inv, const ana_ectopy_t *ectopy,
                     char hints[][ANA_TEXT], int max_hints, ana_arena_t *a, ana_rhythm_t *rh) {
    if (rh) {
        memset(rh, 0, sizeof *rh);
    }
    if (n_t < 20) {
        return 0;
    }
    size_t mark = a->used;
    int n = n_t - 1;
    double *rr = ANA_NEW(a, double, n);
    bool *bad = ANA_NEW(a, bool, n);
    bool *ok = ANA_NEW(a, bool, n);
    double *tmp = ANA_NEW(a, double, n);
    if (a->failed) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        rr[i] = t[i + 1] - t[i];
    }
    ana_rr_overlap_mask(t, n_t, inv, bad);
    int n_hard = 0;
    for (int i = 0; i < n; i++) {
        if (rr[i] >= ANA_RR_MIN_S && rr[i] <= ANA_RR_MAX_S && !bad[i]) {
            tmp[n_hard++] = rr[i];
        }
    }
    if (n_hard < 15) {
        a->used = mark;
        return 0;
    }
    int nh = 0;
    char buf[ANA_TEXT];
    double hr = 60.0 / ana_median(tmp, n_hard, tmp);
    if (rh) {
        rh->valid = true;
        rh->median_hr = hr;
    }
    if (hr < 50) {
        snprintf(buf, sizeof buf, "Herzfrequenz niedrig (%ld/min)", (long)ana_round(hr));
        add_hint(hints, &nh, max_hints, buf);
    } else if (hr > 100) {
        snprintf(buf, sizeof buf, "Herzfrequenz hoch (%ld/min)", (long)ana_round(hr));
        add_hint(hints, &nh, max_hints, buf);
    }

    ana_rrclass_t cls;
    if (!ana_nn_mask(rr, n, bad, ok, &cls, a)) {
        return nh;
    }
    ana_irregularity_t irr;
    bool has_irr = ana_irregularity(rr, n, bad, &irr, a);
    double fraction = has_irr && irr.windows ? (double)irr.irregular_windows / irr.windows : 0.0;
    if (rh && has_irr) {
        rh->has_irr = true;
        rh->windows = irr.windows;
        rh->irregular_windows = irr.irregular_windows;
    }
    if (has_irr && fraction >= 0.5) {
        add_hint(hints, &nh, max_hints,
                 "Rhythmus über weite Strecken zufällig unregelmäßig – bei wiederholtem Befund "
                 "ärztlich abklären");
    } else if (has_irr && irr.irregular_windows) {
        add_hint(hints, &nh, max_hints, "Rhythmus zeitweise zufällig unregelmäßig");
    }
    bool mostly_irregular = has_irr && fraction >= 0.5;
    int n_ect = ectopy ? ectopy->normal_qrs + ectopy->other_qrs : cls.n_ectopic;
    if (rh) {
        rh->n_ect = n_ect;
    }
    if (!mostly_irregular && n_ect >= 3 && (n_ect >= 10 || n_ect >= 0.01 * n)) {
        int len = snprintf(buf, sizeof buf, "%d vorzeitige Schläge mit nachfolgender Pause", n_ect);
        if (ectopy && ectopy->other_qrs && len > 0 && len < (int)sizeof buf) {
            snprintf(buf + len, sizeof buf - (size_t)len, ", davon %d mit veränderter QRS-Form",
                     ectopy->other_qrs);
        }
        add_hint(hints, &nh, max_hints, buf);
    }
    int n_err = cls.n_missed + cls.n_extra;
    if (rh) {
        rh->n_err = n_err;
    }
    if (n_err >= 3) {
        snprintf(buf, sizeof buf,
                 "%d vermutete Erkennungsfehler (verpasste/zusätzliche Schläge) – Signal prüfen",
                 n_err);
        add_hint(hints, &nh, max_hints, buf);
    }
    int n_ok = 0;
    for (int i = 0; i < n; i++) {
        n_ok += ok[i];
    }
    if (has_irr && fraction < 0.5 && n_ok > 30) {
        double *nn = ANA_NEW(a, double, n);
        double *d2 = ANA_NEW(a, double, n);
        if (nn && d2) {
            int m = 0, nd = 0;
            for (int i = 0; i < n; i++) {
                if (ok[i]) {
                    nn[m++] = rr[i];
                }
            }
            for (int i = 0; i + 1 < n; i++) {
                if (ok[i + 1] && ok[i]) {
                    double d = rr[i + 1] - rr[i];
                    d2[nd++] = d * d;
                }
            }
            double nrmssd = nd > 10 ? sqrt(ana_mean(d2, nd)) / ana_mean(nn, m) : 0.0;
            if (nrmssd > 0.05 && irr.sd1_sd2 < 0.6) {
                add_hint(hints, &nh, max_hints,
                         "Herzfrequenz schwankt stark, aber gleichmäßig (Atmung und "
                         "Kreislaufregulation – in Ruhe normal)");
                if (rh) {
                    rh->resp_variation = true;
                }
            }
        }
    }
    a->used = mark;
    return nh;
}
