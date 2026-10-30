// Körperfigur (Vorderansicht) für die Elektroden-Anleitung: angepasst an Geschlecht, Größe und
// Gewicht des Profils oder als Standardfigur (normalgewichtig, ohne Profil neutral).
// Ohne Hardware-Bezug.

#pragma once

#include <stdint.h>

#include "gfx.h"
#include "profile.h"

enum { AVATAR_PROFILE = 0, AVATAR_STANDARD = 1 };

typedef struct {
    uint8_t sex;     // SEX_*; SEX_UNKNOWN = neutral
    int height_cm;
    int bmi10;       // Body-Mass-Index mal 10
} avatar_t;

typedef struct {
    float x, y;
} avatar_pt_t;

// Lage der drei Elektroden im Bild (Mitte) und Radius der Markierung
typedef struct {
    avatar_pt_t ra, la, rl;
    float r;
} avatar_sites_t;

// p = NULL: Gast. mode: AVATAR_PROFILE (Größe und Gewicht aus dem Profil) oder AVATAR_STANDARD.
avatar_t avatar_for(const profile_t *p, int mode);
// Zeichnet die Figur in die Box (Füße unten, mittig); die Höhe folgt der Körpergröße (190 cm
// füllen die Box), die Breite dem BMI. sites darf NULL sein.
void avatar_draw(canvas_t *c, int bx, int by, int bw, int bh, const avatar_t *av,
                 avatar_sites_t *sites);
