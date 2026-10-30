// Lernmodell der Solo-Session. Bewusst einfach und nachvollziehbar: Der Puls steigt von einem
// Ruhewert am Start auf einen Höchstwert; das Modell merkt sich, wie hoch dieser Anstieg und wie
// lang die Session bei den letzten gewerteten Sessions typisch war. Die Anzeige vergleicht den
// aktuellen Puls mit dem eigenen typischen Anstieg. Ohne Hardware-Bezug.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "private_data.h"

#define SOLO_MIN_SESSIONS 3  // so viele gewertete Sessions, bevor es eine Anzeige gibt
#define SOLO_MODEL_SESSIONS 8
#define SOLO_MAX_S 2400      // längste Session (40 min)

typedef struct {
    int n;             // gewertete Sessions im Modell
    bool learned;      // n >= SOLO_MIN_SESSIONS
    bool confident;    // gelernt, und die Anstiege liegen eng beieinander
    float gain;        // typischer Anstieg (Median von Höchst- minus Startpuls)
    float peak;        // typischer Höchstpuls
    int t_lo, t_med, t_hi;  // Dauer in s: unteres Viertel, Median, oberes Viertel
} solo_model_t;

void solo_build(const priv_data_t *d, solo_model_t *m);
// Annäherung in Prozent (0 bis 100) oder -1, wenn das Modell dafür nicht sicher genug ist
int solo_percent(const solo_model_t *m, float start, float now);

// Auswertung einer Session aus dem Puls je Sekunde (0 = Lücke). false: zu wenige Werte.
bool solo_summarize(const uint8_t *hr, int n_s, priv_session_t *out);
// Ruhewert: Median der ersten 30 gültigen Werte; NAN ohne Werte
float solo_start(const uint8_t *hr, int n_s);
// Median der 5 Werte bis einschließlich Index end (mindestens 3 gültig), sonst NAN
float solo_median5(const uint8_t *hr, int n_s, int end);
// Sekunde, ab der 90 % des Anstiegs erreicht waren, -1 wenn nie
int solo_t90(const uint8_t *hr, int n_s, float start, float gain);
