// Hintergrundbeleuchtung (GP5, PWM) und Touch (XPT2046 an SPI1, Meldeleitung GP14).
//
// T_IRQ geht auf Low, solange der Bildschirm berührt wird; dann liefert der XPT2046
// Druck und Rohkoordinaten (12 Bit). Ohne angeschlossenes T_DO bleiben die Werte am
// Anschlag; der Touch taugt dann nur als Taste.

#pragma once

#include <stdbool.h>

#define HMI_PIN_LED 5u
#define HMI_PIN_T_CLK 10u
#define HMI_PIN_T_DIN 11u
#define HMI_PIN_T_DO 12u
#define HMI_PIN_T_CS 13u
#define HMI_PIN_T_IRQ 14u

void hmi_init(void);
void backlight_set(int permille);
bool touch_pressed(void);

typedef struct {
    bool pressed;  // T_IRQ aktiv
    bool valid;    // Koordinaten plausibel und ruhig
    int rx, ry;    // Rohwerte (Median aus 5 Wandlungen)
    int z;         // Druckmaß, ab ~300 sicher berührt
} touch_sample_t;

void touch_read(touch_sample_t *t);
