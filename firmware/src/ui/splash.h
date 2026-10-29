// Startbildschirm „SophiEKG“: Herz schlägt, Schriftzug blendet ein, eine EKG-Linie
// zeichnet sich von links nach rechts.

#pragma once

#include <stdint.h>

#include "gfx.h"

#define SPLASH_DURATION_MS 3400u

void splash_render(canvas_t *c, uint32_t t_ms, const char *version);
// Helligkeit der Hintergrundbeleuchtung in Promille (sanftes Einblenden)
int splash_backlight(uint32_t t_ms);
