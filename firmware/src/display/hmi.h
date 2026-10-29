// Hintergrundbeleuchtung (GP5, PWM) und Touch (XPT2046 an SPI1, Meldeleitung GP14).
//
// Der Touch wird nur als Taste benutzt: T_IRQ geht auf Low, solange der Bildschirm
// berührt wird. Kurz tippen = Helligkeit, 2 s halten = Bild um 180° drehen.

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
