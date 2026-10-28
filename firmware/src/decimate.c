#include "decimate.h"

size_t ekg_decimate(const uint16_t *in, size_t n_in, uint32_t factor, uint32_t *out, bool *clip) {
    if (factor == 0) {
        return 0;
    }
    size_t n_out = n_in / factor;
    for (size_t k = 0; k < n_out; k++) {
        uint32_t acc = 0;
        const uint16_t *p = in + k * factor;
        for (uint32_t i = 0; i < factor; i++) {
            uint16_t v = p[i] & 0x0FFF;
            if (v <= EKG_ADC_CLIP_LOW || v >= EKG_ADC_CLIP_HIGH) {
                *clip = true;
            }
            acc += v;
        }
        out[k] = acc;
    }
    return n_out;
}
