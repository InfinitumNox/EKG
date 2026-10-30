// Bildschirmtastatur für Namen (QWERTZ mit Umlauten) und Zahlen, quer und hochkant.
// Die Tasten liegen lückenlos nebeneinander (Treffer immer auf die nächste Taste); gezeichnet
// werden sie mit Abstand. Ohne Hardware-Bezug.

#pragma once

#include <stdbool.h>

#include "gfx.h"
#include "profile.h"

typedef enum { KB_TEXT, KB_NUMBER } kb_mode_t;
typedef enum { KB_NONE, KB_OK, KB_CANCEL } kb_result_t;

typedef struct {
    kb_mode_t mode;
    bool shift;                    // nächster Buchstabe groß
    char buf[PROFILE_NAME_BYTES];  // Text (UTF-8) oder Ziffern
    int max_chars;                 // Text: Zeichen
    int min_val, max_val;          // Zahl; leer = 0 = „keine Angabe“
    const char *title;
    const char *unit;
} kb_t;

void kb_open_text(kb_t *k, const char *title, const char *initial, int max_chars);
void kb_open_number(kb_t *k, const char *title, const char *unit, int initial, int min_val,
                    int max_val);
void kb_draw(const kb_t *k, canvas_t *c);
// x, y im Bild mit den Maßen des Zeichenbereichs c->w × c->h
kb_result_t kb_tap(kb_t *k, int w, int h, int x, int y);
int kb_value(const kb_t *k);  // Zahl, 0 wenn leer
bool kb_can_ok(const kb_t *k);
// Mitte einer Taste (für Tests und die Vorschau): label ist das Zeichen in Kleinbuchstaben,
// "Leer", "OK" oder "<" (löschen), "^" (Umschalter)
bool kb_key_center(const kb_t *k, int w, int h, const char *label, int *x, int *y);
