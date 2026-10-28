// Host-Tests für Protokoll, Dezimation und Testsignal.
// Der Golden-Frame ist identisch mit host/tests/test_protocol.py, damit Firmware und
// Host nachweislich dasselbe Format sprechen.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decimate.h"
#include "protocol.h"
#include "testsignal.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static void hex_to_bytes(const char *hex, uint8_t *out, size_t *len) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    *len = n;
}

static void test_crc(void) {
    CHECK(ekg_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
}

static void check_cobs(const char *in_hex, const char *out_hex) {
    uint8_t in[600], expected[600], enc[700], dec[600];
    size_t in_len, exp_len;
    hex_to_bytes(in_hex, in, &in_len);
    hex_to_bytes(out_hex, expected, &exp_len);
    size_t n = ekg_cobs_encode(in, in_len, enc);
    CHECK(n == exp_len);
    CHECK(memcmp(enc, expected, exp_len) == 0);
    size_t m = ekg_cobs_decode(enc, n, dec, sizeof(dec));
    CHECK(m == in_len);
    CHECK(memcmp(dec, in, in_len) == 0);
}

static void test_cobs(void) {
    check_cobs("00", "0101");
    check_cobs("0000", "010101");
    check_cobs("11220033", "0311220233");
    check_cobs("11223344", "0511223344");
    check_cobs("11000000", "0211010101");

    // 254 Bytes ungleich null: ein voller Block, kanonisch ohne angehängten Leerblock.
    uint8_t in[255], enc[300], dec[300];
    for (int i = 0; i < 254; i++) {
        in[i] = (uint8_t)(i + 1);
    }
    size_t n = ekg_cobs_encode(in, 254, enc);
    CHECK(n == 255);
    CHECK(enc[0] == 0xFF);
    CHECK(ekg_cobs_decode(enc, n, dec, sizeof(dec)) == 254);
    CHECK(memcmp(dec, in, 254) == 0);

    // 255 Bytes: voller Block plus ein Byte.
    in[254] = 0xFF;
    n = ekg_cobs_encode(in, 255, enc);
    CHECK(n == 257);
    CHECK(enc[255] == 0x02 && enc[256] == 0xFF);
    CHECK(ekg_cobs_decode(enc, n, dec, sizeof(dec)) == 255);

    // Ungültig: Null im Datenstrom, Block länger als die Eingabe, zu kleiner Puffer.
    uint8_t bad1[] = {0x03, 0x11, 0x00};
    CHECK(ekg_cobs_decode(bad1, sizeof(bad1), dec, sizeof(dec)) == 0);
    uint8_t bad2[] = {0x05, 0x11};
    CHECK(ekg_cobs_decode(bad2, sizeof(bad2), dec, sizeof(dec)) == 0);
    uint8_t ok[] = {0x05, 0x11, 0x22, 0x33, 0x44};
    CHECK(ekg_cobs_decode(ok, sizeof(ok), dec, 2) == 0);
}

static void test_golden_samples_frame(void) {
    const uint32_t samples[] = {0, 1, 0x00ABCDEF};
    uint8_t payload[EKG_MAX_PAYLOAD];
    size_t len = ekg_build_samples(payload, 0x1234, 0x01020304, 0x05, samples, 3);
    uint8_t expected[64];
    size_t exp_len;
    hex_to_bytes("010134120403020103050000000001000000efcdab006d80", expected, &exp_len);
    CHECK(len == exp_len);
    CHECK(memcmp(payload, expected, exp_len) == 0);
    CHECK(ekg_payload_valid(payload, len));

    uint8_t frame[EKG_MAX_FRAME];
    size_t flen = ekg_frame(payload, len, frame);
    hex_to_bytes("0b010134120403020103050101010201010104efcdab036d8000", expected, &exp_len);
    CHECK(flen == exp_len);
    CHECK(memcmp(frame, expected, exp_len) == 0);

    payload[7] ^= 0x01;  // ein Bitfehler muss erkannt werden
    CHECK(!ekg_payload_valid(payload, len));
}

static void test_build_limits(void) {
    uint32_t samples[EKG_MAX_SAMPLES_PER_FRAME + 1] = {0};
    uint8_t payload[EKG_MAX_PAYLOAD];
    CHECK(ekg_build_samples(payload, 0, 0, 0, samples, EKG_MAX_SAMPLES_PER_FRAME) > 0);
    CHECK(ekg_build_samples(payload, 0, 0, 0, samples, EKG_MAX_SAMPLES_PER_FRAME + 1) == 0);

    ekg_info_t info = {.fw_version = 0x000100, .sample_rate_hz = 500, .oversampling = 64,
                       .adc_bits = 12, .vref_mv = 3300, .gain_nominal = 1100,
                       .name = "Pico EKG AD8232"};
    size_t n = ekg_build_info(payload, 7, &info);
    CHECK(n == 4 + 4 + 2 + 2 + 1 + 2 + 2 + 1 + 1 + 8 + 1 + 15 + 2);
    CHECK(ekg_payload_valid(payload, n));
}

static void test_decimate(void) {
    uint16_t in[128];
    for (int i = 0; i < 128; i++) {
        in[i] = (uint16_t)(2000 + (i % 2));
    }
    uint32_t out[2];
    bool clip = false;
    CHECK(ekg_decimate(in, 128, 64, out, &clip) == 2);
    CHECK(out[0] == 64u * 2000u + 32u);
    CHECK(!clip);
    in[5] = 4095;
    ekg_decimate(in, 128, 64, out, &clip);
    CHECK(clip);
}

static void test_testsignal(void) {
    ekg_testsig_t s;
    const float fs = 500.0f;
    ekg_testsig_init(&s, fs, 60.0f);
    int n = (int)(60.0f * fs);
    float *v = malloc(sizeof(float) * (size_t)n);
    float vmax = -1e9f, vmin = 1e9f;
    for (int i = 0; i < n; i++) {
        v[i] = ekg_testsig_next_mv(&s);
        vmax = fmaxf(vmax, v[i]);
        vmin = fminf(vmin, v[i]);
    }
    CHECK(vmax > 1.0f && vmax < 1.6f);
    CHECK(vmin > -0.6f);
    // R-Zacken zählen: einfache Schwelle mit 250 ms Totzeit; erwartet ~60 Schläge.
    int beats = 0, last = -1000;
    for (int i = 1; i < n - 1; i++) {
        if (v[i] > 0.7f && v[i] >= v[i - 1] && v[i] > v[i + 1] && i - last > 125) {
            beats++;
            last = i;
        }
    }
    CHECK(beats >= 55 && beats <= 65);
    free(v);
}

int main(void) {
    test_crc();
    test_cobs();
    test_golden_samples_frame();
    test_build_limits();
    test_decimate();
    test_testsignal();
    if (failures) {
        fprintf(stderr, "%d Fehler\n", failures);
        return 1;
    }
    printf("alle Firmware-Tests bestanden\n");
    return 0;
}
