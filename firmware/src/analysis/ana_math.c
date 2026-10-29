#include "ana_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void *ana_alloc(ana_arena_t *a, size_t bytes) {
    size_t at = (a->used + 7u) & ~(size_t)7u;
    if (a->failed || at + bytes > a->size) {
        a->failed = true;
        return NULL;
    }
    a->used = at + bytes;
    if (a->used > a->peak) {
        a->peak = a->used;
    }
    return a->base + at;
}

double ana_round(double v) {
    return rint(v);  // Rundungsmodus „nächster, bei Gleichstand gerade“ wie Python
}

// numpy pairwise_sum (8-fach entrollt, Blöcke bis 128)
double ana_sum(const double *x, int n) {
    if (n < 8) {
        double res = 0.0;
        for (int i = 0; i < n; i++) {
            res += x[i];
        }
        return res;
    }
    if (n <= 128) {
        double r[8];
        for (int j = 0; j < 8; j++) {
            r[j] = x[j];
        }
        int i;
        for (i = 8; i < n - (n % 8); i += 8) {
            for (int j = 0; j < 8; j++) {
                r[j] += x[i + j];
            }
        }
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++) {
            res += x[i];
        }
        return res;
    }
    int n2 = n / 2;
    n2 -= n2 % 8;
    return ana_sum(x, n2) + ana_sum(x + n2, n - n2);
}

double ana_mean(const double *x, int n) {
    return n > 0 ? ana_sum(x, n) / (double)n : NAN;
}

// wie numpy: Mittelwert, dann Summe der Abweichungsquadrate (paarweise) / (n - ddof);
// die Quadrate entstehen blockweise, damit kein zweites Feld nötig ist
static double sum_sq_dev(const double *x, int n, double mean) {
    if (n < 8) {
        double res = 0.0;
        for (int i = 0; i < n; i++) {
            double d = x[i] - mean;
            res += d * d;
        }
        return res;
    }
    if (n <= 128) {
        double r[8];
        for (int j = 0; j < 8; j++) {
            double d = x[j] - mean;
            r[j] = d * d;
        }
        int i;
        for (i = 8; i < n - (n % 8); i += 8) {
            for (int j = 0; j < 8; j++) {
                double d = x[i + j] - mean;
                r[j] += d * d;
            }
        }
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++) {
            double d = x[i] - mean;
            res += d * d;
        }
        return res;
    }
    int n2 = n / 2;
    n2 -= n2 % 8;
    return sum_sq_dev(x, n2, mean) + sum_sq_dev(x + n2, n - n2, mean);
}

double ana_var(const double *x, int n, int ddof) {
    if (n - ddof <= 0) {
        return NAN;
    }
    double m = ana_mean(x, n);
    return sum_sq_dev(x, n, m) / (double)(n - ddof);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double ana_median(const double *x, int n, double *tmp) {
    if (n <= 0) {
        return NAN;
    }
    for (int i = 0; i < n; i++) {
        if (isnan(x[i])) {
            return NAN;  // numpy.median gibt bei NaN NaN zurück
        }
        tmp[i] = x[i];
    }
    qsort(tmp, (size_t)n, sizeof(double), cmp_double);
    if (n % 2) {
        return tmp[n / 2];
    }
    return (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
}

double ana_corrcoef(const double *a, const double *b, int n) {
    if (n < 2) {
        return NAN;
    }
    double ma = ana_mean(a, n), mb = ana_mean(b, n);
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; i++) {
        double da = a[i] - ma, db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    double f = 1.0 / (double)(n - 1);
    double c = sab * f;
    double sa = sqrt(saa * f), sb = sqrt(sbb * f);
    c /= sa;
    c /= sb;
    if (isnan(c)) {
        return c;
    }
    return c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
}

double ana_trapezoid(const double *y, const double *x, int n) {
    if (n < 2) {
        return 0.0;
    }
    // numpy: add.reduce(diff(x) * (y[1:] + y[:-1]) / 2.0), paarweise summiert; längere
    // Felder werden wie dort halbiert (auf Vielfache von 8), Blöcke bis 128 direkt summiert
    int m = n - 1;
    if (m <= 128) {
        double buf[128];
        for (int i = 0; i < m; i++) {
            buf[i] = (x[i + 1] - x[i]) * (y[i + 1] + y[i]) / 2.0;
        }
        return ana_sum(buf, m);
    }
    int n2 = m / 2;
    n2 -= n2 % 8;
    return ana_trapezoid(y, x, n2 + 1) + ana_trapezoid(y + n2, x + n2, m - n2 + 1);
}

void ana_percentile_filter(const double *x, int n, double pct, int size, double *out,
                           double *tmp) {
    int rank = (int)((double)size * pct / 100.0);
    if (rank >= size) {
        rank = size - 1;
    }
    int half = size / 2;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < size; j++) {
            int k = i - half + j;
            k = k < 0 ? 0 : k >= n ? n - 1 : k;
            tmp[j] = x[k];
        }
        qsort(tmp, (size_t)size, sizeof(double), cmp_double);
        out[i] = tmp[rank];
    }
}

void ana_median_filter(const double *x, int n, int size, double *out, double *tmp) {
    int half = size / 2;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < size; j++) {
            int k = i - half + j;
            k = k < 0 ? 0 : k >= n ? n - 1 : k;
            tmp[j] = x[k];
        }
        qsort(tmp, (size_t)size, sizeof(double), cmp_double);
        out[i] = tmp[size / 2];
    }
}

static double reflect_at(const double *x, int n, int k) {
    while (k < 0 || k >= n) {
        k = k < 0 ? -k - 1 : 2 * n - k - 1;
    }
    return x[k];
}

void ana_uniform_filter(const double *x, int n, int size, double *out) {
    if (n <= 0) {
        return;
    }
    int size1 = size / 2;
    // wie scipy (NI_UniformFilter1D): Startsumme, dann laufend (neu - alt) / size
    double tmp = 0.0;
    for (int l = 0; l < size; l++) {
        tmp += reflect_at(x, n, l - size1);
    }
    tmp /= (double)size;
    out[0] = tmp;
    for (int l = 1; l < n; l++) {
        tmp += (reflect_at(x, n, l + size - 1 - size1) - reflect_at(x, n, l - 1 - size1)) /
               (double)size;
        out[l] = tmp;
    }
}

void ana_gradient(const double *x, int n, double *out) {
    if (n < 2) {
        if (n == 1) {
            out[0] = 0.0;
        }
        return;
    }
    out[0] = (x[1] - x[0]) / 1.0;
    for (int i = 1; i < n - 1; i++) {
        out[i] = (x[i + 1] - x[i - 1]) / 2.0;
    }
    out[n - 1] = (x[n - 1] - x[n - 2]) / 1.0;
}

void ana_detrend_linear(const double *y, int n, double *out) {
    // Ausgleichsgerade über x = (1 … n) / n wie scipy, geschlossen gelöst
    double sx = 0, sy = 0;
    for (int i = 0; i < n; i++) {
        sx += (double)(i + 1) / (double)n;
        sy += y[i];
    }
    double mx = sx / n, my = sy / n;
    double sxy = 0, sxx = 0;
    for (int i = 0; i < n; i++) {
        double dx = (double)(i + 1) / (double)n - mx;
        sxy += dx * (y[i] - my);
        sxx += dx * dx;
    }
    double a = sxx > 0 ? sxy / sxx : 0.0, b = my - a * mx;
    for (int i = 0; i < n; i++) {
        out[i] = y[i] - (a * ((double)(i + 1) / (double)n) + b);
    }
}

void ana_lombscargle(const double *t, const double *y, int n, const double *freqs, int nf,
                     double *out) {
    const double w = 1.0 / (double)n;  // Gewichte summieren sich zu 1
    const double epsneg = 1.1102230246251565e-16;
    for (int k = 0; k < nf; k++) {
        double om = 2.0 * ANA_PI * freqs[k];
        double cc = 0, cs = 0;
        for (int j = 0; j < n; j++) {
            double c = cos(om * t[j]), s = sin(om * t[j]);
            cc += w * (c * c);
            cs += w * (c * s);
        }
        double ss = 1.0 - cc;
        double tau = 0.5 * atan2(2.0 * cs, cc - ss);
        double yc = 0, ys = 0, cc2 = 0;
        for (int j = 0; j < n; j++) {
            double ph = om * t[j] - tau;
            double c = cos(ph), s = sin(ph);
            yc += (w * y[j]) * c;
            ys += (w * y[j]) * s;
            cc2 += w * (c * c);
        }
        double ss2 = 1.0 - cc2;
        if (cc2 < epsneg) {
            cc2 = epsneg;
        }
        if (ss2 < epsneg) {
            ss2 = epsneg;
        }
        double a = yc / cc2, b = ys / ss2;
        out[k] = 2.0 * (a * yc + b * ys) * ((double)n / 4.0);
    }
}

// ------------------------------------------------------------------ FFT
static ana_cpx_t cmul(ana_cpx_t a, ana_cpx_t b) {
    return (ana_cpx_t){a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}

bool ana_fft_init(ana_fft_t *f, int n, ana_arena_t *a) {
    f->n = n;
    int rest = n, k = 0;
    const int radices[] = {4, 2, 3, 5, 7, 11, 13};
    for (size_t r = 0; r < sizeof radices / sizeof radices[0] && rest > 1;) {
        int p = radices[r];
        if (rest % p == 0 && k < 62) {
            rest /= p;
            f->factors[k++] = p;
            f->factors[k++] = rest;
        } else {
            r++;
        }
    }
    if (rest != 1) {
        return false;
    }
    f->tw = ANA_NEW(a, ana_cpx_t, n);
    if (!f->tw) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        double ph = -2.0 * ANA_PI * (double)i / (double)n;
        f->tw[i] = (ana_cpx_t){(float)cos(ph), (float)sin(ph)};
    }
    return true;
}

static void bfly(const ana_fft_t *f, ana_cpx_t *out, int fstride, int m, int p) {
    ana_cpx_t s[13];
    const int n = f->n;
    for (int u = 0; u < m; u++) {
        for (int q = 0, k = u; q < p; q++, k += m) {
            s[q] = out[k];
        }
        for (int q1 = 0, k = u; q1 < p; q1++, k += m) {
            int idx = 0;
            ana_cpx_t acc = s[0];
            for (int q = 1; q < p; q++) {
                idx += fstride * k;
                idx %= n;
                ana_cpx_t t = cmul(s[q], f->tw[idx]);
                acc.r += t.r;
                acc.i += t.i;
            }
            out[k] = acc;
        }
    }
}

static void work(const ana_fft_t *f, ana_cpx_t *out, const ana_cpx_t *in, int fstride,
                 const int *factors) {
    const int p = factors[0], m = factors[1];
    if (m == 1) {
        for (int q = 0; q < p; q++) {
            out[q] = in[q * fstride];
        }
    } else {
        for (int q = 0; q < p; q++) {
            work(f, out + q * m, in + q * fstride, fstride * p, factors + 2);
        }
    }
    bfly(f, out, fstride, m, p);
}

void ana_fft(const ana_fft_t *f, const ana_cpx_t *in, ana_cpx_t *out) {
    if (f->n == 1) {
        out[0] = in[0];
        return;
    }
    work(f, out, in, 1, f->factors);
}
