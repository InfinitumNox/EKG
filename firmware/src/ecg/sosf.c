#include "sosf.h"

#include "ecg_tables.h"

_Static_assert(sizeof(ECG_DISPLAY_SOS) / sizeof(ECG_DISPLAY_SOS[0]) == SOSF_SECTIONS,
               "SOSF_SECTIONS passt nicht zu ecg_tables.h");

void sosf_reset(sosf_t *f) {
    f->primed = false;
}

float sosf_step(sosf_t *f, float x) {
    if (!f->primed) {  // auf den ersten Wert einschwingen, wie StreamingSos am Laptop
        for (int k = 0; k < SOSF_SECTIONS; k++) {
            f->z[k][0] = ECG_DISPLAY_SOS[k][5] * x;
            f->z[k][1] = ECG_DISPLAY_SOS[k][6] * x;
        }
        f->primed = true;
    }
    for (int k = 0; k < SOSF_SECTIONS; k++) {
        const float *c = ECG_DISPLAY_SOS[k];
        float y = c[0] * x + f->z[k][0];
        f->z[k][0] = c[1] * x - c[3] * y + f->z[k][1];
        f->z[k][1] = c[2] * x - c[4] * y;
        x = y;
    }
    return x;
}
