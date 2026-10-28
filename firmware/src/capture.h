// ADC-Erfassung: ADC0 freilaufend mit fester Rate, zwei DMA-Kanäle im Ping-Pong.
// Der Takt kommt aus dem 12-MHz-Quarz (ADC-Takt 48 MHz), die Abtastung ist damit
// unabhängig von CPU-Last und USB.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EKG_ADC_RATE_HZ 32000u
#define EKG_ADC_BLOCK 1600u  // 50 ms
#define EKG_ADC_GPIO 26u

void capture_init(void);
void capture_start(void);
void capture_stop(void);

// Liefert den nächsten vollständigen Block oder NULL, wenn keiner bereit ist.
// block_no zählt seit capture_start; lost ist die Zahl der übersprungenen Blöcke, wenn
// die Verarbeitung nicht hinterherkam. Der Block muss vor Ablauf von 50 ms verarbeitet sein.
const uint16_t *capture_next_block(uint32_t *block_no, uint32_t *lost);

uint32_t capture_fifo_overflows(void);
