// Signal-Teil der Auswertung: Ausblendung, 50-Hz-Brumm, R-Zacken, phasenfreie Filterung,
// Median-Schlag mit Vermessung und Bestätigung von Extraschlägen. Wie
// host/src/ekg/dsp/signal_quality.py, filters.py, qrs.py, beats.py und ectopy.py.

#include <math.h>
#include <string.h>

#include "ana_wave.h"
#include "ecg_tables.h"
#include "protocol.h"

#define LSB_V (3.3 / 4096.0 / 64.0)
#define FILT_SCALE 10000.0  // gefiltertes Signal: 0,1 µV je Stufe (±3,27 mV)
#define FILT_NAN INT16_MIN

// ------------------------------------------------------------------ Rohwerte
double ana_raw_mv(const ana_recording_t *rec, int i) {
    uint16_t r = rec->raw4[i];
    if (r == ANA_RAW_GAP) {
        return NAN;
    }
    double v = (double)r * 4.0 * LSB_V;
    return (v - 3.3 / 2) / 1100.0 * 1000.0;
}

static bool lead_off_at(const ana_recording_t *rec, int i) {
    return (rec->flags[i / ANA_BLOCK] & (EKG_FLAG_LO_PLUS | EKG_FLAG_LO_MINUS)) != 0;
}

double ana_filtered_mv(const ana_recording_t *rec, int i) {
    int16_t v = (int16_t)rec->raw4[i];
    return v == FILT_NAN ? NAN : (double)v / FILT_SCALE;
}

static void store_filtered(ana_recording_t *rec, int i, double mv) {
    double q = rint(mv * FILT_SCALE);
    if (isnan(mv)) {
        q = FILT_NAN;
    } else if (q > 32767.0) {
        q = 32767.0;
    } else if (q < -32767.0) {
        q = -32767.0;
    }
    rec->raw4[i] = (uint16_t)(int16_t)q;
}

bool ana_mask_get(const uint8_t *mask, int i) {
    return (mask[i >> 3] >> (i & 7)) & 1u;
}

static void mask_set_range(uint8_t *mask, int a, int b) {
    for (int i = a; i < b; i++) {
        mask[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}

// ------------------------------------------------------------------ Ausblendung
bool ana_invalid_mask(const ana_recording_t *rec, int n, uint8_t *mask, ana_run_t *runs,
                      int max_runs, int *n_runs) {
    memset(mask, 0, (size_t)(n + 7) / 8);
    const double raw_max = (double)ANA_RAW_MAX;
    const double rail = 0.01 * raw_max;
    const int margin = 250;  // max(1, round(0.5 s · fs))
    // Übersteuerung (Anschlag oder CLIP-Flag) samt Rand
    int i = 0;
    while (i < n) {
        bool bad;
        uint16_t r = rec->raw4[i];
        double raw = (double)r * 4.0;
        bool finite = r != ANA_RAW_GAP;
        bad = (finite && (raw <= rail || raw >= raw_max - rail)) ||
              (rec->flags[i / ANA_BLOCK] & EKG_FLAG_CLIP);
        if (!bad) {
            i++;
            continue;
        }
        int s = i;
        while (i < n) {
            r = rec->raw4[i];
            raw = (double)r * 4.0;
            finite = r != ANA_RAW_GAP;
            bad = (finite && (raw <= rail || raw >= raw_max - rail)) ||
                  (rec->flags[i / ANA_BLOCK] & EKG_FLAG_CLIP);
            if (!bad) {
                break;
            }
            i++;
        }
        int a = s - margin < 0 ? 0 : s - margin;
        int b = i + margin > n ? n : i + margin;
        mask_set_range(mask, a, b);
    }
    for (i = 0; i < n; i++) {
        if (lead_off_at(rec, i) || rec->raw4[i] == ANA_RAW_GAP) {
            mask[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }
    // zusammenhängende Abschnitte
    *n_runs = 0;
    i = 0;
    while (i < n) {
        if (!ana_mask_get(mask, i)) {
            i++;
            continue;
        }
        int s = i;
        while (i < n && ana_mask_get(mask, i)) {
            i++;
        }
        if (*n_runs >= max_runs) {
            return false;
        }
        runs[*n_runs].s = s;
        runs[*n_runs].e = i;
        (*n_runs)++;
    }
    return true;
}

// ------------------------------------------------------------------ 50-Hz-Brumm (Welch)
typedef struct {
    const ana_recording_t *rec;
    uint16_t *blocks;  // Blöcke ohne Lead-off und Lücke, in Reihenfolge
    int n_blocks;
} finite_seq_t;

static double seq_at(const finite_seq_t *q, int k) {
    int b = q->blocks[k / ANA_BLOCK];
    return ana_raw_mv(q->rec, b * ANA_BLOCK + k % ANA_BLOCK);
}

static double welch_band(const double *p, double df, int nb, double lo, double hi) {
    // Trapezregel über die Stützstellen im Band (wie numpy.trapezoid über die Maske)
    int i0 = -1, i1 = -1;
    for (int k = 0; k < nb; k++) {
        double f = (double)k * df;
        if (f >= lo && f <= hi) {
            if (i0 < 0) {
                i0 = k;
            }
            i1 = k;
        }
    }
    if (i0 < 0 || i1 == i0) {
        return 0.0;
    }
    double s = 0.0;
    for (int k = i0; k < i1; k++) {
        double f0 = (double)k * df, f1 = (double)(k + 1) * df;
        s += (f1 - f0) * (p[k + 1] + p[k]) / 2.0;
    }
    return s;
}

// Leistung |X[k]|² eines reellen Segments der geraden Länge n über eine FFT halber Länge
static void real_power(const ana_fft_t *fft, const float *x, int n, ana_cpx_t *z, ana_cpx_t *Z,
                       double *acc) {
    const int h = n / 2;
    for (int k = 0; k < h; k++) {
        z[k].r = x[2 * k];
        z[k].i = x[2 * k + 1];
    }
    ana_fft(fft, z, Z);
    for (int k = 0; k <= h; k++) {
        ana_cpx_t a = Z[k % h], b = Z[(h - k) % h];
        double er = 0.5 * ((double)a.r + b.r), ei = 0.5 * ((double)a.i - b.i);
        double or_ = 0.5 * ((double)a.i + b.i), oi = -0.5 * ((double)a.r - b.r);
        double ph = -2.0 * ANA_PI * (double)k / (double)n;
        double c = cos(ph), s = sin(ph);
        double xr = er + (c * or_ - s * oi), xi = ei + (c * oi + s * or_);
        acc[k] += xr * xr + xi * xi;
    }
}

bool ana_mains_level(const ana_recording_t *rec, int n, ana_arena_t *a, double *mains_uv,
                     bool *has_ratio, double *ratio) {
    size_t mark = a->used;
    finite_seq_t q = {rec, NULL, 0};
    int nblk = n / ANA_BLOCK;
    q.blocks = ANA_NEW(a, uint16_t, nblk);
    if (!q.blocks) {
        return false;
    }
    for (int b = 0; b < nblk; b++) {
        int i = b * ANA_BLOCK;
        if (!lead_off_at(rec, i) && rec->raw4[i] != ANA_RAW_GAP) {
            q.blocks[q.n_blocks++] = (uint16_t)b;
        }
    }
    const int len = q.n_blocks * ANA_BLOCK;
    if (len < 4 * ANA_FS) {
        a->used = mark;
        return false;
    }
    const int nper = len < 8 * ANA_FS ? len : 8 * ANA_FS;
    const int nover = nper / 2, step = nper - nover;
    const int nseg = (len - nover) / step;
    const int nb = nper / 2 + 1;
    double *acc = ANA_NEW(a, double, nb);
    float *win = ANA_NEW(a, float, nper);
    float *seg = ANA_NEW(a, float, nper);
    if (a->failed) {
        a->used = mark;
        return false;
    }
    ana_fft_t fft;
    ana_cpx_t *z = NULL, *Z = NULL;
    bool use_fft = nper % 2 == 0;
    if (use_fft) {
        z = ANA_NEW(a, ana_cpx_t, nper / 2);
        Z = ANA_NEW(a, ana_cpx_t, nper / 2);
        use_fft = !a->failed && ana_fft_init(&fft, nper / 2, a);
        if (a->failed) {
            a->used = mark;
            return false;
        }
    }
    // Hann, periodisch (scipy.signal.get_window("hann", nper))
    const double wstep = (ANA_PI - (-ANA_PI)) / (double)nper;
    double wsum = 0.0;
    for (int k = 0; k < nper; k++) {
        double fac = (double)k * wstep + (-ANA_PI);
        double w = 0.5 + 0.5 * cos(fac);
        win[k] = (float)w;
        wsum += w * w;
    }
    memset(acc, 0, sizeof(double) * (size_t)nb);
    const double df = 1.0 / ((double)nper * (1.0 / (double)ANA_FS));
    for (int s = 0; s < nseg; s++) {
        int off = s * step;
        double mean = 0.0;
        for (int k = 0; k < nper; k++) {
            mean += seq_at(&q, off + k);
        }
        mean /= (double)nper;
        for (int k = 0; k < nper; k++) {
            seg[k] = (float)((seq_at(&q, off + k) - mean) * win[k]);
        }
        if (use_fft) {
            real_power(&fft, seg, nper, z, Z, acc);
            continue;
        }
        // seltene Längen: nur die nötigen Stützstellen direkt berechnen
        for (int k = 0; k < nb; k++) {
            double f = (double)k * df;
            if (!((f >= 0.5 && f <= 40.0) || (f >= 49.0 && f <= 51.0))) {
                continue;
            }
            double re = 0, im = 0;
            for (int j = 0; j < nper; j++) {
                double ph = -2.0 * ANA_PI * (double)((long long)k * j % nper) / nper;
                re += seg[j] * cos(ph);
                im += seg[j] * sin(ph);
            }
            acc[k] += re * re + im * im;
        }
    }
    const double scale = 1.0 / ((double)ANA_FS * wsum);
    for (int k = 0; k < nb; k++) {
        double p = acc[k] / (double)nseg * scale;
        bool edge = k == 0 || (nper % 2 == 0 && k == nb - 1);
        acc[k] = edge ? p : 2.0 * p;
    }
    double p50 = welch_band(acc, df, nb, 49.0, 51.0);
    double pecg = welch_band(acc, df, nb, 0.5, 40.0);
    *mains_uv = sqrt(p50) * 1000.0;
    *has_ratio = pecg > 0;
    *ratio = pecg > 0 ? p50 / pecg : 0.0;
    a->used = mark;
    return true;
}

// ------------------------------------------------------------------ Lücken füllen
// Wie fill_gaps(): ausgeblendete Abschnitte linear überbrücken (numpy.interp), davor und
// danach mit dem Randwert. Liest der Reihe nach; der letzte gültige Wert wird gemerkt,
// damit der Puffer hinter dem Lesekopf schon überschrieben werden darf.
typedef struct {
    const ana_recording_t *rec;
    const ana_run_t *runs;
    int n_runs;
    int n;
    int run;  // nächster Abschnitt, der noch nicht hinter uns liegt
    double last_good;
    int last_good_i;
    bool any_good;
} filler_t;

static void filler_init(filler_t *f, const ana_recording_t *rec, const ana_run_t *runs,
                        int n_runs, int n) {
    f->rec = rec;
    f->runs = runs;
    f->n_runs = n_runs;
    f->n = n;
    f->run = 0;
    f->last_good = 0.0;
    f->last_good_i = -1;
    f->any_good = !(n_runs == 1 && runs[0].s == 0 && runs[0].e == n);
}

static double filler_next(filler_t *f, int i) {
    while (f->run < f->n_runs && f->runs[f->run].e <= i) {
        f->run++;
    }
    if (!f->any_good) {
        return 0.0;  // alles ausgeblendet: fill_gaps liefert Nullen
    }
    if (f->run < f->n_runs && f->runs[f->run].s <= i) {
        const ana_run_t *r = &f->runs[f->run];
        if (r->s == 0) {
            return ana_raw_mv(f->rec, r->e);  // vor dem ersten gültigen Wert
        }
        if (r->e >= f->n) {
            return f->last_good;  // nach dem letzten gültigen Wert
        }
        double x0 = (double)(r->s - 1), x1 = (double)r->e;
        double y0 = f->last_good, y1 = ana_raw_mv(f->rec, r->e);
        double slope = (y1 - y0) / (x1 - x0);
        double v = slope * ((double)i - x0) + y0;
        if (isnan(v)) {
            v = slope * ((double)i - x1) + y1;
            if (isnan(v) && y0 == y1) {
                v = y0;
            }
        }
        return v;
    }
    double v = ana_raw_mv(f->rec, i);
    f->last_good = v;
    f->last_good_i = i;
    return v;
}

// beliebiger Index, nur solange der Puffer noch Rohwerte enthält
static double filled_at(const ana_recording_t *rec, const ana_run_t *runs, int n_runs, int n,
                        int i) {
    int lo = 0, hi = n_runs - 1, hit = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (runs[mid].e <= i) {
            lo = mid + 1;
        } else if (runs[mid].s > i) {
            hi = mid - 1;
        } else {
            hit = mid;
            break;
        }
    }
    if (hit < 0) {
        return ana_raw_mv(rec, i);
    }
    const ana_run_t *r = &runs[hit];
    if (r->s == 0 && r->e >= n) {
        return 0.0;
    }
    if (r->s == 0) {
        return ana_raw_mv(rec, r->e);
    }
    if (r->e >= n) {
        return ana_raw_mv(rec, r->s - 1);
    }
    double x0 = (double)(r->s - 1), x1 = (double)r->e;
    double y0 = ana_raw_mv(rec, r->s - 1), y1 = ana_raw_mv(rec, r->e);
    double slope = (y1 - y0) / (x1 - x0);
    double v = slope * ((double)i - x0) + y0;
    if (isnan(v)) {
        v = slope * ((double)i - x1) + y1;
        if (isnan(v) && y0 == y1) {
            v = y0;
        }
    }
    return v;
}

// ------------------------------------------------------------------ R-Zacken
int ana_detect(const ana_recording_t *rec, int n, const ana_run_t *runs, int n_runs,
               qrs_detector_t *det, ana_beat_t *beats, int max_beats, bool *truncated) {
    qrs_init(det);
    filler_t f;
    filler_init(&f, rec, runs, n_runs, n);
    double x[QRS_MAX_BLOCK];
    qrs_beat_t out[16];
    int nb = 0;
    *truncated = false;
    double last = 0.0;
    for (int s = 0; s < n + (int)QRS_MAX_BLOCK; s += QRS_MAX_BLOCK) {
        int m = 0;
        if (s < n) {
            m = n - s < (int)QRS_MAX_BLOCK ? n - s : (int)QRS_MAX_BLOCK;
            for (int k = 0; k < m; k++) {
                x[k] = filler_next(&f, s + k);
            }
            last = x[m - 1];
        } else {
            // am Ende fehlt nur die Vorausschau: mit dem letzten Wert auffüllen
            m = QRS_LOOKAHEAD + 1;
            for (int k = 0; k < m; k++) {
                x[k] = last;
            }
            s = n + (int)QRS_MAX_BLOCK;  // letzter Durchlauf
        }
        size_t k = qrs_process(det, x, (size_t)m, out, 16);
        for (size_t j = 0; j < k; j++) {
            if (out[j].index >= (double)n) {
                continue;
            }
            if (nb >= max_beats) {
                *truncated = true;
                continue;
            }
            beats[nb].index = out[j].index;
            beats[nb].amplitude_mv = out[j].amplitude_mv;
            beats[nb].searchback = out[j].searchback;
            nb++;
        }
    }
    return nb;
}

// ------------------------------------------------------------------ phasenfrei filtern
typedef struct {
    double z[4][2];
} cascade_t;

static void cascade_prime(cascade_t *c, double x0) {
    for (int s = 0; s < 4; s++) {
        c->z[s][0] = ANA_SOS[s][5] * x0;
        c->z[s][1] = ANA_SOS[s][6] * x0;
    }
}

static double cascade_step(cascade_t *c, double x) {
    for (int s = 0; s < 4; s++) {
        const double *k = ANA_SOS[s];
        double y = k[0] * x + c->z[s][0];
        c->z[s][0] = k[1] * x - k[3] * y + c->z[s][1];
        c->z[s][1] = k[2] * x - k[4] * y;
        x = y;
    }
    return x;
}

bool ana_zero_phase(ana_recording_t *rec, int n, const uint8_t *mask, const ana_run_t *runs,
                    int n_runs, ana_arena_t *a) {
    if (n < 16) {
        // zu kurz zum Filtern: unverändert übernehmen (Python: Kopie)
        for (int i = 0; i < n; i++) {
            double v = ana_mask_get(mask, i) ? NAN : filled_at(rec, runs, n_runs, n, i);
            store_filtered(rec, i, v);
        }
        return true;
    }
    size_t mark = a->used;
    const int padlen = n - 1 < 3 * ANA_FS ? n - 1 : 3 * ANA_FS;
    double *left = ANA_NEW(a, double, padlen);
    double *right = ANA_NEW(a, double, padlen);
    if (a->failed) {
        return false;
    }
    // ungerade Fortsetzung: 2·x[0] - x[padlen … 1] und 2·x[-1] - x[-2 … -padlen-1]
    double x0 = filled_at(rec, runs, n_runs, n, 0);
    double xn = filled_at(rec, runs, n_runs, n, n - 1);
    for (int i = 0; i < padlen; i++) {
        left[i] = 2 * x0 - filled_at(rec, runs, n_runs, n, padlen - i);
        right[i] = 2 * xn - filled_at(rec, runs, n_runs, n, n - 2 - i);
    }
    cascade_t c;
    cascade_prime(&c, left[0]);
    for (int i = 0; i < padlen; i++) {
        cascade_step(&c, left[i]);
    }
    filler_t f;
    filler_init(&f, rec, runs, n_runs, n);
    for (int i = 0; i < n; i++) {
        double x = filler_next(&f, i);
        store_filtered(rec, i, cascade_step(&c, x));  // Vorwärts-Ergebnis zwischenspeichern
    }
    for (int i = 0; i < padlen; i++) {
        right[i] = cascade_step(&c, right[i]);  // jetzt Vorwärts-Ergebnis der Fortsetzung
    }
    cascade_prime(&c, right[padlen - 1]);
    for (int i = padlen - 1; i >= 0; i--) {
        cascade_step(&c, right[i]);
    }
    for (int i = n - 1; i >= 0; i--) {
        double y = cascade_step(&c, ana_filtered_mv(rec, i));
        store_filtered(rec, i, ana_mask_get(mask, i) ? NAN : y);
    }
    a->used = mark;
    return true;
}

// ------------------------------------------------------------------ R verfeinern
static int argmax_signed(const double *w, int n, double sign) {
    int k = 0;
    for (int i = 1; i < n; i++) {
        if (sign * w[i] > sign * w[k]) {
            k = i;
        }
    }
    return k;
}

void ana_refine(const ana_recording_t *rec, int n, ana_beat_t *beats, int nb, ana_arena_t *a) {
    const int half = 20;  // round(0.04 s · fs)
    size_t mark = a->used;
    double *w = ANA_NEW(a, double, 2 * half + 1);
    double *b = ANA_NEW(a, double, 4 * half + 1);
    double *tmp = ANA_NEW(a, double, 4 * half + 1);
    if (a->failed) {
        return;
    }
    for (int q = 0; q < nb; q++) {
        int c = (int)rint(beats[q].index);
        int lo = c - half < 0 ? 0 : c - half;
        int hi = c + half + 1 > n ? n : c + half + 1;
        int m = hi - lo;
        bool finite = m >= 3;
        for (int i = 0; i < m && finite; i++) {
            w[i] = ana_filtered_mv(rec, lo + i);
            finite = !isnan(w[i]);
        }
        if (!finite) {
            continue;  // Schlag unverändert übernehmen
        }
        double sign = beats[q].amplitude_mv >= 0 ? 1.0 : -1.0;
        int k = argmax_signed(w, m, sign);
        double frac = 0.0;
        if (k > 0 && k < m - 1) {
            double y0 = sign * w[k - 1], y1 = sign * w[k], y2 = sign * w[k + 1];
            double denom = y0 - 2 * y1 + y2;
            if (denom < 0) {
                frac = 0.5 * (y0 - y2) / denom;
            }
        }
        int blo = c - 2 * half < 0 ? 0 : c - 2 * half;
        int bhi = c + 2 * half + 1 > n ? n : c + 2 * half + 1;
        for (int i = blo; i < bhi; i++) {
            b[i - blo] = ana_filtered_mv(rec, i);
        }
        double base = ana_median(b, bhi - blo, tmp);
        beats[q].index = (double)(lo + k) + frac;
        beats[q].amplitude_mv = w[k] - base;
    }
    a->used = mark;
}

// ------------------------------------------------------------------ Median-Schlag
bool ana_median_beat(const ana_recording_t *rec, int n, const double *r, int nr,
                     double *tmpl, int *n_good, double *mean_rr_s, ana_arena_t *a) {
    if (nr < 5) {
        return false;
    }
    size_t mark = a->used;
    const int pre = ANA_TMPL_PRE, post = ANA_TMPL_POST, len = ANA_TMPL_LEN;
    double *rr = ANA_NEW(a, double, nr - 1);
    bool *ok = ANA_NEW(a, bool, nr - 1);
    int *seg = ANA_NEW(a, int, nr);  // Startindex je Segment
    if (a->failed) {
        return false;
    }
    for (int i = 0; i + 1 < nr; i++) {
        rr[i] = (r[i + 1] - r[i]) / (double)ANA_FS;
    }
    ana_rrclass_t cls;
    if (!ana_nn_mask(rr, nr - 1, NULL, ok, &cls, a)) {
        return false;
    }
    int ns = 0;
    double *s = ANA_NEW(a, double, len);
    if (!s) {
        return false;
    }
    for (int k = 1; k < nr - 1; k++) {
        if (!(ok[k - 1] && ok[k])) {
            continue;
        }
        int c = (int)rint(r[k]);
        if (c - pre < 0 || c + post > n) {
            continue;
        }
        bool finite = true;
        for (int i = 0; i < len && finite; i++) {
            finite = !isnan(ana_filtered_mv(rec, c - pre + i));
        }
        if (finite) {
            seg[ns++] = c - pre;
        }
    }
    if (ns < 3) {
        a->used = mark;
        return false;
    }
    if (ns > 300) {  // numpy.linspace(0, ns - 1, 300).astype(int)
        double st = (double)(ns - 1) / 299.0;
        for (int j = 0; j < 300; j++) {
            int idx = j == 299 ? ns - 1 : (int)((double)j * st + 0.0);
            seg[j] = seg[idx];
        }
        ns = 300;
    }
    double *col = ANA_NEW(a, double, ns);
    double *tmp = ANA_NEW(a, double, ns);
    bool *good = ANA_NEW(a, bool, ns);
    if (a->failed) {
        return false;
    }
    for (int i = 0; i < len; i++) {
        for (int j = 0; j < ns; j++) {
            col[j] = ana_filtered_mv(rec, seg[j] + i);
        }
        tmpl[i] = ana_median(col, ns, tmp);
    }
    // Ausreißer (Artefakte) entfernen und erneut mitteln
    int ng = 0;
    for (int j = 0; j < ns; j++) {
        for (int i = 0; i < len; i++) {
            s[i] = ana_filtered_mv(rec, seg[j] + i);
        }
        double corr = ana_corrcoef(s, tmpl, len);
        good[j] = corr > 0.8;
        ng += good[j];
    }
    if (ng >= 3) {
        for (int i = 0; i < len; i++) {
            int m = 0;
            for (int j = 0; j < ns; j++) {
                if (good[j]) {
                    col[m++] = ana_filtered_mv(rec, seg[j] + i);
                }
            }
            tmpl[i] = ana_median(col, m, tmp);
        }
    }
    int n_ok = 0;
    double *rr_ok = ANA_NEW(a, double, nr - 1);
    double *tmp_rr = ANA_NEW(a, double, nr - 1);
    if (a->failed) {
        return false;
    }
    for (int i = 0; i + 1 < nr; i++) {
        if (ok[i]) {
            rr_ok[n_ok++] = rr[i];
        }
    }
    *mean_rr_s = n_ok ? ana_mean(rr_ok, n_ok) : ana_median(rr, nr - 1, tmp_rr);
    *n_good = ng;
    a->used = mark;
    return true;
}

// ------------------------------------------------------------------ Vermessung
static int ms_to_n(double v) {
    return (int)rint(v * ANA_FS / 1000.0);
}

static double max_range(const double *d, int a, int b) {  // max(d[a:b])
    double m = d[a];
    for (int i = a + 1; i < b; i++) {
        m = d[i] > m ? d[i] : m;
    }
    return m;
}

static void p_wave(const double *x, int len, const double *deriv, int lo, int hi, double sign,
                   int *onset, int *peak, double *amp_out, ana_arena_t *a) {
    *onset = -1;
    *peak = -1;
    *amp_out = NAN;
    if (hi - lo <= ms_to_n(60)) {
        return;
    }
    size_t mark = a->used;
    int m = hi - lo;
    double *rel = ANA_NEW(a, double, m);
    double *smooth = ANA_NEW(a, double, len);
    double *resid = ANA_NEW(a, double, len);
    double *ad = ANA_NEW(a, double, len);
    double *d = ANA_NEW(a, double, len);
    if (a->failed) {
        return;
    }
    // Gerade zwischen den Fenstergrenzen (numpy.linspace)
    double y0 = x[lo], y1 = x[hi - 1];
    double st = m > 1 ? (y1 - y0) / (double)(m - 1) : 0.0;
    for (int i = 0; i < m; i++) {
        double line = (m > 1 && i == m - 1) ? y1 : (double)i * st + y0;
        rel[i] = (x[lo + i] - line) * sign;
    }
    int k = 0;
    for (int i = 1; i < m; i++) {
        if (rel[i] > rel[k]) {
            k = i;
        }
    }
    double amp = rel[k];
    int sz = ms_to_n(20) > 3 ? ms_to_n(20) : 3;
    ana_uniform_filter(x, len, sz, smooth);
    for (int i = 0; i < len; i++) {
        resid[i] = x[i] - smooth[i];
    }
    double noise = sqrt(ana_var(resid, len, 0));
    double lim = 4.0 * noise > 0.03 ? 4.0 * noise : 0.03;
    if (amp < lim || k < ms_to_n(15)) {
        a->used = mark;
        return;
    }
    int pk = lo + k;
    for (int i = 0; i < len; i++) {
        ad[i] = fabs(deriv[i]);
    }
    int s8 = ms_to_n(8) > 1 ? ms_to_n(8) : 1;
    ana_uniform_filter(ad, len, s8, d);
    double thr = 0.2 * max_range(d, lo, pk + 1);
    int w = ms_to_n(16) > 2 ? ms_to_n(16) : 2;
    int j = pk;
    while (j > lo && d[j] < thr) {  // vom Gipfel in den ansteigenden Schenkel
        j--;
    }
    while (j - w > lo && max_range(d, j - w, j + 1) >= thr) {
        j--;
    }
    *peak = pk;
    if (j - w <= lo) {
        *amp_out = amp * sign;
    } else {
        *onset = j;
        *amp_out = x[pk] - x[j];
    }
    a->used = mark;
}

bool ana_delineate(const double *x, int len, int r, double mean_rr_s, ana_delineation_t *o,
                   ana_arena_t *a) {
    size_t mark = a->used;
    double *tmp = ANA_NEW(a, double, len);
    double *d_raw = ANA_NEW(a, double, len);
    double *ad = ANA_NEW(a, double, len);
    double *d = ANA_NEW(a, double, len);
    if (a->failed) {
        return false;
    }
    const double fs = ANA_FS;
    double sign = x[r] >= ana_median(x, len, tmp) ? 1.0 : -1.0;
    ana_gradient(x, len, d_raw);
    for (int i = 0; i < len; i++) {
        d_raw[i] = d_raw[i] * fs;  // mV/s
        ad[i] = fabs(d_raw[i]);
    }
    int s8 = ms_to_n(8) > 1 ? ms_to_n(8) : 1;
    ana_uniform_filter(ad, len, s8, d);
    int q_lo = r - ms_to_n(120) > 0 ? r - ms_to_n(120) : 0;
    int q_hi = r + ms_to_n(140) < len ? r + ms_to_n(140) : len;
    double dmax = max_range(d, q_lo, q_hi);
    double thr = 0.04 * dmax;
    int w = ms_to_n(12) > 2 ? ms_to_n(12) : 2;
    int on = r;
    while (on - w > q_lo && max_range(d, on - w, on + 1) >= thr) {
        on--;
    }
    int off = r;
    while (off + w < q_hi && max_range(d, off, off + w + 1) >= thr) {
        off++;
    }
    double baseline = x[on];

    int t_peak = -1, t_end = -1;
    double t_amp = NAN;
    int t_lo = off + ms_to_n(60);
    int t_hi_c = r + (int)rint(0.62 * mean_rr_s * fs);
    int t_hi = len - 1 < t_hi_c ? len - 1 : t_hi_c;
    if (t_hi - t_lo > ms_to_n(60)) {
        int k = 0;
        double best = -1.0;
        for (int i = t_lo; i < t_hi; i++) {
            double v = fabs(x[i] - baseline);
            if (v > best) {
                best = v;
                k = i - t_lo;
            }
        }
        t_peak = t_lo + k;
        t_amp = x[t_peak] - baseline;
        if (fabs(t_amp) > 0.03) {
            int tl = t_hi - t_peak;
            if (tl > 3) {
                double *tail = ANA_NEW(a, double, tl);
                double *slope = ANA_NEW(a, double, tl);
                if (a->failed) {
                    return false;
                }
                for (int i = 0; i < tl; i++) {
                    tail[i] = x[t_peak + i] - baseline;
                }
                ana_gradient(tail, tl, slope);
                double sg = t_amp > 0 ? 1.0 : -1.0;
                int j = 0;
                for (int i = 1; i < tl; i++) {
                    if (slope[i] * sg < slope[j] * sg) {
                        j = i;
                    }
                }
                double s = slope[j];
                if (fabs(s) > 1e-9) {
                    double cross = (double)j - tail[j] / s;
                    t_end = (int)rint((double)t_peak + cross);
                    double lim = (double)r + 0.6 * mean_rr_s * fs;
                    double cap = (double)(len - 1) < lim ? (double)(len - 1) : lim;
                    if (!(t_peak < t_end && (double)t_end <= cap)) {
                        t_end = -1;
                    }
                }
            }
        } else {
            t_peak = -1;
            t_amp = NAN;
        }
    }

    int rr_n = (int)rint(mean_rr_s * fs);
    int prev_t_end = (t_end >= 0 ? t_end : r + (int)rint(0.6 * rr_n)) - rr_n;
    int p_lo = 0;
    if (on - ms_to_n(300) > p_lo) {
        p_lo = on - ms_to_n(300);
    }
    if (prev_t_end + ms_to_n(10) > p_lo) {
        p_lo = prev_t_end + ms_to_n(10);
    }
    int p_hi = on - ms_to_n(30);
    int p_on, p_pk;
    double p_amp;
    p_wave(x, len, d_raw, p_lo, p_hi, sign, &p_on, &p_pk, &p_amp, a);

    memset(o, 0, sizeof *o);
    o->baseline_mv = baseline;
    o->qrs_onset = on;
    o->qrs_offset = off;
    o->t_peak = t_peak;
    o->t_end = t_end;
    o->p_peak = p_pk;
    o->p_amp_mv = p_amp;
    o->t_amp_mv = t_amp;
    o->r_amp_mv = x[r] - baseline;
    o->qrs_ms = (double)(off - on) / fs * 1000.0;
    o->qt_ms = t_end >= 0 ? (double)(t_end - on) / fs * 1000.0 : NAN;
    o->p_onset = p_on;
    o->pr_ms = p_on >= 0 ? (double)(on - p_on) / fs * 1000.0 : NAN;
    if (!isnan(o->pr_ms) && !(80.0 <= o->pr_ms && o->pr_ms <= 300.0)) {
        o->p_onset = -1;  // unplausibel: lieber kein Wert als ein falscher
        o->pr_ms = NAN;
    }
    bool qt = !isnan(o->qt_ms) && o->qt_ms != 0.0;
    o->qtc_bazett_ms = qt ? o->qt_ms / sqrt(mean_rr_s) : NAN;
    o->qtc_fridericia_ms = qt ? o->qt_ms / pow(mean_rr_s, 1.0 / 3.0) : NAN;
    a->used = mark;
    return true;
}

// ------------------------------------------------------------------ Extraschläge
void ana_confirm_ectopics(const ana_recording_t *rec, int n, const double *r, int nr,
                          const int *cand, int n_cand, const double *tmpl, bool has_tmpl,
                          ana_ectopy_t *out, ana_arena_t *a) {
    memset(out, 0, sizeof *out);
    size_t mark = a->used;
    double *rr = ANA_NEW(a, double, nr > 1 ? nr - 1 : 1);
    int *seen = ANA_NEW(a, int, n_cand + 1);
    double *nb = ANA_NEW(a, double, 32);
    double *tmp = ANA_NEW(a, double, 32);
    double *w = ANA_NEW(a, double, 64);
    if (a->failed) {
        return;
    }
    for (int i = 0; i + 1 < nr; i++) {
        rr[i] = (r[i + 1] - r[i]) / (double)ANA_FS;
    }
    // QRS-Fenster der Vorlage: R - 50 ms … R + 70 ms
    const int t_a = ANA_TMPL_PRE - 25, t_b = ANA_TMPL_PRE + 35;
    const double *t = has_tmpl ? tmpl + t_a : NULL;
    const int tl = t_b - t_a;
    int n_seen = 0;
    for (int q = 0; q < n_cand; q++) {
        int i = cand[q];
        int beats[2], nbt = 0;
        for (int k = i; k <= i + 1; k++) {
            if (k >= 1 && k < nr) {
                beats[nbt++] = k;
            }
        }
        if (!nbt) {
            continue;
        }
        int k = beats[0];
        if (nbt == 2 && rr[beats[1] - 1] < rr[beats[0] - 1]) {
            k = beats[1];
        }
        bool dup = false;
        for (int s = 0; s < n_seen; s++) {
            dup |= seen[s] == k;
        }
        if (dup) {
            continue;
        }
        seen[n_seen++] = k;
        int lo = k - 11 > 0 ? k - 11 : 0;
        int hi = k + 10 < nr - 1 ? k + 10 : nr - 1;
        int m = 0;
        for (int j = lo; j < hi; j++) {
            if (j == k - 1 || j == k) {
                continue;
            }
            nb[m++] = rr[j];
        }
        if (m < 3 || rr[k - 1] >= 0.85 * ana_median(nb, m, tmp)) {
            out->not_premature++;
            continue;
        }
        double center = r[k];
        int wa = (int)rint(center - 0.05 * ANA_FS), wb = (int)rint(center + 0.07 * ANA_FS);
        bool have_w = wa >= 0 && wb <= n && wb - wa == tl;
        for (int j = 0; have_w && j < tl; j++) {
            w[j] = ana_filtered_mv(rec, wa + j);
            have_w = !isnan(w[j]);
        }
        if (!have_w || !t) {
            out->normal_qrs++;
            continue;
        }
        double corr = ana_corrcoef(w, t, tl);
        if (corr >= 0.9) {
            out->normal_qrs++;
        } else {
            out->other_qrs++;
        }
    }
    a->used = mark;
}
