#include "feed.h"

#include <string.h>

#include "hardware/sync.h"

static feed_block_t ring[FEED_BLOCKS];
static volatile uint32_t head;  // nur Kern 0 schreibt
static volatile uint32_t tail;  // nur Kern 1 schreibt

link_state_t g_link;

static uint32_t seq;

bool feed_push(const uint32_t *raw, uint8_t n, uint8_t flags, uint16_t factor) {
    uint32_t s = seq++;
    if (head - tail >= FEED_BLOCKS) {
        return false;
    }
    feed_block_t *b = &ring[head % FEED_BLOCKS];
    b->seq = s;
    if (n > FEED_MAX_SAMPLES) {
        n = FEED_MAX_SAMPLES;
    }
    memcpy(b->raw, raw, n * sizeof(uint32_t));
    b->n = n;
    b->flags = flags;
    b->factor = factor;
    __dmb();  // Daten vor dem Index sichtbar machen
    head = head + 1;
    return true;
}

bool feed_pop(feed_block_t *out) {
    if (tail == head) {
        return false;
    }
    __dmb();
    *out = ring[tail % FEED_BLOCKS];
    __dmb();
    tail = tail + 1;
    return true;
}
