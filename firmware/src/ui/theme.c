#include "theme.h"

const theme_t THEME_NIGHT = {
    .bg = 0x0A0F1Eu,
    .panel = 0x111933u,
    .panel_edge = 0x1E2A4Au,
    .grid = 0x1A2848u,
    .trace = 0x3CF2A6u,  // Mint
    .trace_glow = 0x1D7E5Cu,
    .head = 0xEFFFF7u,
    .text = 0xF4F6FBu,
    .text_dim = 0x8C96B8u,
    .text_faint = 0x55607Fu,
    .accent = 0xFF5C7Cu,
    .accent_2 = 0xFF9466u,
    .warn = 0xFFB547u,
    .ok = 0x3CF2A6u,
    .splash_top = 0x0B1024u,
    .splash_bottom = 0x1C1340u,
    .glow = true,
};

// Dunkle Kurve auf weißem Papier mit rosa Raster; Farben dunkler als nachts, damit sie auf
// Weiß genug Kontrast haben.
const theme_t THEME_DAY = {
    .bg = 0xFFFBF9u,
    .panel = 0xEDF0F5u,
    .panel_edge = 0xD2D7E3u,
    .grid = 0xF0A3B2u,
    .trace = 0x0E1326u,
    .trace_glow = 0x0E1326u,
    .head = 0x0E1326u,
    .text = 0x0E1326u,
    .text_dim = 0x4A5370u,
    .text_faint = 0x8A91A8u,
    .accent = 0xE0365Au,
    .accent_2 = 0xF0683Cu,
    .warn = 0xB86A00u,
    .ok = 0x08966Au,
    .splash_top = 0xFFFFFFu,
    .splash_bottom = 0xEDE8F6u,
    .glow = false,
};

const theme_t *g_theme = &THEME_NIGHT;
