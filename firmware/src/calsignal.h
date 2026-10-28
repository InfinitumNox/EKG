// Kalibriersignal an GP15 (PWM, 73 kHz Träger, ±0,5 V um 1,65 V). Über zwei RC-Glieder und
// einen ~1000:1-Teiler an die AD8232-Eingänge gelegt, ergibt sich knapp 1 mV Spitze-Spitze
// (Schaltung: docs/KALIBRIERUNG.md). Nur ohne Menschen am Gerät verwenden.
#pragma once

#include <stdint.h>

#define EKG_CAL_GPIO 15u

// wave: ekg_cal_wave; freq_mhz: Frequenz in Millihertz.
void cal_set(uint8_t wave, uint32_t freq_mhz);
uint8_t cal_wave(void);
