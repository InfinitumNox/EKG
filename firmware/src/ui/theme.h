// Farben und Maße der Oberfläche (320 × 240, quer).
//
// Zwei Farbschemata: „Nacht“ (dunkel, leuchtende Kurve) und „Tag“ (hell wie EKG-Papier).
// Ein TFT leuchtet nur mit seiner Hintergrundbeleuchtung; bei Tageslicht wirkt es deshalb
// nur mit hellem Grund hell genug.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 240

typedef struct {
    uint32_t bg;  // Hintergrund
    uint32_t panel;  // Kopf- und Fußleiste
    uint32_t panel_edge;
    uint32_t grid;
    uint32_t trace;  // Kurve
    uint32_t trace_glow;
    uint32_t head;  // Spitze der laufenden Kurve
    uint32_t text;
    uint32_t text_dim;
    uint32_t text_faint;
    uint32_t accent;  // Herz, „EKG“
    uint32_t accent_2;  // Verlauf im Schriftzug
    uint32_t warn;
    uint32_t ok;
    uint32_t splash_top;
    uint32_t splash_bottom;
    bool glow;  // Leuchteffekte (nur auf dunklem Grund)
} theme_t;

extern const theme_t THEME_NIGHT;
extern const theme_t THEME_DAY;
extern const theme_t *g_theme;  // aktuelles Schema, nur Kern 1 ändert es

#define C_BG (g_theme->bg)
#define C_PANEL (g_theme->panel)
#define C_PANEL_EDGE (g_theme->panel_edge)
#define C_GRID (g_theme->grid)
#define C_TRACE (g_theme->trace)
#define C_TRACE_GLOW (g_theme->trace_glow)
#define C_HEAD (g_theme->head)
#define C_TEXT (g_theme->text)
#define C_TEXT_DIM (g_theme->text_dim)
#define C_TEXT_FAINT (g_theme->text_faint)
#define C_ACCENT (g_theme->accent)
#define C_ACCENT_2 (g_theme->accent_2)
#define C_WARN (g_theme->warn)
#define C_OK (g_theme->ok)
#define SPLASH_TOP (g_theme->splash_top)
#define SPLASH_BOTTOM (g_theme->splash_bottom)
#define GLOW (g_theme->glow)
