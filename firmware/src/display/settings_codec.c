// Speicherformat der Einstellungen. Jede Fassung hat eine eigene Kennung; ältere werden
// beim Laden übernommen, geschrieben wird immer die neueste.

#include <stddef.h>
#include <string.h>

#include "settings.h"

#define MAGIC_V1 0x534B4547u  // "SEKG": Drehung und Stufe (0.3.x)
#define MAGIC_V2 0x324B4553u  // "SEK2": dazu Touch-Kalibrierung und VSYS-Teiler (0.4.x)
#define MAGIC_V3 0x334B4553u  // "SEK3": dazu Verstärkung und Vorschub (0.5.x)
#define MAGIC 0x344B4553u     // "SEK4": dazu Person und Profile (0.6.x)

typedef struct {
    uint32_t magic;
    uint8_t rotation;
    uint8_t look;
    uint8_t reserved[2];
    uint32_t check;
} stored_v1_t;

typedef struct {
    uint32_t magic;
    uint8_t rotation;
    uint8_t look;
    uint8_t has_cal;
    uint8_t vsys;
    float cal[6];
    uint32_t check;
} stored_v2_t;

typedef struct {
    uint32_t magic;
    uint8_t rotation;
    uint8_t look;
    uint8_t has_cal;
    uint8_t vsys;
    uint8_t gain;
    uint8_t speed;
    uint8_t reserved[2];
    float cal[6];
    uint32_t check;
} stored_v3_t;

typedef struct {
    uint32_t magic;
    uint8_t rotation;
    uint8_t look;
    uint8_t has_cal;
    uint8_t vsys;
    uint8_t gain;
    uint8_t speed;
    uint8_t person;
    uint8_t opts;  // Bit 0: Standardfigur, Bit 2–3: Kabelfarben
    float cal[6];
    profile_t profiles[PROFILE_COUNT];
    uint32_t check;
} stored_t;

_Static_assert(sizeof(stored_t) <= 256, "Datensatz passt nicht in eine Flash-Seite");
_Static_assert(offsetof(stored_t, check) % 4 == 0, "Prüfsumme über ganze Wörter");

uint32_t settings_checksum(const void *p, size_t n_words) {
    const unsigned char *b = p;
    uint32_t sum = 0x5A5AA5A5u;
    for (size_t i = 0; i < n_words; i++) {
        uint32_t w;
        memcpy(&w, b + 4 * i, 4);
        sum = (sum << 5 | sum >> 27) ^ w;
    }
    return sum;
}

static uint32_t checksum_v1(const stored_v1_t *s) {
    return s->magic ^ ((uint32_t)s->rotation << 8 | s->look) ^ 0x5A5AA5A5u;
}

void settings_defaults(ui_settings_t *s) {
    memset(s, 0, sizeof *s);
    s->speed = 1;  // 25 mm/s
    s->person = PERSON_GUEST;
}

static touch_cal_t cal_from(const float *k) {
    return (touch_cal_t){k[0], k[1], k[2], k[3], k[4], k[5]};
}

void settings_decode(const void *flash, ui_settings_t *s) {
    settings_defaults(s);
    stored_t st;
    stored_v3_t v3;
    stored_v2_t v2;
    stored_v1_t v1;
    memcpy(&st, flash, sizeof st);
    memcpy(&v3, flash, sizeof v3);
    memcpy(&v2, flash, sizeof v2);
    memcpy(&v1, flash, sizeof v1);
    if (st.magic == MAGIC && st.check == settings_checksum(&st, offsetof(stored_t, check) / 4) &&
        st.rotation <= 1 && st.look <= 3) {
        s->rotation = st.rotation;
        s->look = st.look;
        s->has_cal = st.has_cal == 1;
        s->vsys = st.vsys <= VSYS_NONE ? st.vsys : VSYS_UNKNOWN;
        s->gain = st.gain <= 3 ? st.gain : 0;
        s->speed = st.speed <= 2 ? st.speed : 1;
        s->person = st.person <= PERSON_GUEST ? st.person : PERSON_GUEST;
        s->avatar = st.opts & 1u;
        s->cables = (st.opts >> 2) & 3u;
        if (s->cables >= CABLE_PRESETS) {
            s->cables = 0;
        }
        s->cal = cal_from(st.cal);
        for (int i = 0; i < PROFILE_COUNT; i++) {
            s->profiles[i] = st.profiles[i];
            profile_sanitize(&s->profiles[i]);
        }
    } else if (v3.magic == MAGIC_V3 &&
               v3.check == settings_checksum(&v3, offsetof(stored_v3_t, check) / 4) &&
               v3.rotation <= 1 && v3.look <= 3) {
        s->rotation = v3.rotation;
        s->look = v3.look;
        s->has_cal = v3.has_cal == 1;
        s->vsys = v3.vsys <= VSYS_NONE ? v3.vsys : VSYS_UNKNOWN;
        s->gain = v3.gain <= 3 ? v3.gain : 0;
        s->speed = v3.speed <= 2 ? v3.speed : 1;
        s->cal = cal_from(v3.cal);
    } else if (v2.magic == MAGIC_V2 &&
               v2.check == settings_checksum(&v2, offsetof(stored_v2_t, check) / 4) &&
               v2.rotation <= 1 && v2.look <= 3) {
        s->rotation = 0;  // früher „180° gedreht“, jetzt hieße 1 hochkant
        s->look = v2.look;
        s->has_cal = v2.has_cal == 1;
        s->vsys = v2.vsys <= VSYS_NONE ? v2.vsys : VSYS_UNKNOWN;
        s->cal = cal_from(v2.cal);
    } else if (v1.magic == MAGIC_V1 && v1.check == checksum_v1(&v1) && v1.rotation <= 1 &&
               v1.look <= 3) {
        s->look = v1.look;
    }
}

size_t settings_encode(const ui_settings_t *s, uint8_t *page, size_t page_size) {
    stored_t st;
    memset(&st, 0, sizeof st);
    st.magic = MAGIC;
    st.rotation = s->rotation;
    st.look = s->look;
    st.has_cal = s->has_cal ? 1 : 0;
    st.vsys = s->vsys;
    st.gain = s->gain;
    st.speed = s->speed;
    st.person = s->person;
    st.opts = (uint8_t)((s->avatar & 1u) | (s->cables & 3u) << 2);
    const touch_cal_t *c = &s->cal;
    const float k[6] = {c->a, c->b, c->c, c->d, c->e, c->f};
    for (int i = 0; i < 6; i++) {
        st.cal[i] = k[i];
    }
    for (int i = 0; i < PROFILE_COUNT; i++) {
        st.profiles[i] = s->profiles[i];
        profile_sanitize(&st.profiles[i]);
    }
    st.check = settings_checksum(&st, offsetof(stored_t, check) / 4);
    memset(page, 0xFF, page_size);
    memcpy(page, &st, sizeof st);
    return sizeof st;
}
