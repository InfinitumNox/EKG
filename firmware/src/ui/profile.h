// Angaben zur Person für die Bewertung: zwei Profile und „Gast“ (ohne Angaben).
// Ohne Hardware-Bezug.

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define PROFILE_COUNT 2
#define PERSON_GUEST PROFILE_COUNT  // aktive Person ohne Profil
#define PROFILE_NAME_CHARS 12
#define PROFILE_NAME_BYTES 28  // 12 Zeichen mit Umlauten (2 Byte) und Endzeichen

#define PROFILE_AGE_MAX 120
#define PROFILE_HEIGHT_MIN 50
#define PROFILE_HEIGHT_MAX 250
#define PROFILE_WEIGHT_MIN 2
#define PROFILE_WEIGHT_MAX 250

enum { SEX_UNKNOWN = 0, SEX_MALE = 1, SEX_FEMALE = 2 };

typedef struct {
    char name[PROFILE_NAME_BYTES];  // UTF-8, leer = Profil nicht angelegt
    uint8_t age;                    // Jahre, 0 = keine Angabe
    uint8_t sex;                    // SEX_*
    uint8_t height_cm;              // 0 = keine Angabe
    uint8_t weight_kg;              // 0 = keine Angabe
} profile_t;

bool profile_used(const profile_t *p);
void profile_clear(profile_t *p);
// nach dem Laden aus dem Flash: Endzeichen setzen, Werte außerhalb des Bereichs verwerfen
void profile_sanitize(profile_t *p);
// Body-Mass-Index mal 10 (245 = 24,5), -1 ohne Größe oder Gewicht
int profile_bmi10(const profile_t *p);
// Kurzzeile wie „41 J. · m · 182 cm · 78 kg“
void profile_summary(const profile_t *p, char *buf, size_t n);
