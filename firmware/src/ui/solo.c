#include "solo.h"

#include <math.h>
#include <string.h>

static void sort_f(float *x, int n) {
    for (int i = 1; i < n; i++) {
        float v = x[i];
        int j = i - 1;
        while (j >= 0 && x[j] > v) {
            x[j + 1] = x[j];
            j--;
        }
        x[j + 1] = v;
    }
}

static float median_f(float *x, int n) {
    sort_f(x, n);
    return n % 2 ? x[n / 2] : 0.5f * (x[n / 2 - 1] + x[n / 2]);
}

// Perzentil mit linearer Interpolation (x ist sortiert)
static float percentile(const float *x, int n, float q) {
    const float pos = q * (float)(n - 1);
    const int i = (int)floorf(pos);
    return i + 1 < n ? x[i] + (pos - (float)i) * (x[i + 1] - x[i]) : x[n - 1];
}

void solo_build(const priv_data_t *d, solo_model_t *m) {
    memset(m, 0, sizeof *m);
    float gains[SOLO_MODEL_SESSIONS], peaks[SOLO_MODEL_SESSIONS], dur[SOLO_MODEL_SESSIONS];
    int n = 0;
    for (int i = (int)d->n - 1; i >= 0 && n < SOLO_MODEL_SESSIONS; i--) {
        const priv_session_t *s = &d->s[i];
        if (s->flags & PRIV_EXCLUDED) {
            continue;
        }
        gains[n] = fmaxf((float)s->hr_peak - (float)s->hr_start, 1.0f);
        peaks[n] = s->hr_peak;
        dur[n] = s->duration_s;
        n++;
    }
    m->n = n;
    m->learned = n >= SOLO_MIN_SESSIONS;
    if (!m->learned) {
        return;
    }
    float tmp[SOLO_MODEL_SESSIONS];
    memcpy(tmp, gains, sizeof(float) * (size_t)n);
    m->gain = median_f(tmp, n);
    for (int i = 0; i < n; i++) {
        tmp[i] = fabsf(gains[i] - m->gain);
    }
    const float mad = median_f(tmp, n);
    memcpy(tmp, peaks, sizeof(float) * (size_t)n);
    m->peak = median_f(tmp, n);
    sort_f(dur, n);
    m->t_lo = (int)lrintf(percentile(dur, n, 0.25f));
    m->t_hi = (int)lrintf(percentile(dur, n, 0.75f));
    m->t_med = (int)lrintf(percentile(dur, n, 0.5f));
    m->confident = m->gain >= 8.0f && mad / m->gain < 0.35f;
}

int solo_percent(const solo_model_t *m, float start, float now) {
    if (!m->confident) {
        return -1;
    }
    const float p = (now - start) / m->gain * 100.0f;
    return (int)lrintf(p < 0.0f ? 0.0f : p > 100.0f ? 100.0f : p);
}

float solo_start(const uint8_t *hr, int n_s) {
    float v[30];
    int k = 0;
    for (int i = 0; i < n_s && k < 30; i++) {
        if (hr[i]) {
            v[k++] = hr[i];
        }
    }
    return k ? median_f(v, k) : NAN;
}

float solo_median5(const uint8_t *hr, int n_s, int end) {
    float v[5];
    int k = 0;
    for (int i = end - 4; i <= end; i++) {
        if (i >= 0 && i < n_s && hr[i]) {
            v[k++] = hr[i];
        }
    }
    return k >= 3 ? median_f(v, k) : NAN;
}

bool solo_summarize(const uint8_t *hr, int n_s, priv_session_t *out) {
    memset(out, 0, sizeof *out);
    int valid = 0;
    for (int i = 0; i < n_s; i++) {
        valid += hr[i] != 0;
    }
    if (valid < 20) {
        return false;
    }
    const float start = solo_start(hr, n_s);
    float peak = NAN;
    for (int e = n_s - 1; e >= 4 && e >= n_s - 20; e--) {  // die letzten 20 s: Reaktionszeit
        const float m = solo_median5(hr, n_s, e);
        if (!isnan(m) && (isnan(peak) || m > peak)) {
            peak = m;
        }
    }
    if (isnan(start) || isnan(peak)) {
        return false;
    }
    out->duration_s = (uint16_t)(n_s > 65535 ? 65535 : n_s);
    out->hr_start = (uint8_t)lrintf(start);
    out->hr_peak = (uint8_t)lrintf(peak);
    for (int b = 0; b < PRIV_CURVE; b++) {
        const int i0 = b * n_s / PRIV_CURVE, i1 = (b + 1) * n_s / PRIV_CURVE;
        int sum = 0, c = 0;
        for (int i = i0; i < i1 && i < n_s; i++) {
            if (hr[i]) {
                sum += hr[i];
                c++;
            }
        }
        out->curve[b] = c ? (uint8_t)((sum + c / 2) / c) : 0;
    }
    return true;
}

int solo_t90(const uint8_t *hr, int n_s, float start, float gain) {
    for (int i = 4; i < n_s; i++) {
        const float m = solo_median5(hr, n_s, i);
        if (!isnan(m) && m - start >= 0.9f * gain) {
            return i;
        }
    }
    return -1;
}
