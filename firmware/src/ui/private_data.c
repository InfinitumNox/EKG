#include "private_data.h"

#include <string.h>

#define MAGIC 0x31565250u  // "PRV1"

typedef struct {
    uint32_t magic;
    uint8_t has_pin, display_mode, n, reserved;
    uint8_t salt[16];
    uint8_t pin_hash[32];
    priv_session_t s[PRIV_SESSIONS];
    uint32_t check;
} stored_t;

_Static_assert(sizeof(stored_t) <= PRIV_RECORD_BYTES, "Datensatz zu groß");
_Static_assert(sizeof(priv_session_t) == 64, "Session: 64 Byte");

void priv_clear(priv_data_t *d) {
    memset(d, 0, sizeof *d);
}

static uint32_t checksum(const void *p, size_t n_words) {
    const unsigned char *b = p;
    uint32_t sum = 0xA5A55A5Au;
    for (size_t i = 0; i < n_words; i++) {
        uint32_t w;
        memcpy(&w, b + 4 * i, 4);
        sum = (sum << 7 | sum >> 25) ^ w;
    }
    return sum;
}

size_t priv_encode(const priv_data_t *d, uint8_t *buf, size_t cap) {
    if (cap < PRIV_RECORD_BYTES) {
        return 0;
    }
    stored_t st;
    memset(&st, 0, sizeof st);
    st.magic = MAGIC;
    st.has_pin = d->has_pin;
    st.display_mode = d->display_mode;
    st.n = d->n > PRIV_SESSIONS ? PRIV_SESSIONS : d->n;
    memcpy(st.salt, d->salt, sizeof st.salt);
    memcpy(st.pin_hash, d->pin_hash, sizeof st.pin_hash);
    memcpy(st.s, d->s, sizeof st.s);
    st.check = checksum(&st, offsetof(stored_t, check) / 4);
    memset(buf, 0xFF, PRIV_RECORD_BYTES);
    memcpy(buf, &st, sizeof st);
    return PRIV_RECORD_BYTES;
}

void priv_decode(const void *flash, priv_data_t *d) {
    priv_clear(d);
    stored_t st;
    memcpy(&st, flash, sizeof st);
    if (st.magic != MAGIC || st.check != checksum(&st, offsetof(stored_t, check) / 4) ||
        st.n > PRIV_SESSIONS || st.display_mode > 1) {
        return;
    }
    d->has_pin = st.has_pin == 1;
    d->display_mode = st.display_mode;
    d->n = st.n;
    memcpy(d->salt, st.salt, sizeof d->salt);
    memcpy(d->pin_hash, st.pin_hash, sizeof d->pin_hash);
    memcpy(d->s, st.s, sizeof d->s);
}

// ------------------------------------------------------------------ SHA-256
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, int n) {
    return x >> n | x << (32 - n);
}

static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void priv_sha256(const uint8_t *msg, size_t n, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        block(h, msg + i);
    }
    uint8_t tail[128];
    size_t r = n - i;
    memcpy(tail, msg + i, r);
    tail[r++] = 0x80;
    const size_t total = r + 8 <= 64 ? 64 : 128;
    memset(tail + r, 0, total - r);
    const uint64_t bits = (uint64_t)n * 8u;
    for (int k = 0; k < 8; k++) {
        tail[total - 1 - (size_t)k] = (uint8_t)(bits >> (8 * k));
    }
    for (size_t k = 0; k < total; k += 64) {
        block(h, tail + k);
    }
    for (int k = 0; k < 8; k++) {
        out[4 * k] = (uint8_t)(h[k] >> 24);
        out[4 * k + 1] = (uint8_t)(h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(h[k] >> 8);
        out[4 * k + 3] = (uint8_t)h[k];
    }
}

// ------------------------------------------------------------------ PIN und Sessions
static void hash_pin(const uint8_t salt[16], const char *digits, uint8_t out[32]) {
    uint8_t msg[16 + PRIV_PIN_DIGITS];
    memcpy(msg, salt, 16);
    memcpy(msg + 16, digits, PRIV_PIN_DIGITS);
    priv_sha256(msg, sizeof msg, out);
}

bool priv_check_pin(const priv_data_t *d, const char *digits) {
    uint8_t h[32];
    hash_pin(d->salt, digits, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (uint8_t)(h[i] ^ d->pin_hash[i]);
    }
    return d->has_pin && diff == 0;
}

void priv_set_pin(priv_data_t *d, const char *digits) {
    priv_random_bytes(d->salt, sizeof d->salt);
    hash_pin(d->salt, digits, d->pin_hash);
    d->has_pin = true;
}

void priv_add_session(priv_data_t *d, const priv_session_t *s) {
    if (d->n >= PRIV_SESSIONS) {
        memmove(&d->s[0], &d->s[1], sizeof d->s[0] * (PRIV_SESSIONS - 1));
        d->n = PRIV_SESSIONS - 1;
    }
    d->s[d->n++] = *s;
}
