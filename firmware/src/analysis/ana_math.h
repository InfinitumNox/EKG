// Rechenbausteine der Auswertung mit denselben Konventionen wie NumPy/SciPy
// (Median, Perzentil- und Medianfilter mit Rand „nearest“, gleitender Mittelwert mit
// Rand „reflect“, Varianz mit ddof, Korrelationskoeffizient, Trapezregel, Lomb-Scargle).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ANA_PI 3.141592653589793  // numpy.pi

// Einfacher Stapelspeicher für Zwischenwerte (wird am Ende von ana_run verworfen)
typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
    bool failed;
    size_t peak;  // größter Bedarf (für Tests und Speicherplanung)
} ana_arena_t;

void *ana_alloc(ana_arena_t *a, size_t bytes);
#define ANA_NEW(a, type, n) ((type *)ana_alloc((a), sizeof(type) * (size_t)(n)))

double ana_round(double v);  // wie Python round(): halb auf gerade
double ana_median(const double *x, int n, double *tmp);  // tmp: n Plätze
double ana_mean(const double *x, int n);
double ana_var(const double *x, int n, int ddof);
double ana_corrcoef(const double *a, const double *b, int n);
double ana_trapezoid(const double *y, const double *x, int n);
// scipy.ndimage.percentile_filter(x, pct, size, mode="nearest")
void ana_percentile_filter(const double *x, int n, double pct, int size, double *out,
                           double *tmp);
// scipy.ndimage.median_filter(x, size, mode="nearest")
void ana_median_filter(const double *x, int n, int size, double *out, double *tmp);
// scipy.ndimage.uniform_filter1d(x, size) mit mode="reflect"
void ana_uniform_filter(const double *x, int n, int size, double *out);
// numpy.gradient(x) bei Abstand 1
void ana_gradient(const double *x, int n, double *out);
// scipy.signal.detrend(y, type="linear")
void ana_detrend_linear(const double *y, int n, double *out);
// scipy.signal.lombscargle(t, y, 2π·f) mit den Voreinstellungen von SciPy 1.18
void ana_lombscargle(const double *t, const double *y, int n, const double *freqs, int nf,
                     double *out);
double ana_sum(const double *x, int n);  // paarweise wie numpy.add.reduce

// Komplexe FFT gemischter Basis (Faktoren bis 13), vorwärts, ohne Normierung
typedef struct {
    float r, i;
} ana_cpx_t;

typedef struct {
    int n;
    int factors[2 * 32];  // je Stufe: Basis p, Restlänge m
    ana_cpx_t *tw;        // exp(-2πi k/n)
} ana_fft_t;

bool ana_fft_init(ana_fft_t *f, int n, ana_arena_t *a);  // false: Primfaktor > 13
void ana_fft(const ana_fft_t *f, const ana_cpx_t *in, ana_cpx_t *out);
