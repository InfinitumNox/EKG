#include "private_store.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/rand.h"

#define PRIV_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)
#define SAVE_DELAY_MS 2000u

static volatile bool pending;
static volatile uint32_t requested_at;
static priv_data_t wanted;
static uint8_t buf[PRIV_RECORD_BYTES] __attribute__((aligned(4)));

void priv_load(priv_data_t *d) {
    priv_decode((const void *)(XIP_BASE + PRIV_OFFSET), d);
}

void priv_request_save(const priv_data_t *d, uint32_t now_ms) {
    wanted = *d;
    requested_at = now_ms;
    __dmb();
    pending = true;
}

static void write_flash(void *param) {
    (void)param;
    flash_range_erase(PRIV_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PRIV_OFFSET, buf, PRIV_RECORD_BYTES);
}

void priv_service(uint32_t now_ms) {
    if (!pending || now_ms - requested_at < SAVE_DELAY_MS) {
        return;
    }
    pending = false;
    __dmb();
    if (priv_encode(&wanted, buf, sizeof buf)) {
        flash_safe_execute(write_flash, NULL, 100);
    }
}

void priv_random_bytes(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        uint32_t r = get_rand_32();
        memcpy(out + i, &r, n - i < 4 ? n - i : 4);
    }
}
