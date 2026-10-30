#include "check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const LABELS[CHK_MAX] = {"Signal",   "Puls in Ruhe", "Rhythmus",
                                            "Extraschläge", "P-Welle",  "PR-Zeit",
                                            "QRS-Breite", "QT-Zeit",    "HRV"};

static const char *const ABOUT[CHK_MAX] = {
    "Wie sauber das Signal war. Bewegung, trockene Haut und schlechter Kontakt stören.",
    "Herzschläge pro Minute in Ruhe. Üblich sind 50 bis 100.",
    "Ob die Schläge in gleichmäßigem Abstand kommen. Schwanken mit der Atmung ist normal.",
    "Vorzeitige Schläge mit anschließender Pause. Einzelne hat fast jeder.",
    "Der kleine Ausschlag vor jedem Schlag zeigt die Vorhofaktivität. Er ist oft sehr klein.",
    "Zeit vom Vorhof bis zur Kammer. Üblich sind 120 bis 200 ms.",
    "Dauer der Kammererregung, des großen Ausschlags. Üblich sind unter 110 ms.",
    "Erholungszeit der Herzkammer, auf den Puls umgerechnet (Fridericia).",
    "Wie stark die Abstände von Schlag zu Schlag schwanken.",
};

int check_quality(const ana_result_t *r, const char **text) {
    const char *dummy;
    if (!text) {
        text = &dummy;
    }
    if (r->has_mains_ratio && r->mains_ratio > 0.5) {
        *text = "starke 50-Hz-Störung";
        return 2;
    }
    if (r->masked_s > 10.0) {
        *text = "teils unbrauchbar, ausgeblendet";
        return 2;
    }
    if (r->has_mains_ratio && r->mains_ratio > 0.1) {
        *text = "leichte 50-Hz-Störung";
        return 1;
    }
    if (r->masked_s > 1.0) {
        *text = "kurze Störungen ausgeblendet";
        return 1;
    }
    *text = "gut";
    return 0;
}

static void set(check_item_t *it, chk_status_t st, const char *verdict, const char *advice) {
    it->status = st;
    it->verdict = verdict;
    it->advice = advice;
}

static void not_rated(check_item_t *it, const char *value, const char *verdict,
                      const char *advice) {
    if (value) {
        snprintf(it->value, sizeof it->value, "%s", value);
    }
    set(it, CHK_NA, verdict, advice);
}

static const char *const MINOR_VERDICT = "Erwachsenen-Werte";
static const char *const MINOR_ADVICE =
    "Die Grenzen gelten ab 18 Jahren. Bei Kindern und Jugendlichen sind andere Werte normal.";

static void fill_signal(check_item_t *it, const ana_result_t *r) {
    int q = check_quality(r, NULL);
    bool unsure = r->rhythm.valid && r->rhythm.n_err >= 3;
    if (q == 0 && !unsure) {
        snprintf(it->value, sizeof it->value, "gut");
        set(it, CHK_OK, "sauber", "Das Signal war sauber genug für die Auswertung.");
    } else if (q == 0) {
        snprintf(it->value, sizeof it->value, "Schläge unsicher");
        set(it, CHK_NOTE, "einzelne Schläge unsicher",
            "Einige Schläge wurden vermutlich verpasst oder doppelt erkannt. Elektroden prüfen, "
            "ruhig sitzen und bei Bedarf noch einmal messen.");
    } else if (q == 1) {
        snprintf(it->value, sizeof it->value, "leichte Störung");
        set(it, CHK_NOTE, "leicht gestört",
            "Leichte Störungen wurden herausgefiltert. Die Werte sind trotzdem brauchbar.");
    } else {
        snprintf(it->value, sizeof it->value, "starke Störung");
        set(it, CHK_NOTE, "stark gestört – neu messen",
            "Die übrigen Werte sind unsicher. Haut sauber und trocken, Elektroden prüfen, ruhig "
            "sitzen, nicht sprechen – dann noch einmal messen.");
    }
}

static void fill_pulse(check_item_t *it, const ana_result_t *r, bool minor) {
    double hr = r->has_hrv ? r->hrv.mean_hr : r->rhythm.valid ? r->rhythm.median_hr : NAN;
    if (!isfinite(hr)) {
        not_rated(it, "–", "zu wenige Schläge", "Es wurden zu wenige saubere Schläge erkannt.");
        return;
    }
    const long h = lrint(hr);
    snprintf(it->value, sizeof it->value, "%ld /min", h);
    if (minor) {
        set(it, CHK_NA, MINOR_VERDICT, MINOR_ADVICE);
    } else if (h < 40) {
        set(it, CHK_ALERT, "sehr niedrig",
            "Das ist sehr niedrig. Bei Schwindel, Schwäche oder Ohnmacht bald ärztlich abklären.");
    } else if (h < 50) {
        set(it, CHK_NOTE, "niedrig",
            "Etwas niedrig. Bei Sportlern und im Schlaf normal. Bei Schwindel oder Schwäche "
            "ärztlich ansehen lassen.");
    } else if (h <= 100) {
        set(it, CHK_OK, "im üblichen Bereich", "Der Puls liegt im üblichen Bereich für die Ruhe.");
    } else if (h <= 120) {
        set(it, CHK_NOTE, "erhöht",
            "Etwas erhöht. Aufregung, Kaffee, Fieber oder Bewegung davor treiben den Puls. Nach "
            "fünf Minuten Ruhe noch einmal messen.");
    } else {
        set(it, CHK_ALERT, "sehr hoch",
            "Sehr hoch für die Ruhe. Noch einmal in Ruhe messen. Bleibt es so oder gibt es "
            "Beschwerden, ärztlich abklären.");
    }
}

static bool mostly_irregular(const ana_rhythm_t *rh) {
    return rh->has_irr && rh->windows > 0 && 2 * rh->irregular_windows >= rh->windows;
}

static void fill_rhythm(check_item_t *it, const ana_rhythm_t *rh) {
    if (!rh->valid || !rh->has_irr) {
        not_rated(it, "–", "zu wenige Schläge", "Für eine Aussage zum Rhythmus waren es zu wenige Schläge.");
    } else if (mostly_irregular(rh)) {
        snprintf(it->value, sizeof it->value, "unregelmäßig");
        set(it, CHK_ALERT, "über weite Strecken unregelmäßig",
            "Die Abstände schwanken ohne erkennbares Muster. Eine mögliche Ursache ist "
            "Vorhofflimmern; das klärt nur eine ärztliche Untersuchung. Bei Wiederholung bald "
            "abklären.");
    } else if (rh->irregular_windows > 0) {
        snprintf(it->value, sizeof it->value, "zeitweise unruhig");
        set(it, CHK_NOTE, "zeitweise unregelmäßig",
            "Zeitweise ungleichmäßig. Das kommt auch durch Bewegung oder schlechten Kontakt vor. "
            "Noch einmal in Ruhe messen; bleibt es so, ärztlich ansprechen.");
    } else {
        snprintf(it->value, sizeof it->value, "gleichmäßig");
        set(it, CHK_OK, "gleichmäßig",
            rh->resp_variation ? "Die Abstände waren gleichmäßig. Das Schwanken mit der Atmung "
                                 "ist normal."
                               : "Die Abstände waren gleichmäßig.");
    }
}

static void fill_ectopy(check_item_t *it, const ana_result_t *r) {
    const ana_rhythm_t *rh = &r->rhythm;
    if (!rh->valid) {
        not_rated(it, "–", "zu wenige Schläge", "Für eine Aussage waren es zu wenige Schläge.");
        return;
    }
    if (mostly_irregular(rh)) {
        not_rated(it, "–", "nicht zählbar",
                  "Bei unregelmäßigem Rhythmus lassen sich vorzeitige Schläge nicht zählen.");
        return;
    }
    const int n = rh->n_ect;
    const bool reported = n >= 3 && (n >= 10 || n >= 0.01 * (r->n_beats - 1));
    const int pct = r->n_beats > 0 ? (int)lrint(100.0 * n / r->n_beats) : 0;
    if (n == 0) {
        snprintf(it->value, sizeof it->value, "keine");
        set(it, CHK_OK, "keine erkannt", "Es wurden keine vorzeitigen Schläge erkannt.");
    } else if (!reported) {
        snprintf(it->value, sizeof it->value, "vereinzelt (%d)", n);
        set(it, CHK_OK, "vereinzelt",
            "Einzelne vorzeitige Schläge sind häufig und meist harmlos.");
    } else if (pct >= 10) {
        snprintf(it->value, sizeof it->value, "%d (%d %%)", n, pct);
        set(it, CHK_ALERT, "viele",
            "Mehr als jeder zehnte Schlag kam zu früh. Bitte ärztlich abklären lassen.");
    } else {
        snprintf(it->value, sizeof it->value, "%d (%d %%)", n, pct);
        set(it, CHK_NOTE, "mehrere",
            "Mehrere vorzeitige Schläge. Oft harmlos (Stress, Koffein, Schlafmangel). Bei Herz"
            "stolpern oder häufigem Auftreten ärztlich ansehen lassen.");
    }
}

static void fill_pwave(check_item_t *it, const ana_result_t *r) {
    if (!r->has_delineation) {
        not_rated(it, "–", "nicht bestimmbar", "Die Schlagform war nicht sicher auswertbar.");
    } else if (r->d.p_onset >= 0) {
        snprintf(it->value, sizeof it->value, "erkannt");
        set(it, CHK_OK, "erkannt", "Vor dem Schlag ist eine P-Welle zu erkennen.");
    } else {
        not_rated(it, "nicht erkennbar", "nicht erkennbar",
                  "Mit einer Ableitung ist die P-Welle oft zu klein. Das allein ist kein Befund.");
    }
}

static void fill_pr(check_item_t *it, const ana_result_t *r, bool minor) {
    const double v = r->has_delineation ? r->d.pr_ms : NAN;
    if (!isfinite(v) || v < 60.0 || v > 400.0) {
        not_rated(it, "–", "nicht bestimmbar", "Die PR-Zeit ließ sich nicht sicher bestimmen.");
        return;
    }
    const long ms = lrint(v);
    snprintf(it->value, sizeof it->value, "%ld ms", ms);
    if (minor) {
        set(it, CHK_NA, MINOR_VERDICT, MINOR_ADVICE);
    } else if (ms < 120) {
        set(it, CHK_NOTE, "kurz",
            "Etwas kurz. Meist ohne Bedeutung; mit einer Ableitung ist der Wert nur eine Näherung.");
    } else if (ms <= 200) {
        set(it, CHK_OK, "im üblichen Bereich", "Die PR-Zeit liegt im üblichen Bereich.");
    } else {
        set(it, CHK_NOTE, "verlängert",
            "Etwas lang. Das ist oft harmlos (ein AV-Block 1. Grades ist möglich). Bei der "
            "nächsten ärztlichen Untersuchung erwähnen.");
    }
}

static void fill_qrs(check_item_t *it, const ana_result_t *r, bool minor) {
    const double v = r->has_delineation ? r->d.qrs_ms : NAN;
    if (!isfinite(v) || v < 40.0 || v > 220.0) {
        not_rated(it, "–", "nicht bestimmbar", "Die QRS-Breite ließ sich nicht sicher bestimmen.");
        return;
    }
    const long ms = lrint(v);
    snprintf(it->value, sizeof it->value, "%ld ms", ms);
    if (minor) {
        set(it, CHK_NA, MINOR_VERDICT, MINOR_ADVICE);
    } else if (ms < 110) {
        set(it, CHK_OK, "im üblichen Bereich", "Die QRS-Breite liegt im üblichen Bereich.");
    } else if (ms < 120) {
        set(it, CHK_NOTE, "grenzwertig",
            "Grenzwertig breit. Mit einer Ableitung wird die Breite oft überschätzt.");
    } else {
        set(it, CHK_ALERT, "breit",
            "Ab 120 ms gilt der Ausschlag als breit (zum Beispiel bei einem Schenkelblock). Mit "
            "einer Ableitung wird er oft überschätzt. Ärztlich abklären lassen.");
    }
}

static void fill_qtc(check_item_t *it, const ana_result_t *r, bool minor, int sex) {
    const double v = r->has_delineation ? r->d.qtc_fridericia_ms : NAN;
    if (!isfinite(v) || v < 250.0 || v > 700.0) {
        not_rated(it, "–", "nicht bestimmbar", "Die QT-Zeit ließ sich nicht sicher bestimmen.");
        return;
    }
    const long ms = lrint(v);
    snprintf(it->value, sizeof it->value, "%ld ms", ms);
    const long limit = sex == SEX_FEMALE ? 460 : 450;
    if (minor) {
        set(it, CHK_NA, MINOR_VERDICT, MINOR_ADVICE);
    } else if (ms >= 500) {
        set(it, CHK_ALERT, "deutlich verlängert",
            "Ab 500 ms steigt das Risiko für Rhythmusstörungen. Bald ärztlich abklären; auch "
            "Medikamente prüfen lassen.");
    } else if (ms >= limit) {
        if (sex == SEX_UNKNOWN && ms < 460) {
            set(it, CHK_NOTE, "grenzwertig",
                "Für Männer leicht verlängert, für Frauen noch im Bereich (bis 460 ms). Das "
                "Geschlecht im Profil macht die Bewertung genauer.");
        } else {
            set(it, CHK_NOTE, "leicht verlängert",
                "Leicht verlängert. Einige Medikamente verlängern die QT-Zeit; bei der nächsten "
                "ärztlichen Untersuchung erwähnen.");
        }
    } else if (ms < 340) {
        set(it, CHK_NOTE, "kurz",
            "Eher kurz. Meist ohne Bedeutung; mit einer Ableitung ist der Wert unsicher.");
    } else {
        set(it, CHK_OK, "im üblichen Bereich",
            sex == SEX_UNKNOWN
                ? "Im üblichen Bereich (unter 450 ms; Frauen bis 460 ms)."
                : sex == SEX_FEMALE ? "Im üblichen Bereich (bei Frauen unter 460 ms)."
                                    : "Im üblichen Bereich (bei Männern unter 450 ms).");
    }
}

static void fill_hrv(check_item_t *it, const ana_result_t *r) {
    if (!r->has_hrv || !isfinite(r->hrv.rmssd_ms)) {
        not_rated(it, "–", "zu wenige Schläge", "Für die HRV waren es zu wenige saubere Schläge.");
        return;
    }
    snprintf(it->value, sizeof it->value, "RMSSD %ld ms", lrint(r->hrv.rmssd_ms));
    set(it, CHK_INFO, "nur zur Information",
        "Gut zum Vergleich über Wochen, nicht als Urteil. Normwerte stammen aus 5-Minuten-"
        "Messungen und passen auf 2 Minuten nur grob. Typisch für Erwachsene in Ruhe: etwa 20 "
        "bis 75 ms.");
}

int check_build(const ana_result_t *r, const profile_t *p, bool test, check_item_t *out,
                int max) {
    if (max < CHK_MAX) {
        return 0;
    }
    const bool minor = p && p->age > 0 && p->age < 18;
    const int sex = p ? p->sex : SEX_UNKNOWN;
    memset(out, 0, sizeof *out * CHK_MAX);
    for (int i = 0; i < CHK_MAX; i++) {
        out[i].id = (chk_id_t)i;
        out[i].label = LABELS[i];
        out[i].about = ABOUT[i];
        snprintf(out[i].value, sizeof out[i].value, "–");
        set(&out[i], CHK_NA, "nicht bewertet", "");
    }
    fill_signal(&out[CHK_SIGNAL], r);
    fill_pulse(&out[CHK_PULSE], r, minor);
    fill_rhythm(&out[CHK_RHYTHM], &r->rhythm);
    fill_ectopy(&out[CHK_ECTOPY], r);
    fill_pwave(&out[CHK_P], r);
    fill_pr(&out[CHK_PR], r, minor);
    fill_qrs(&out[CHK_QRS], r, minor);
    fill_qtc(&out[CHK_QTC], r, minor, sex);
    fill_hrv(&out[CHK_HRV], r);
    if (test) {
        for (int i = 0; i < CHK_MAX; i++) {
            out[i].status = CHK_NA;
            out[i].verdict = "Testsignal";
            out[i].advice = "Das Gerät zeigte ein Testsignal – das ist keine echte Messung.";
        }
    }
    return CHK_MAX;
}

check_summary_t check_summarize(const check_item_t *items, int n) {
    check_summary_t s = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        switch (items[i].status) {
            case CHK_OK:
                s.ok++;
                break;
            case CHK_NOTE:
                s.note++;
                break;
            case CHK_ALERT:
                s.alert++;
                break;
            default:
                break;
        }
    }
    s.rated = s.ok + s.note + s.alert;
    return s;
}

void check_summary_text(const check_summary_t *s, char *buf, size_t n) {
    if (s->rated < 3) {
        snprintf(buf, n, "Zu wenig für einen Check");
    } else if (s->alert > 0 && s->note > 0) {
        snprintf(buf, n, "%d %s ärztlich abklären, %d beachten", s->alert,
                 s->alert == 1 ? "Punkt" : "Punkte", s->note);
    } else if (s->alert > 0) {
        snprintf(buf, n, "%d %s ärztlich abklären", s->alert, s->alert == 1 ? "Punkt" : "Punkte");
    } else if (s->note > 0) {
        snprintf(buf, n, "%d %s beachten", s->note, s->note == 1 ? "Punkt" : "Punkte");
    } else {
        snprintf(buf, n, "Alles im üblichen Bereich");
    }
}
