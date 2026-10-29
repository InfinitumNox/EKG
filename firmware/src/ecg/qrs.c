#include "qrs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HMASK (QRS_HIST_CAP - 1u)
#define H(buf, i) ((buf)[(uint64_t)(i) & HMASK])

static void reset_state(qrs_detector_t *d) {
    memset(&d->bp, 0, sizeof(d->bp));
    memset(&d->ref, 0, sizeof(d->ref));
    d->primed = false;
    memset(d->dx, 0, sizeof(d->dx));
    memset(d->sq_win, 0, sizeof(d->sq_win));
    d->sq_pos = 0;
    d->sq_sum = 0.0;
    d->initialized = false;
    d->scan_from = 0;
    d->spki = 0.0;
    d->npki = 0.0;
    d->has_last_qrs = false;
    d->last_qrs = 0;
    d->last_slope = 0.0;
    d->n_recent = 0;
    d->n_selected = 0;
    d->n_cands = 0;
    d->polarity = 0;
    d->last_stall_decay = 0;
    d->init_index = 0;
}

void qrs_init(qrs_detector_t *d) {
    reset_state(d);
    d->has_origin = false;
    d->origin = d->next = d->hist_start = 0;
}

void qrs_reset(qrs_detector_t *d, int64_t start_index) {
    reset_state(d);
    d->has_origin = true;
    d->origin = d->next = d->hist_start = start_index;
}

bool qrs_initialized(const qrs_detector_t *d) {
    return d->initialized;
}

int64_t qrs_next_index(const qrs_detector_t *d) {
    return d->next;
}

// Kausaler SOS-Filter wie scipy.signal.sosfilt (direkte Form II transponiert).
static double sos_step(const double sos[2][7], qrs_sos_state_t *s, double x) {
    for (int k = 0; k < 2; k++) {
        const double *c = sos[k];
        double y = c[0] * x + s->z[k][0];
        s->z[k][0] = c[1] * x - c[3] * y + s->z[k][1];
        s->z[k][1] = c[2] * x - c[4] * y;
        x = y;
    }
    return x;
}

static void sos_prime(const double sos[2][7], qrs_sos_state_t *s, double x0) {
    for (int k = 0; k < 2; k++) {
        s->z[k][0] = sos[k][5] * x0;
        s->z[k][1] = sos[k][6] * x0;
    }
}

static double mean_int(const int *v, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += v[i];
    }
    return (double)sum / (double)n;
}

static bool rr_average(const qrs_detector_t *d, double *out) {
    if (d->n_selected) {
        *out = mean_int(d->rr_selected, d->n_selected);
        return true;
    }
    if (d->n_recent) {
        *out = mean_int(d->rr_recent, d->n_recent);
        return true;
    }
    return false;
}

static double thr1(const qrs_detector_t *d) {
    double t = d->npki + 0.25 * (d->spki - d->npki);
    return t > QRS_FLOOR ? t : QRS_FLOOR;
}

static double thr2(const qrs_detector_t *d) {
    double t = 0.5 * thr1(d);
    return t > QRS_FLOOR ? t : QRS_FLOOR;
}

static void push_last8(int *v, int *n, int x) {
    if (*n == 8) {
        memmove(v, v + 1, 7 * sizeof(int));
        v[7] = x;
    } else {
        v[(*n)++] = x;
    }
}

static void update_rr(qrs_detector_t *d, int rr) {
    push_last8(d->rr_recent, &d->n_recent, rr);
    if (!d->n_selected) {
        push_last8(d->rr_selected, &d->n_selected, rr);
        return;
    }
    double avg2 = mean_int(d->rr_selected, d->n_selected);
    if (0.92 * avg2 <= rr && rr <= 1.16 * avg2) {
        push_last8(d->rr_selected, &d->n_selected, rr);
        return;
    }
    if (d->n_recent == 8) {
        for (int i = 0; i < 8; i++) {
            int r = d->rr_recent[i];
            if (0.92 * avg2 <= r && r <= 1.16 * avg2) {
                return;
            }
        }
        // Rhythmuswechsel: acht irreguläre Schläge in Folge -> neu synchronisieren.
        memcpy(d->rr_selected, d->rr_recent, sizeof(d->rr_selected));
        d->n_selected = 8;
    }
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *w, int n) {
    qsort(w, (size_t)n, sizeof(double), cmp_double);
    return (n % 2) ? w[n / 2] : 0.5 * (w[n / 2 - 1] + w[n / 2]);
}

static qrs_beat_t locate(qrs_detector_t *d, int64_t mwi_index, bool searchback) {
    double center = (double)mwi_index - QRS_MWI_DELAY + QRS_REF_DELAY;
    int64_t c = (int64_t)rint(center);  // wie Python round(): halbe Werte zur geraden Zahl
    int64_t len = d->next - d->hist_start;
    int64_t a = c - QRS_LOCATE_HALF - d->hist_start;
    int64_t b = c + QRS_LOCATE_HALF + 1 - d->hist_start;
    if (a < 0) {
        a = 0;
    }
    if (b > len) {
        b = len;
    }
    qrs_beat_t beat = {(double)mwi_index - QRS_MWI_DELAY, 0.0, searchback};
    int n = (int)(b - a);
    if (n < 3) {
        return beat;
    }
    static double w[2 * QRS_LOCATE_HALF + 1];  // statisch: schont den Stapel von Kern 1
    static double tmp[2 * QRS_LOCATE_HALF + 1];
    double wmax = -INFINITY, wmin = INFINITY;
    for (int i = 0; i < n; i++) {
        w[i] = H(d->ref_h, d->hist_start + a + i);
        tmp[i] = w[i];
        if (w[i] > wmax) {
            wmax = w[i];
        }
        if (w[i] < wmin) {
            wmin = w[i];
        }
    }
    double base = median(tmp, n);
    double pos = wmax - base;
    double neg = base - wmin;
    int vote = pos >= neg ? 1 : -1;
    int sign = d->polarity == 0 ? vote : (d->polarity > 0 ? 1 : -1);
    d->polarity += vote;
    if (d->polarity > 8) {
        d->polarity = 8;
    } else if (d->polarity < -8) {
        d->polarity = -8;
    }
    int k = 0;
    for (int i = 1; i < n; i++) {
        if (sign * w[i] > sign * w[k]) {
            k = i;
        }
    }
    double frac = 0.0;
    if (k > 0 && k < n - 1) {
        double y0 = sign * w[k - 1], y1 = sign * w[k], y2 = sign * w[k + 1];
        double denom = y0 - 2.0 * y1 + y2;
        if (denom < 0.0) {
            frac = 0.5 * (y0 - y2) / denom;
        }
    }
    beat.index = (double)(d->hist_start + a + k) + frac - QRS_REF_DELAY;
    beat.amplitude_mv = w[k] - base;
    return beat;
}

typedef struct {
    qrs_beat_t *buf;
    size_t n;
    size_t max;
} out_t;

static void emit(out_t *o, qrs_beat_t b) {
    if (o->n < o->max) {
        o->buf[o->n] = b;
    }
    o->n++;
}

static void accept(qrs_detector_t *d, qrs_cand_t c, bool searchback, out_t *o) {
    double w = searchback ? 0.25 : 0.125;
    d->spki = w * c.value + (1.0 - w) * d->spki;
    if (d->has_last_qrs) {
        update_rr(d, (int)(c.index - d->last_qrs));
    }
    d->has_last_qrs = true;
    d->last_qrs = c.index;
    d->last_slope = c.slope;
    int m = 0;
    for (int i = 0; i < d->n_cands; i++) {
        if (d->cands[i].index > c.index) {
            d->cands[m++] = d->cands[i];
        }
    }
    d->n_cands = m;
    emit(o, locate(d, c.index, searchback));
}

static void search_back(qrs_detector_t *d, int64_t now, out_t *o) {
    while (d->has_last_qrs) {
        double rr;
        if (!rr_average(d, &rr) || (double)(now - d->last_qrs) <= 1.66 * rr) {
            return;
        }
        double t2 = thr2(d);
        int best = -1;
        for (int i = 0; i < d->n_cands; i++) {
            const qrs_cand_t *c = &d->cands[i];
            if (c->index - d->last_qrs >= QRS_REFRACTORY && c->index < now && !c->t_wave &&
                c->value > t2 && (best < 0 || c->value > d->cands[best].value)) {
                best = i;
            }
        }
        if (best < 0) {
            return;
        }
        accept(d, d->cands[best], true, o);
    }
}

static double slope_at(const qrs_detector_t *d, int64_t i) {
    int64_t lo = i - QRS_W;
    if (lo < d->hist_start) {
        lo = d->hist_start;
    }
    double m = -INFINITY;
    for (int64_t j = lo; j <= i; j++) {
        double v = H(d->sq_h, j);
        if (v > m) {
            m = v;
        }
    }
    return m;
}

static void add_candidate(qrs_detector_t *d, qrs_cand_t c) {
    if (d->n_cands == QRS_MAX_CANDS) {  // praktisch unerreichbar (max. 1 Spitze je 100 ms)
        memmove(d->cands, d->cands + 1, (QRS_MAX_CANDS - 1) * sizeof(qrs_cand_t));
        d->n_cands--;
    }
    d->cands[d->n_cands++] = c;
}

static void candidate(qrs_detector_t *d, int64_t i, double v, out_t *o) {
    if (d->has_last_qrs && i - d->last_qrs < QRS_REFRACTORY) {
        return;
    }
    search_back(d, i, o);
    if (d->has_last_qrs && i - d->last_qrs < QRS_REFRACTORY) {
        return;
    }
    double slope = slope_at(d, i);
    bool is_t_wave =
        d->has_last_qrs && i - d->last_qrs < QRS_T_WINDOW && slope < 0.5 * d->last_slope;
    qrs_cand_t c = {i, v, slope, is_t_wave};
    if (v > thr1(d) && !is_t_wave) {
        accept(d, c, false, o);
    } else {
        d->npki = 0.125 * v + 0.875 * d->npki;
        add_candidate(d, c);
    }
}

static void scan(qrs_detector_t *d, int64_t lo, int64_t hi, out_t *o) {
    for (int64_t p = lo; p < hi; p++) {
        double center = H(d->mwi_h, p);
        if (!(center > 0.0) || !(center > H(d->mwi_h, p - 1))) {
            continue;
        }
        bool peak = true;
        for (int64_t j = p - QRS_LOOKAHEAD; j <= p + QRS_LOOKAHEAD; j++) {
            if (H(d->mwi_h, j) > center) {
                peak = false;
                break;
            }
        }
        if (peak) {
            candidate(d, p, center, o);
        }
    }
}

static void stall_check(qrs_detector_t *d, int64_t now) {
    int64_t ref = d->has_last_qrs ? d->last_qrs : d->init_index;
    double rr;
    if (!rr_average(d, &rr) || rr == 0.0) {
        rr = (double)ECG_FS;
    }
    double limit = QRS_STALL > 3.0 * rr ? QRS_STALL : 3.0 * rr;
    if ((double)(now - ref) > limit && now - d->last_stall_decay > ECG_FS) {
        d->spki *= 0.5;
        d->npki *= 0.5;
        d->last_stall_decay = now;
    }
}

static bool try_initialize(qrs_detector_t *d) {
    int64_t learn_end = d->origin + QRS_SETTLE + QRS_LEARN;
    if (d->next < learn_end) {
        return false;
    }
    double mx = -INFINITY, sum = 0.0;
    int64_t a = d->hist_start + QRS_SETTLE;
    for (int64_t i = a; i < learn_end; i++) {
        double v = H(d->mwi_h, i);
        if (v > mx) {
            mx = v;
        }
        sum += v;
    }
    d->spki = mx / 3.0;
    d->npki = sum / (double)(learn_end - a) / 2.0;
    d->initialized = true;
    d->scan_from = d->origin + QRS_SETTLE + QRS_LOOKAHEAD;
    d->init_index = d->scan_from;
    return true;
}

static void trim(qrs_detector_t *d) {
    int64_t excess = (d->next - d->hist_start) - QRS_KEEP;
    if (excess > 0) {
        d->hist_start += excess;
    }
    int64_t horizon = d->hist_start + QRS_LOOKAHEAD;
    int m = 0;
    for (int i = 0; i < d->n_cands; i++) {
        if (d->cands[i].index >= horizon) {
            d->cands[m++] = d->cands[i];
        }
    }
    d->n_cands = m;
}

size_t qrs_process(qrs_detector_t *d, const double *x, size_t n, qrs_beat_t *out,
                   size_t max_out) {
    out_t o = {out, 0, max_out};
    if (n > QRS_MAX_BLOCK) {
        n = QRS_MAX_BLOCK;  // mehr passt nicht zusätzlich in die Historie (siehe qrs.h)
    }
    if (!d->has_origin) {
        d->has_origin = true;
        d->origin = d->next = d->hist_start = 0;
    }
    if (n == 0) {
        return 0;
    }
    if (!d->primed) {
        sos_prime(QRS_BP_SOS, &d->bp, x[0]);
        sos_prime(QRS_REF_SOS, &d->ref, x[0]);
        d->primed = true;
    }
    for (size_t k = 0; k < n; k++) {
        double bp = sos_step(QRS_BP_SOS, &d->bp, x[k]);
        double der = QRS_DERIV_GAIN * (2.0 * bp + d->dx[0] - d->dx[2] - 2.0 * d->dx[3]);
        d->dx[3] = d->dx[2];
        d->dx[2] = d->dx[1];
        d->dx[1] = d->dx[0];
        d->dx[0] = bp;
        double sq = der * der;
        d->sq_sum += sq - d->sq_win[d->sq_pos];
        d->sq_win[d->sq_pos] = sq;
        d->sq_pos = (d->sq_pos + 1) % QRS_W;
        int64_t i = d->next + (int64_t)k;
        H(d->mwi_h, i) = d->sq_sum / (double)QRS_W;
        H(d->sq_h, i) = sq;
        H(d->ref_h, i) = sos_step(QRS_REF_SOS, &d->ref, x[k]);
    }
    d->next += (int64_t)n;

    if (!d->initialized && !try_initialize(d)) {
        return 0;
    }
    int64_t hi = d->next - QRS_LOOKAHEAD;
    if (hi > d->scan_from) {
        scan(d, d->scan_from, hi, &o);
        d->scan_from = hi;
    }
    search_back(d, hi, &o);
    stall_check(d, hi);
    trim(d);
    return o.n < max_out ? o.n : max_out;
}
