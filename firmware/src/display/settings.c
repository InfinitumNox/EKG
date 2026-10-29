#include "settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#define SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define MAGIC_V1 0x534B4547u  // "SEKG": Drehung und Stufe (0.3.x)
#define MAGIC 0x324B4553u     // "SEK2": dazu die Touch-Kalibrierung
#define SAVE_DELAY_MS 3000u

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
    uint8_t reserved;
    float cal[6];
    uint32_t check;
} stored_t;

static volatile bool pending;
static volatile uint32_t requested_at;
static ui_settings_t wanted;
static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static uint32_t checksum_v1(const stored_v1_t *s) {
    return s->magic ^ ((uint32_t)s->rotation << 8 | s->look) ^ 0x5A5AA5A5u;
}

static uint32_t checksum(const stored_t *s) {
    const uint32_t *w = (const uint32_t *)s;
    uint32_t sum = 0x5A5AA5A5u;
    for (size_t i = 0; i < offsetof(stored_t, check) / 4; i++) {
        sum = (sum << 5 | sum >> 27) ^ w[i];
    }
    return sum;
}

void settings_load(ui_settings_t *s) {
    memset(s, 0, sizeof *s);
    const stored_t *st = (const stored_t *)(XIP_BASE + SETTINGS_OFFSET);
    const stored_v1_t *v1 = (const stored_v1_t *)(XIP_BASE + SETTINGS_OFFSET);
    if (st->magic == MAGIC && st->check == checksum(st) && st->rotation <= 1 && st->look <= 3) {
        s->rotation = st->rotation;
        s->look = st->look;
        s->has_cal = st->has_cal == 1;
        s->cal = (touch_cal_t){st->cal[0], st->cal[1], st->cal[2],
                               st->cal[3], st->cal[4], st->cal[5]};
    } else if (v1->magic == MAGIC_V1 && v1->check == checksum_v1(v1) && v1->rotation <= 1 &&
               v1->look <= 3) {
        s->rotation = v1->rotation;
        s->look = v1->look;
    }
}

void settings_request_save(const ui_settings_t *s, uint32_t now_ms) {
    wanted = *s;
    requested_at = now_ms;
    __dmb();
    pending = true;
}

static void write_flash(void *param) {
    (void)param;
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_OFFSET, page, FLASH_PAGE_SIZE);
}

void settings_service(uint32_t now_ms) {
    if (!pending || now_ms - requested_at < SAVE_DELAY_MS) {
        return;
    }
    pending = false;
    __dmb();
    stored_t st;
    memset(&st, 0, sizeof st);
    st.magic = MAGIC;
    st.rotation = wanted.rotation;
    st.look = wanted.look;
    st.has_cal = wanted.has_cal ? 1 : 0;
    const touch_cal_t *c = &wanted.cal;
    const float k[6] = {c->a, c->b, c->c, c->d, c->e, c->f};
    for (int i = 0; i < 6; i++) {
        st.cal[i] = k[i];
    }
    st.check = checksum(&st);
    memset(page, 0xFF, sizeof page);
    memcpy(page, &st, sizeof st);
    // ~50 ms; die ADC-DMA läuft weiter, schlimmstenfalls geht ein Block für das Display verloren
    flash_safe_execute(write_flash, NULL, 100);
}
