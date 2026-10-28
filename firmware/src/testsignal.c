#include "testsignal.h"

#include <math.h>

#define TWO_PI 6.28318530718f

typedef struct {
    float offset_s;  // relativ zur R-Zacke
    float amp_mv;
    float width_s;
    bool scales_with_rr;  // T-Welle wandert mit dem RR-Intervall (QT-Anpassung)
} wave_t;

static const wave_t WAVES[] = {
    {-0.200f, 0.15f, 0.025f, false},  // P
    {-0.035f, -0.10f, 0.010f, false}, // Q
    {0.000f, 1.20f, 0.010f, false},   // R
    {0.035f, -0.25f, 0.012f, false},  // S
    {0.300f, 0.30f, 0.060f, true},    // T
};

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float uniform(uint32_t *state) {
    return (float)(xorshift32(state) >> 8) * (1.0f / 16777216.0f);
}

// Näherung einer Normalverteilung (Summe von 4 Gleichverteilungen), reicht für Rauschen.
static float gauss(uint32_t *state) {
    float s = uniform(state) + uniform(state) + uniform(state) + uniform(state);
    return (s - 2.0f) * 1.7320508f;
}

static float next_rr(ekg_testsig_t *s, float at_s) {
    float rr0 = 60.0f / s->hr_bpm;
    float rsa = 0.04f * sinf(TWO_PI * 0.25f * at_s);
    float jitter = 0.015f * gauss(&s->rng);
    return rr0 * (1.0f + rsa + jitter);
}

static float beat(float dt, float rr) {
    float v = 0.0f;
    for (unsigned i = 0; i < sizeof(WAVES) / sizeof(WAVES[0]); i++) {
        const wave_t *w = &WAVES[i];
        float off = w->scales_with_rr ? w->offset_s * sqrtf(rr) : w->offset_s;
        float x = (dt - off) / w->width_s;
        if (x > -6.0f && x < 6.0f) {
            v += w->amp_mv * expf(-0.5f * x * x);
        }
    }
    return v;
}

void ekg_testsig_init(ekg_testsig_t *s, float fs, float hr_bpm) {
    s->fs = fs;
    s->n = 0;
    s->t = 0.0f;
    s->hr_bpm = hr_bpm;
    s->mains = false;
    s->rng = 0x12345678u;
    s->rr_prev = 60.0f / hr_bpm;
    s->rr_cur = s->rr_prev;
    s->r_cur = 0.5f;
    s->r_prev = s->r_cur - s->rr_prev;
    s->r_next = s->r_cur + s->rr_cur;
}

float ekg_testsig_next_mv(ekg_testsig_t *s) {
    // Fenster weiterschieben, sobald die Mitte zwischen aktueller und nächster R-Zacke erreicht ist.
    if (s->t > 0.5f * (s->r_cur + s->r_next)) {
        s->r_prev = s->r_cur;
        s->rr_prev = s->rr_cur;
        s->r_cur = s->r_next;
        s->rr_cur = next_rr(s, s->r_cur);
        s->r_next = s->r_cur + s->rr_cur;
    }
    // Die T-Welle eines Schlags richtet sich nach dem vorangehenden RR-Intervall.
    float v = beat(s->t - s->r_prev, s->rr_prev) + beat(s->t - s->r_cur, s->rr_prev) +
              beat(s->t - s->r_next, s->rr_cur);
    v += 0.03f * sinf(TWO_PI * 0.2f * s->t);  // Atmung / Restdrift
    v += 0.008f * gauss(&s->rng);             // Rauschen ~8 µV eff
    if (s->mains) {
        v += 0.05f * sinf(TWO_PI * 50.0f * s->t);
    }

    s->n++;
    // Nach 1 h alles um 3600 s zurückschieben, damit die float-Auflösung erhalten bleibt.
    // 3600 s sind ganzzahlige Vielfache aller Modulationsperioden, die Phasen bleiben stetig.
    if (s->n >= (uint32_t)(3600.0f * s->fs)) {
        s->n = 0;
        s->r_prev -= 3600.0f;
        s->r_cur -= 3600.0f;
        s->r_next -= 3600.0f;
    }
    s->t = (float)s->n / s->fs;
    return v;
}
