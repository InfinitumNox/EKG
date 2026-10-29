#include "settings.h"

#include <stdbool.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#define SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define MAGIC 0x534B4547u  // "SEKG"
#define SAVE_DELAY_MS 3000u

typedef struct {
    uint32_t magic;
    uint8_t rotation;
    uint8_t look;
    uint8_t reserved[2];
    uint32_t check;
} stored_t;

static volatile bool pending;
static volatile uint32_t requested_at;
static ui_settings_t wanted;
static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static uint32_t checksum(const stored_t *s) {
    return s->magic ^ ((uint32_t)s->rotation << 8 | s->look) ^ 0x5A5AA5A5u;
}

void settings_load(ui_settings_t *s) {
    const stored_t *st = (const stored_t *)(XIP_BASE + SETTINGS_OFFSET);
    s->rotation = 0;
    s->look = 0;
    if (st->magic == MAGIC && st->check == checksum(st) && st->rotation <= 1 &&
        st->look <= 3) {
        s->rotation = st->rotation;
        s->look = st->look;
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
    stored_t st = {MAGIC, wanted.rotation, wanted.look, {0, 0}, 0};
    st.check = checksum(&st);
    memset(page, 0xFF, sizeof page);
    memcpy(page, &st, sizeof st);
    // ~50 ms; die ADC-DMA läuft weiter, schlimmstenfalls geht ein Block für das Display verloren
    flash_safe_execute(write_flash, NULL, 100);
}
