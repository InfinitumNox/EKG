#include "settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#define SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SAVE_DELAY_MS 3000u

static volatile bool pending;
static volatile uint32_t requested_at;
static ui_settings_t wanted;
static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

void settings_load(ui_settings_t *s) {
    settings_decode((const void *)(XIP_BASE + SETTINGS_OFFSET), s);
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
    settings_encode(&wanted, page, sizeof page);
    // ~50 ms; die ADC-DMA läuft weiter, schlimmstenfalls geht ein Block für das Display verloren
    flash_safe_execute(write_flash, NULL, 100);
}
