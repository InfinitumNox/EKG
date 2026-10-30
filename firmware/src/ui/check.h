// Check-Liste für die Auswertung: bewertet die Messwerte gegen übliche Bereiche und erklärt
// jeden Punkt in Alltagssprache. Ohne Hardware-Bezug.
//
// Keine Diagnose: Eine Ableitung mit 0,5–40 Hz liefert Näherungen. Die Grenzen sind die
// üblichen für Erwachsene in Ruhe; bei Alter unter 18 werden Puls und Zeiten nicht bewertet.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ana.h"
#include "profile.h"

#define CHK_MAX 9

typedef enum {
    CHK_OK,     // im üblichen Bereich
    CHK_NOTE,   // beachten
    CHK_ALERT,  // ärztlich abklären
    CHK_INFO,   // nur zur Information (keine Bewertung)
    CHK_NA,     // nicht bewertbar
} chk_status_t;

typedef enum {
    CHK_SIGNAL,
    CHK_PULSE,
    CHK_RHYTHM,
    CHK_ECTOPY,
    CHK_P,
    CHK_PR,
    CHK_QRS,
    CHK_QTC,
    CHK_HRV,
} chk_id_t;

typedef struct {
    chk_id_t id;
    const char *label;
    char value[32];  // z. B. „72 /min“
    chk_status_t status;
    const char *verdict;  // Überschrift der Erklärung
    const char *advice;   // Einordnung für diese Messung
    const char *about;    // was der Wert bedeutet
} check_item_t;

typedef struct {
    int rated;  // bewertete Punkte (ok + note + alert)
    int ok, note, alert;
} check_summary_t;

// p = NULL: Gast (Erwachsenen-Grenzen, Geschlecht unbekannt); test: Testsignal, nichts bewertbar.
// Liefert CHK_MAX Punkte (0, wenn max zu klein ist).
int check_build(const ana_result_t *r, const profile_t *p, bool test, check_item_t *out, int max);
check_summary_t check_summarize(const check_item_t *items, int n);
void check_summary_text(const check_summary_t *s, char *buf, size_t n);
// Signalgüte: 0 gut, 1 leicht gestört, 2 stark gestört; text beschreibt kurz
int check_quality(const ana_result_t *r, const char **text);
