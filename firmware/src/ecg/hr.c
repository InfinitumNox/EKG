#include "hr.h"

#include <string.h>

#define RR_MIN_S 0.25  // 240/min
#define RR_MAX_S 2.5   // 24/min

void hr_init(hr_tracker_t *h, double fs) {
    h->fs = fs;
    hr_reset(h);
}

void hr_reset(hr_tracker_t *h) {
    h->n = 0;
}

void hr_add(hr_tracker_t *h, double r_index) {
    if (h->n == HR_N_INTERVALS + 1) {
        memmove(h->r, h->r + 1, HR_N_INTERVALS * sizeof(double));
        h->n--;
    }
    h->r[h->n++] = r_index;
}

bool hr_bpm(const hr_tracker_t *h, double now_index, double *bpm) {
    double rr[HR_N_INTERVALS];
    int m = 0;
    for (int i = 1; i < h->n; i++) {
        double v = (h->r[i] - h->r[i - 1]) / h->fs;
        if (v >= RR_MIN_S && v <= RR_MAX_S) {
            rr[m++] = v;
        }
    }
    if (m < 2) {
        return false;
    }
    for (int i = 1; i < m; i++) {  // Einfügesortierung, höchstens acht Werte
        double v = rr[i];
        int j = i - 1;
        while (j >= 0 && rr[j] > v) {
            rr[j + 1] = rr[j];
            j--;
        }
        rr[j + 1] = v;
    }
    double med = (m % 2) ? rr[m / 2] : 0.5 * (rr[m / 2 - 1] + rr[m / 2]);
    double stale = 2.5 * med > 3.0 ? 2.5 * med : 3.0;
    if ((now_index - h->r[h->n - 1]) / h->fs > stale) {
        return false;
    }
    *bpm = 60.0 / med;
    return true;
}

bool hr_last_rr_ms(const hr_tracker_t *h, double *rr_ms) {
    if (h->n < 2) {
        return false;
    }
    double v = (h->r[h->n - 1] - h->r[h->n - 2]) / h->fs;
    if (v < RR_MIN_S || v > RR_MAX_S) {
        return false;
    }
    *rr_ms = v * 1000.0;
    return true;
}
