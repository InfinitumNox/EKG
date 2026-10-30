// Host-Tests der Bedienung: Touch-Kalibrierung, Gesten, Batterieanzeige, Speicherformat,
// Tastatur, Check-Liste und der Ablauf Menü -> Messung -> Auswertung mit einem künstlichen EKG.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "check.h"
#include "gesture.h"
#include "keyboard.h"
#include "settings.h"
#include "protocol.h"
#include "theme.h"
#include "touchcal.h"

static int failures;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                         \
        }                                                                       \
    } while (0)

// Panel mit vertauschten Achsen: Rohwert x folgt der Bildschirm-y-Richtung (gespiegelt)
static void panel(double sx, double sy, double *rx, double *ry) {
    *rx = 300.0 + 14.2 * (239.0 - sy) + 0.3 * sx;
    *ry = 260.0 + 11.1 * sx - 0.2 * sy;
}

static void test_touchcal(void) {
    const float tx[5] = {30, 290, 290, 30, 160}, ty[5] = {30, 30, 210, 210, 120};
    float rx[5], ry[5];
    for (int i = 0; i < 5; i++) {
        double a, b;
        panel(tx[i], ty[i], &a, &b);
        rx[i] = (float)(a + ((i % 2) ? 6.0 : -6.0));  // Messrauschen
        ry[i] = (float)(b + ((i % 3) ? -5.0 : 5.0));
    }
    touch_cal_t cal;
    float err = 0;
    CHECK(touchcal_solve(tx, ty, rx, ry, 5, &cal, &err));
    CHECK(err < 3.0f);
    for (int k = 0; k < 20; k++) {
        double sx = 10 + k * 15, sy = 12 + k * 11, a, b;
        panel(sx, sy, &a, &b);
        int x, y;
        touchcal_apply(&cal, (int)lround(a), (int)lround(b), &x, &y);
        CHECK(abs(x - (int)sx) <= 2 && abs(y - (int)sy) <= 2);
    }
    // entartet (alle Punkte gleich): keine Lösung
    float same[5] = {100, 100, 100, 100, 100};
    CHECK(!touchcal_solve(tx, ty, same, same, 5, &cal, &err));
}

static ui_event_t feed(gesture_t *g, bool p, int x, int y, uint32_t t) {
    return gesture_feed(g, p, p, x, y, x * 10, y * 10, t);
}

static void test_gestures(void) {
    gesture_t g;
    gesture_init(&g);
    ui_event_t ev;
    // Tippen: erste drei Positionen gemittelt, ein einzelner Aussetzer zählt nicht
    CHECK(feed(&g, true, 100, 50, 0).type == EV_NONE);
    feed(&g, true, 104, 52, 20);
    feed(&g, false, 0, 0, 40);
    feed(&g, true, 102, 54, 60);
    feed(&g, true, 130, 80, 80);
    feed(&g, false, 0, 0, 100);
    ev = feed(&g, false, 0, 0, 120);
    CHECK(ev.type == EV_NONE);  // 28 px bewegt: kein Tippen
    feed(&g, true, 100, 50, 200);
    feed(&g, true, 102, 52, 220);
    feed(&g, true, 101, 51, 240);
    feed(&g, false, 0, 0, 260);
    ev = feed(&g, false, 0, 0, 280);
    CHECK(ev.type == EV_TAP && ev.x == 101 && ev.y == 51 && ev.has_pos);
    // Halten: genau ein EV_LONG, danach kein Tippen
    int longs = 0;
    for (uint32_t t = 1000; t <= 2600; t += 20) {
        longs += feed(&g, true, 50, 50, t).type == EV_LONG;
    }
    CHECK(longs == 1);
    feed(&g, false, 0, 0, 2620);
    CHECK(feed(&g, false, 0, 0, 2640).type == EV_NONE);
    // Wischen nach links
    for (int k = 0; k < 8; k++) {
        feed(&g, true, 220 - k * 18, 120 + k, 3000 + (uint32_t)k * 20);
    }
    feed(&g, false, 0, 0, 3200);
    CHECK(feed(&g, false, 0, 0, 3220).type == EV_SWIPE_LEFT);
}

static void test_battery(void) {
    CHECK(app_battery_percent(4400) == 100);
    CHECK(app_battery_percent(3900) == 73);
    CHECK(app_battery_percent(3600) == 40);
    CHECK(app_battery_percent(3000) == 0);
    CHECK(app_battery_percent(2400) == -1);  // kein Messteiler
    CHECK(app_battery_percent(6000) == -1);
}

// ------------------------------------------------------------------ Ablauf
static uint16_t fb[SCREEN_W * SCREEN_H];
static monitor_t mon;
static ana_recording_t rec;
static ana_result_t res;
static ui_settings_t ui;
static app_t app;

static double ecg_mv(double t, double hr) {
    double rr = 60.0 / hr, ph = fmod(t, rr) - 0.5 * rr;
    double v = 1.1 * exp(-0.5 * pow(ph / 0.012, 2)) - 0.15 * exp(-0.5 * pow((ph - 0.03) / 0.01, 2));
    v += 0.12 * exp(-0.5 * pow((ph + 0.16) / 0.025, 2)) + 0.3 * exp(-0.5 * pow((ph - 0.25) / 0.05, 2));
    return v;
}

static uint32_t mv_to_raw(double mv, int oversampling) {
    double v = mv / 1000.0 * 1100.0 + 1.65;
    return (uint32_t)lround(v / (3.3 / 4096.0 / oversampling));
}

static void run_blocks(double t0, double t1, double hr, int rate, uint32_t *seq, int skip_every) {
    const int n = rate / 20, os = 32000 / rate;
    uint32_t raw[64];
    for (double t = t0; t < t1 - 1e-9; t += 0.05) {
        for (int i = 0; i < n; i++) {
            raw[i] = mv_to_raw(ecg_mv(t + (double)i / rate, hr), os);
        }
        uint32_t s = (*seq)++;
        if (skip_every && s % (uint32_t)skip_every == (uint32_t)skip_every - 1) {
            continue;  // Block geht verloren
        }
        uint32_t now = (uint32_t)lround(t * 1000.0);
        app_feed(&app, raw, n, 0, (uint32_t)os, s);
        rect_t d[16];
        app_frame(&app, now, d, 16);
    }
}

static canvas_t canvas;
static uint8_t start_rotation;

static void setup_app(void) {
    memset(&app, 0, sizeof app);
    memset(&ui, 0, sizeof ui);
    canvas = (canvas_t){fb, SCREEN_W, SCREEN_H};
    canvas_t *c = &canvas;
    monitor_init(&mon, c);
    ui.has_cal = true;
    ui.look = 1;
    ui.speed = MON_SPEED_25;
    ui.rotation = start_rotation;
    ui.person = PERSON_GUEST;
    app.c = c;
    app.mon = &mon;
    app.rec = &rec;
    app.res = &res;
    app.ui = &ui;
    app.scratch = fb;
    app.scratch_size = sizeof fb;
    app.version = "test";
    app_init(&app);
    app_set_status(&app, false, false);
    app_vsys_sample(&app, 4300, false);
    app_start(&app, true, 0);
}

static void tap(int x, int y, uint32_t now) {
    ui_event_t ev = {EV_TAP, x, y, 0, 0, true};
    app_event(&app, ev, now);
}

static void test_flow(int rate, int skip_every) {
    setup_app();
    uint32_t seq = 0;
    CHECK(app.screen == APP_LIVE);
    run_blocks(0.0, 3.0, 72.0, rate, &seq, 0);
    tap(160, 120, 3000);  // Menü öffnen
    CHECK(app.screen == APP_MENU);
    tap(160, 69, 3100);  // Messen & auswerten
    CHECK(app.measuring && app.screen == APP_LIVE);
    const double end = 3.0 + ANA_MAX_S + 1.0;
    run_blocks(3.0, end, 72.0, rate, &seq, skip_every);
    // nach 2 min voll: erst „wird ausgewertet“, im nächsten Bild die Auswertung
    CHECK(!app.measuring);
    run_blocks(end, end + 1.0, 72.0, rate, &seq, 0);
    CHECK(app.screen == APP_RESULT);
    CHECK(app.has_result && app.result_status == ANA_OK);
    CHECK(res.ok && res.has_hrv);
    CHECK(fabs(res.hrv.mean_hr - 72.0) < 1.0);
    CHECK(res.n_beats > (skip_every ? 128 : 138) && res.n_beats < 147);  // 72/min über 2 min
    CHECK(res.has_median && res.has_delineation);
    CHECK(app.n_checks == CHK_MAX && app.meas_known == false);
    if (!skip_every) {  // gleichmäßiger Puls 72/min ohne Störung
        CHECK(app.checks[CHK_PULSE].status == CHK_OK);
        CHECK(app.checks[CHK_RHYTHM].status == CHK_OK);
        CHECK(app.checks[CHK_ECTOPY].status == CHK_OK);
        CHECK(app.checks[CHK_SIGNAL].status == CHK_OK);
        CHECK(app.checks[CHK_HRV].status == CHK_INFO);
    }
    if (skip_every) {
        CHECK(res.gap_s > 1.0);
    } else {
        CHECK(res.gap_s == 0.0);
    }
    // Seiten durchblättern und zurück zur Live-Ansicht
    for (int i = 0; i < APP_PAGES; i++) {
        app_event(&app, (ui_event_t){EV_SWIPE_LEFT, 0, 0, 0, 0, true}, 130000);
    }
    CHECK(app.page == APP_PAGES - 1);
    tap(100, 222, 130100);  // „Live“
    CHECK(app.screen == APP_LIVE);
}

// Board ohne Pico-2-Teiler: 2,78 V bei frischen Batterien darf nicht als „leer“ erscheinen
static void test_battery_plausibility(void) {
    setup_app();
    app.vsys_ema = 0;
    app_vsys_sample(&app, 4180, false);
    CHECK(app.battery_pct == 90);  // ungeprüft, aber plausibel: anzeigen
    setup_app();
    ui.vsys = VSYS_UNKNOWN;
    app.vsys_ema = 0;
    app_vsys_sample(&app, 2780, false);
    CHECK(app.battery_pct == -1);  // unplausibel: nichts anzeigen
    rect_t d[16];
    app_frame(&app, 1000, d, 16);
    CHECK(!app.toast_shown);  // keine Warnung „fast leer“
    // am USB gemessen: 2,85 V statt ~4,7 V -> kein Standard-Teiler, dauerhaft aus
    app_set_status(&app, true, false);
    app_vsys_sample(&app, 2850, true);
    CHECK(ui.vsys == VSYS_NONE && app.req_save);
    app_set_status(&app, false, false);
    app_vsys_sample(&app, 4600, false);
    CHECK(app.battery_pct == -1);
    // Standard-Board: am USB bestätigt, dann zählt auch eine niedrige Spannung
    ui.vsys = VSYS_UNKNOWN;
    app_set_status(&app, true, false);
    app_vsys_sample(&app, 4720, true);
    CHECK(ui.vsys == VSYS_OK);
    app_set_status(&app, false, false);
    app_vsys_sample(&app, 3150, false);
    CHECK(app.battery_pct == 5);
}

// Hochkant: Bild 240 × 320, Touch-Umrechnung, Knöpfe an den neuen Stellen
static void test_portrait(void) {
    for (int x = 0; x < 240; x += 17) {
        for (int y = 0; y < 320; y += 23) {
            int xl, yl, x2, y2;
            touch_screen_to_land(true, x, y, &xl, &yl);
            CHECK(xl >= 0 && xl < 320 && yl >= 0 && yl < 240);
            touch_land_to_screen(true, xl, yl, &x2, &y2);
            CHECK(x2 == x && y2 == y);
        }
    }
    int x, y;
    touch_land_to_screen(true, 0, 120, &x, &y);  // linker Rand der Queransicht -> oben
    CHECK(y == 0 && x == 119);
    touch_land_to_screen(true, 319, 120, &x, &y);  // rechter Rand -> unten
    CHECK(y == 319);

    start_rotation = 1;
    setup_app();
    start_rotation = 0;
    CHECK(canvas.w == 240 && canvas.h == 320);
    CHECK(mon.L.portrait && mon.L.trace_x1 == 240 && mon.L.bottom_y == 236);
    uint32_t seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    tap(120, 160, 3000);
    CHECK(app.screen == APP_MENU);
    tap(120, 100, 3100);  // Messen & auswerten (hochkant oben im Menü)
    CHECK(app.measuring);
    const double end = 3.0 + ANA_MAX_S + 1.0;
    run_blocks(3.0, end + 1.0, 66.0, 500, &seq, 0);
    CHECK(app.screen == APP_RESULT && app.has_result);
    CHECK(fabs(res.hrv.mean_hr - 66.0) < 1.0);
    tap(220, 300, 130000);  // ›
    CHECK(app.page == 1);
    tap(70, 300, 130100);  // „Kurve“
    CHECK(app.screen == APP_LIVE);
}

// Einstellungen: Verstärkung und Vorschub wirken auf die Kurve, Ausrichtung auf das Bild
static void test_settings(void) {
    setup_app();
    tap(160, 120, 0);
    tap(160, 166, 10);  // Einstellungen
    CHECK(app.screen == APP_SETTINGS);
    const float px_mm = 320.0f / 57.6f;
    CHECK(fabsf(mon.px_per_sample - 25.0f * px_mm / 500.0f) < 1e-4f);  // 25 mm/s
    tap(160, 28 + 3 * 35 + 10, 20);  // Vorschub -> 50 mm/s
    CHECK(ui.speed == MON_SPEED_50 && app.req_save);
    CHECK(fabsf(mon.px_per_sample - 50.0f * px_mm / 500.0f) < 1e-4f);
    tap(160, 28 + 2 * 35 + 10, 30);  // Verstärkung Auto -> 5 mm/mV
    CHECK(ui.gain == MON_GAIN_5 && fabsf(mon.px_per_mv - 5.0f * px_mm) < 1e-3f);
    tap(160, 28 + 2 * 35 + 10, 40);  // -> 10 mm/mV
    CHECK(fabsf(mon.px_per_mv - 10.0f * px_mm) < 1e-3f);
    app.req_rotate = false;
    tap(160, 28 + 1 * 35 + 10, 50);  // Ausrichtung -> hochkant
    CHECK(ui.rotation == 1 && app.req_rotate && canvas.w == 240 && canvas.h == 320);
    CHECK(app.screen == APP_SETTINGS);
    tap(120, 28 + 1 * 44 + 10, 60);  // zurück auf quer (hochkant sind die Zeilen 44 px hoch)
    CHECK(ui.rotation == 0 && canvas.w == 320);
    tap(310, 10, 70);  // ×
    CHECK(app.screen == APP_LIVE);
}

static void test_short_stop(void) {
    setup_app();
    uint32_t seq = 0;
    tap(160, 120, 0);
    tap(160, 69, 10);
    run_blocks(0.0, 10.0, 60.0, 500, &seq, 0);
    tap(300, 12, 10000);  // Stopp nach 10 s: zu kurz
    CHECK(!app.measuring && app.screen == APP_LIVE && app.toast_shown);
}


// ------------------------------------------------------------------ Speicherformat
typedef struct {  // SEK3 (0.5.x), wie es ältere Firmware geschrieben hat
    uint32_t magic;
    uint8_t rotation, look, has_cal, vsys, gain, speed, reserved[2];
    float cal[6];
    uint32_t check;
} old_v3_t;

static void test_settings_codec(void) {
    ui_settings_t a, b;
    settings_defaults(&a);
    CHECK(a.person == PERSON_GUEST && a.speed == 1);
    a.rotation = 1;
    a.look = 0;
    a.has_cal = true;
    a.cal = (touch_cal_t){1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f};
    a.vsys = VSYS_OK;
    a.gain = 2;
    a.speed = 2;
    a.person = 1;
    snprintf(a.profiles[1].name, sizeof a.profiles[1].name, "Jörg");
    a.profiles[1].age = 54;
    a.profiles[1].sex = SEX_MALE;
    a.profiles[1].height_cm = 182;
    a.profiles[1].weight_kg = 78;
    uint8_t page[256];
    size_t n = settings_encode(&a, page, sizeof page);
    CHECK(n > 0 && n <= sizeof page && page[n] == 0xFF);
    settings_decode(page, &b);
    CHECK(b.rotation == 1 && b.look == 0 && b.has_cal && b.vsys == VSYS_OK);
    CHECK(b.gain == 2 && b.speed == 2 && b.person == 1);
    CHECK(b.cal.a == 1.5f && b.cal.f == 6.5f);
    CHECK(strcmp(b.profiles[1].name, "Jörg") == 0 && b.profiles[1].age == 54);
    CHECK(b.profiles[1].sex == SEX_MALE && b.profiles[1].height_cm == 182);
    CHECK(b.profiles[1].weight_kg == 78 && !profile_used(&b.profiles[0]));
    CHECK(settings_person(&b) == &b.profiles[1]);
    b.person = 0;  // Profil 0 nicht angelegt: wie Gast
    CHECK(settings_person(&b) == NULL);

    // beschädigt (ein Bit gekippt): Standardwerte
    page[10] ^= 0x01;
    settings_decode(page, &b);
    CHECK(!b.has_cal && b.person == PERSON_GUEST && b.speed == 1);
    // leerer Flash
    memset(page, 0xFF, sizeof page);
    settings_decode(page, &b);
    CHECK(!b.has_cal && b.person == PERSON_GUEST && !profile_used(&b.profiles[0]));

    // Einstellungen der Vorgängerfassung werden übernommen, Profile sind leer
    old_v3_t o;
    memset(&o, 0, sizeof o);
    o.magic = 0x334B4553u;
    o.rotation = 1;
    o.look = 0;
    o.has_cal = 1;
    o.vsys = VSYS_NONE;
    o.gain = 3;
    o.speed = 0;
    o.cal[0] = 9.0f;
    o.check = settings_checksum(&o, offsetof(old_v3_t, check) / 4);
    memset(page, 0xFF, sizeof page);
    memcpy(page, &o, sizeof o);
    settings_decode(page, &b);
    CHECK(b.rotation == 1 && b.look == 0 && b.has_cal && b.vsys == VSYS_NONE);
    CHECK(b.gain == 3 && b.speed == 0 && b.cal.a == 9.0f);
    CHECK(b.person == PERSON_GUEST && !profile_used(&b.profiles[0]) && !profile_used(&b.profiles[1]));

    // unsinnige Werte im Profil werden verworfen
    settings_defaults(&a);
    snprintf(a.profiles[0].name, sizeof a.profiles[0].name, "X");
    a.profiles[0].age = 200;
    a.profiles[0].height_cm = 10;
    a.profiles[0].weight_kg = 1;
    a.profiles[0].sex = 7;
    settings_encode(&a, page, sizeof page);
    settings_decode(page, &b);
    CHECK(b.profiles[0].age == 0 && b.profiles[0].height_cm == 0 && b.profiles[0].weight_kg == 0);
    CHECK(b.profiles[0].sex == SEX_UNKNOWN);
}

static void test_profile(void) {
    profile_t p;
    profile_clear(&p);
    char buf[64];
    profile_summary(&p, buf, sizeof buf);
    CHECK(strcmp(buf, "keine Angaben") == 0);
    CHECK(profile_bmi10(&p) == -1);
    p.age = 41;
    p.sex = SEX_FEMALE;
    p.height_cm = 168;
    p.weight_kg = 62;
    profile_summary(&p, buf, sizeof buf);
    CHECK(strcmp(buf, "41 J. · w · 168 cm · 62 kg") == 0);
    CHECK(profile_bmi10(&p) == 220);
    p.height_cm = 0;
    profile_summary(&p, buf, sizeof buf);
    CHECK(strcmp(buf, "41 J. · w · 62 kg") == 0);
}

// ------------------------------------------------------------------ Tastatur
static void key(kb_t *k, int w, int h, const char *label) {
    int x = -1, y = -1;
    CHECK(kb_key_center(k, w, h, label, &x, &y));
    kb_tap(k, w, h, x, y);
}

static void test_keyboard_size(int w, int h) {
    kb_t k;
    kb_open_text(&k, "Name", "", 12);
    CHECK(k.shift);  // am Anfang groß
    key(&k, w, h, "j");
    CHECK(strcmp(k.buf, "J") == 0 && !k.shift);
    key(&k, w, h, "ö");
    key(&k, w, h, "r");
    key(&k, w, h, "g");
    CHECK(strcmp(k.buf, "Jörg") == 0);
    key(&k, w, h, "-");
    CHECK(k.shift);  // nach Bindestrich wieder groß
    key(&k, w, h, "ä");
    CHECK(strcmp(k.buf, "Jörg-Ä") == 0);
    key(&k, w, h, "<");  // löscht das ganze Zeichen mit Umlaut
    key(&k, w, h, "<");
    CHECK(strcmp(k.buf, "Jörg") == 0);
    key(&k, w, h, "^");
    key(&k, w, h, "x");
    CHECK(strcmp(k.buf, "JörgX") == 0);
    key(&k, w, h, "Leer");
    key(&k, w, h, "Leer");  // kein doppeltes Leerzeichen
    key(&k, w, h, "s");
    CHECK(strcmp(k.buf, "JörgX S") == 0);
    // höchstens 12 Zeichen
    for (int i = 0; i < 10; i++) {
        key(&k, w, h, "m");
    }
    CHECK(strcmp(k.buf, "JörgX Smmmmm") == 0);
    key(&k, w, h, "ß");
    CHECK(strcmp(k.buf, "JörgX Smmmmm") == 0);
    // OK schneidet Leerzeichen am Ende ab und liefert den Text
    kb_open_text(&k, "Name", "", 12);
    key(&k, w, h, "o");
    key(&k, w, h, "Leer");
    int x, y;
    CHECK(kb_key_center(&k, w, h, "OK", &x, &y));
    CHECK(kb_tap(&k, w, h, x, y) == KB_OK && strcmp(k.buf, "O") == 0);
    // leerer Name: OK gesperrt; Kreuz oben rechts bricht ab
    kb_open_text(&k, "Name", "", 12);
    CHECK(!kb_can_ok(&k));
    CHECK(kb_key_center(&k, w, h, "OK", &x, &y));
    CHECK(kb_tap(&k, w, h, x, y) == KB_NONE);
    CHECK(kb_tap(&k, w, h, w - 20, 12) == KB_CANCEL);

    // Zahlen: Bereich, führende Null, Länge
    kb_open_number(&k, "Größe", "cm", 0, 50, 250);
    key(&k, w, h, "0");
    CHECK(k.buf[0] == '\0');
    key(&k, w, h, "1");
    key(&k, w, h, "8");
    key(&k, w, h, "2");
    key(&k, w, h, "9");
    CHECK(kb_value(&k) == 182 && strcmp(k.buf, "182") == 0);
    key(&k, w, h, "<");
    key(&k, w, h, "<");
    key(&k, w, h, "<");
    CHECK(kb_value(&k) == 0 && kb_can_ok(&k));  // leer = keine Angabe
    key(&k, w, h, "5");
    CHECK(!kb_can_ok(&k));  // 5 cm: unter dem Bereich
    CHECK(kb_key_center(&k, w, h, "OK", &x, &y));
    CHECK(kb_tap(&k, w, h, x, y) == KB_NONE);
    key(&k, w, h, "0");
    CHECK(kb_value(&k) == 50 && kb_can_ok(&k));
    CHECK(kb_tap(&k, w, h, x, y) == KB_OK);
    kb_open_number(&k, "Alter", "Jahre", 0, 1, 120);
    key(&k, w, h, "1");
    key(&k, w, h, "2");
    key(&k, w, h, "5");  // 125 > 120: wird nicht angenommen
    CHECK(kb_value(&k) == 12);
    kb_open_number(&k, "Gewicht", "kg", 78, 2, 250);
    CHECK(kb_value(&k) == 78);
}

static void test_keyboard(void) {
    test_keyboard_size(320, 240);
    test_keyboard_size(240, 320);
    // Taste in der Lücke zwischen zwei Tasten: trifft die nächste, nie ins Leere
    kb_t k;
    kb_open_text(&k, "Name", "", 12);
    int hits = 0;
    for (int x = 0; x < 320; x++) {
        int y = 240 - 34 * 5 + 10;  // erste Tastenreihe
        k.buf[0] = '\0';
        kb_tap(&k, 320, 240, x, y);
        hits += k.buf[0] != '\0';
    }
    CHECK(hits == 320);
}

// ------------------------------------------------------------------ Check-Liste
static ana_result_t synth(void) {
    ana_result_t r;
    memset(&r, 0, sizeof r);
    r.ok = true;
    r.n_beats = 120;
    r.duration_s = 120.0;
    r.has_hrv = true;
    r.hrv.mean_hr = 72.0;
    r.hrv.rmssd_ms = 40.0;
    r.rhythm = (ana_rhythm_t){true, 72.0, true, 10, 0, 0, 0, false};
    r.has_median = true;
    r.has_delineation = true;
    r.d.p_onset = 100;
    r.d.pr_ms = 160.0;
    r.d.qrs_ms = 90.0;
    r.d.qt_ms = 380.0;
    r.d.qtc_fridericia_ms = 410.0;
    return r;
}

static chk_status_t status_of(const ana_result_t *r, const profile_t *p, bool test, chk_id_t id) {
    check_item_t it[CHK_MAX];
    CHECK(check_build(r, p, test, it, CHK_MAX) == CHK_MAX);
    CHECK(it[id].id == id && it[id].label && it[id].verdict && it[id].advice && it[id].about);
    return it[id].status;
}

static void test_check(void) {
    ana_result_t r = synth();
    check_item_t it[CHK_MAX];
    CHECK(check_build(&r, NULL, false, it, CHK_MAX) == CHK_MAX);
    CHECK(check_build(&r, NULL, false, it, CHK_MAX - 1) == 0);
    check_summary_t sum = check_summarize(it, CHK_MAX);
    CHECK(sum.rated == 8 && sum.ok == 8 && sum.note == 0 && sum.alert == 0);  // HRV nur Info
    char t[80];
    check_summary_text(&sum, t, sizeof t);
    CHECK(strcmp(t, "Alles im üblichen Bereich") == 0);
    CHECK(strcmp(it[CHK_PULSE].value, "72 /min") == 0);
    CHECK(strcmp(it[CHK_QTC].value, "410 ms") == 0);
    CHECK(strcmp(it[CHK_HRV].value, "RMSSD 40 ms") == 0);

    // Puls
    const double hrs[] = {35, 39.4, 45, 49.6, 50, 100, 100.4, 105, 120, 121, 150};
    const chk_status_t want[] = {CHK_ALERT, CHK_ALERT, CHK_NOTE, CHK_OK,  CHK_OK,   CHK_OK,
                                 CHK_OK,    CHK_NOTE,  CHK_NOTE, CHK_ALERT, CHK_ALERT};
    for (size_t i = 0; i < sizeof hrs / sizeof hrs[0]; i++) {
        r = synth();
        r.hrv.mean_hr = hrs[i];
        CHECK(status_of(&r, NULL, false, CHK_PULSE) == want[i]);
    }
    // ohne HRV zählt der Median aus dem Rhythmus
    r = synth();
    r.has_hrv = false;
    r.rhythm.median_hr = 130.0;
    CHECK(status_of(&r, NULL, false, CHK_PULSE) == CHK_ALERT);
    r.rhythm.valid = false;
    CHECK(status_of(&r, NULL, false, CHK_PULSE) == CHK_NA);

    // QTc: Grenze 450 ms, bei Frauen 460 ms; ab 500 ms rot; unter 340 ms beachten
    profile_t man, woman, nobody;
    memset(&man, 0, sizeof man);
    memset(&woman, 0, sizeof woman);
    memset(&nobody, 0, sizeof nobody);
    snprintf(man.name, sizeof man.name, "M");
    snprintf(woman.name, sizeof woman.name, "W");
    snprintf(nobody.name, sizeof nobody.name, "N");
    man.sex = SEX_MALE;
    woman.sex = SEX_FEMALE;
    r = synth();
    r.d.qtc_fridericia_ms = 455.0;
    CHECK(status_of(&r, &man, false, CHK_QTC) == CHK_NOTE);
    CHECK(status_of(&r, &woman, false, CHK_QTC) == CHK_OK);
    CHECK(status_of(&r, &nobody, false, CHK_QTC) == CHK_NOTE);
    r.d.qtc_fridericia_ms = 462.0;
    CHECK(status_of(&r, &woman, false, CHK_QTC) == CHK_NOTE);
    r.d.qtc_fridericia_ms = 510.0;
    CHECK(status_of(&r, &woman, false, CHK_QTC) == CHK_ALERT);
    r.d.qtc_fridericia_ms = 330.0;
    CHECK(status_of(&r, &man, false, CHK_QTC) == CHK_NOTE);
    r.d.qtc_fridericia_ms = 800.0;  // unplausibel
    CHECK(status_of(&r, &man, false, CHK_QTC) == CHK_NA);
    check_build(&(ana_result_t){.has_delineation = true, .d = {.qtc_fridericia_ms = 455.0}}, &nobody,
                false, it, CHK_MAX);
    CHECK(strcmp(it[CHK_QTC].verdict, "leicht verlängert") == 0 ||
          strcmp(it[CHK_QTC].verdict, "grenzwertig") == 0);

    // QRS und PR
    r = synth();
    r.d.qrs_ms = 115.0;
    CHECK(status_of(&r, NULL, false, CHK_QRS) == CHK_NOTE);
    r.d.qrs_ms = 125.0;
    CHECK(status_of(&r, NULL, false, CHK_QRS) == CHK_ALERT);
    r.d.qrs_ms = 300.0;
    CHECK(status_of(&r, NULL, false, CHK_QRS) == CHK_NA);
    r = synth();
    r.d.pr_ms = 220.0;
    CHECK(status_of(&r, NULL, false, CHK_PR) == CHK_NOTE);
    r.d.pr_ms = 100.0;
    CHECK(status_of(&r, NULL, false, CHK_PR) == CHK_NOTE);
    r.d.pr_ms = NAN;
    CHECK(status_of(&r, NULL, false, CHK_PR) == CHK_NA);
    r = synth();
    r.d.p_onset = -1;
    CHECK(status_of(&r, NULL, false, CHK_P) == CHK_NA);
    r.has_delineation = false;
    CHECK(status_of(&r, NULL, false, CHK_QTC) == CHK_NA);

    // Rhythmus und Extraschläge
    r = synth();
    r.rhythm.irregular_windows = 2;
    CHECK(status_of(&r, NULL, false, CHK_RHYTHM) == CHK_NOTE);
    CHECK(status_of(&r, NULL, false, CHK_ECTOPY) == CHK_OK);
    r.rhythm.irregular_windows = 6;
    CHECK(status_of(&r, NULL, false, CHK_RHYTHM) == CHK_ALERT);
    CHECK(status_of(&r, NULL, false, CHK_ECTOPY) == CHK_NA);  // bei Flimmern nicht zählbar
    r = synth();
    r.rhythm.n_ect = 2;
    CHECK(status_of(&r, NULL, false, CHK_ECTOPY) == CHK_OK);
    r.rhythm.n_ect = 5;
    CHECK(status_of(&r, NULL, false, CHK_ECTOPY) == CHK_NOTE);
    r.rhythm.n_ect = 14;
    CHECK(status_of(&r, NULL, false, CHK_ECTOPY) == CHK_ALERT);  // > 10 % von 120
    r = synth();
    r.rhythm.valid = false;
    CHECK(status_of(&r, NULL, false, CHK_RHYTHM) == CHK_NA);

    // Signal
    r = synth();
    r.has_mains_ratio = true;
    r.mains_ratio = 0.2;
    CHECK(status_of(&r, NULL, false, CHK_SIGNAL) == CHK_NOTE);
    r.mains_ratio = 0.7;
    check_build(&r, NULL, false, it, CHK_MAX);
    CHECK(it[CHK_SIGNAL].status == CHK_NOTE && strstr(it[CHK_SIGNAL].verdict, "neu messen"));
    r = synth();
    r.rhythm.n_err = 4;
    CHECK(status_of(&r, NULL, false, CHK_SIGNAL) == CHK_NOTE);
    const char *txt;
    CHECK(check_quality(&r, &txt) == 0 && strcmp(txt, "gut") == 0);
    r.masked_s = 12.0;
    CHECK(check_quality(&r, &txt) == 2);

    // Zusammenfassung
    r = synth();
    r.hrv.mean_hr = 130.0;  // rot
    r.d.pr_ms = 230.0;      // gelb
    r.d.qrs_ms = 100.0;
    check_build(&r, NULL, false, it, CHK_MAX);
    sum = check_summarize(it, CHK_MAX);
    CHECK(sum.alert == 1 && sum.note == 1);
    check_summary_text(&sum, t, sizeof t);
    CHECK(strcmp(t, "1 Punkt ärztlich abklären, 1 beachten") == 0);
    sum = (check_summary_t){8, 5, 3, 0};
    check_summary_text(&sum, t, sizeof t);
    CHECK(strcmp(t, "3 Punkte beachten") == 0);
    sum = (check_summary_t){2, 2, 0, 0};
    check_summary_text(&sum, t, sizeof t);
    CHECK(strcmp(t, "Zu wenig für einen Check") == 0);

    // Kinder: Puls und Zeiten werden nicht bewertet, Rhythmus schon
    profile_t kid;
    memset(&kid, 0, sizeof kid);
    snprintf(kid.name, sizeof kid.name, "Kind");
    kid.age = 12;
    r = synth();
    r.hrv.mean_hr = 130.0;
    CHECK(status_of(&r, &kid, false, CHK_PULSE) == CHK_NA);
    CHECK(status_of(&r, &kid, false, CHK_PR) == CHK_NA);
    CHECK(status_of(&r, &kid, false, CHK_QRS) == CHK_NA);
    CHECK(status_of(&r, &kid, false, CHK_QTC) == CHK_NA);
    CHECK(status_of(&r, &kid, false, CHK_RHYTHM) == CHK_OK);
    kid.age = 18;
    CHECK(status_of(&r, &kid, false, CHK_PULSE) == CHK_ALERT);

    // Testsignal: nichts wird bewertet
    r = synth();
    check_build(&r, NULL, true, it, CHK_MAX);
    sum = check_summarize(it, CHK_MAX);
    CHECK(sum.rated == 0);
    check_summary_text(&sum, t, sizeof t);
    CHECK(strcmp(t, "Zu wenig für einen Check") == 0);
}

// ------------------------------------------------------------------ Profile im Gerät
static void key_tap(const char *label, uint32_t now) {
    int x = -1, y = -1;
    CHECK(kb_key_center(&app.kb, canvas.w, canvas.h, label, &x, &y));
    tap(x, y, now);
}

static void test_profiles_flow(void) {
    setup_app();
    uint32_t seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    tap(160, 120, 3000);  // Menü
    CHECK(app.screen == APP_MENU);
    tap(160, 206, 3100);  // Person
    CHECK(app.screen == APP_PROFILES);
    // Profil 1 anlegen: Name, Alter, Geschlecht (zweimal tippen = weiblich), Größe, Gewicht
    tap(100, 58, 3200);
    CHECK(app.screen == APP_KEYBOARD && app.kb.mode == KB_TEXT);
    key_tap("a", 3300);
    key_tap("n", 3310);
    key_tap("n", 3320);
    key_tap("a", 3330);
    key_tap("OK", 3340);
    CHECK(app.screen == APP_PROFILE_EDIT && strcmp(ui.profiles[0].name, "Anna") == 0);
    CHECK(app.req_save);
    tap(100, 73, 3400);  // Alter
    CHECK(app.screen == APP_KEYBOARD && app.kb.mode == KB_NUMBER);
    key_tap("4", 3410);
    key_tap("1", 3420);
    key_tap("OK", 3430);
    CHECK(ui.profiles[0].age == 41 && app.screen == APP_PROFILE_EDIT);
    tap(100, 103, 3500);  // Geschlecht
    tap(100, 103, 3510);
    CHECK(ui.profiles[0].sex == SEX_FEMALE);
    tap(100, 133, 3600);  // Größe
    key_tap("1", 3610);
    key_tap("6", 3620);
    key_tap("8", 3630);
    key_tap("OK", 3640);
    tap(100, 163, 3700);  // Gewicht, dann abbrechen: bleibt leer
    key_tap("6", 3710);
    tap(310, 12, 3720);  // Kreuz
    CHECK(app.screen == APP_PROFILE_EDIT && ui.profiles[0].weight_kg == 0);
    tap(100, 163, 3800);
    key_tap("6", 3810);
    key_tap("2", 3820);
    key_tap("OK", 3830);
    CHECK(ui.profiles[0].weight_kg == 62 && profile_bmi10(&ui.profiles[0]) == 220);
    tap(240, 218, 3900);  // Fertig: neues Profil wird gleich gewählt
    CHECK(app.screen == APP_PROFILES && ui.person == 0);
    tap(310, 12, 3950);  // zurück ins Menü
    CHECK(app.screen == APP_MENU);
    // Profil 2 antippen: leer -> Editor; ohne Namen wieder zu, Person bleibt
    tap(160, 206, 4000);
    tap(100, 114, 4100);
    CHECK(app.screen == APP_KEYBOARD);
    tap(310, 12, 4110);  // Abbrechen
    CHECK(app.screen == APP_PROFILE_EDIT);
    tap(240, 218, 4200);  // Fertig ohne Namen
    CHECK(app.screen == APP_PROFILES && !profile_used(&ui.profiles[1]) && ui.person == 0);
    // Gast wählen -> Menü zeigt „Person: Gast“
    tap(100, 172, 4300);
    CHECK(app.screen == APP_MENU && ui.person == PERSON_GUEST);
    // Anna wieder wählen und messen
    tap(160, 206, 4400);
    tap(60, 58, 4500);  // Zeile antippen = wählen
    CHECK(app.screen == APP_MENU && ui.person == 0);
    tap(160, 69, 4600);
    CHECK(app.measuring && app.meas_known && app.meas_profile.age == 41);
    const double end = 3.0 + ANA_MAX_S + 1.0;
    ui.profiles[0].age = 30;  // Änderung während der Messung ändert die Bewertung nicht
    run_blocks(3.0, end + 1.0, 66.0, 500, &seq, 0);
    CHECK(app.screen == APP_RESULT && app.has_result && app.n_checks == CHK_MAX);
    CHECK(app.meas_profile.age == 41 && strcmp(app.meas_profile.name, "Anna") == 0);
    // Übersicht -> Check-Seite durch Tippen auf die Zeile „Check“
    CHECK(app.page == 0);
    tap(160, 160, 130000);
    CHECK(app.page == PAGE_CHECK);
    // Zeile „Puls in Ruhe“ -> Erklärung, wischen, zurück
    tap(160, 28 + 17 + 8, 130100);
    CHECK(app.screen == APP_CHECK_DETAIL && app.detail == CHK_PULSE);
    app_event(&app, (ui_event_t){EV_SWIPE_LEFT, 0, 0, 0, 0, true}, 130200);
    CHECK(app.detail == CHK_RHYTHM);
    app_event(&app, (ui_event_t){EV_SWIPE_RIGHT, 0, 0, 0, 0, true}, 130300);
    app_event(&app, (ui_event_t){EV_SWIPE_RIGHT, 0, 0, 0, 0, true}, 130400);
    app_event(&app, (ui_event_t){EV_SWIPE_RIGHT, 0, 0, 0, 0, true}, 130500);
    CHECK(app.detail == CHK_SIGNAL);  // am Anfang bleibt es stehen
    tap(160, 120, 130600);
    CHECK(app.screen == APP_RESULT && app.page == PAGE_CHECK);
    CHECK(app.checks[CHK_PULSE].status == CHK_OK);
    // Profil löschen
    tap(100, 222, 131000);  // Kurve (Live)
    CHECK(app.screen == APP_LIVE);
    tap(160, 120, 131100);
    tap(160, 206, 131200);
    tap(300, 58, 131300);  // „Ändern“ bei Anna
    CHECK(app.screen == APP_PROFILE_EDIT && !app.edit_new);
    tap(80, 218, 131400);  // Löschen
    CHECK(app.screen == APP_PROFILES && !profile_used(&ui.profiles[0]) && ui.person == PERSON_GUEST);
}

// Hochkant: Profilbildschirme und Check-Seite an den neuen Stellen
static void test_profiles_portrait(void) {
    start_rotation = 1;
    setup_app();
    start_rotation = 0;
    uint32_t seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    tap(120, 160, 3000);
    CHECK(app.screen == APP_MENU);
    tap(120, 252, 3100);  // Person
    CHECK(app.screen == APP_PROFILES);
    tap(100, 30 + 35, 3200);  // Profil 1 anlegen
    CHECK(app.screen == APP_KEYBOARD);
    key_tap("t", 3300);
    key_tap("i", 3310);
    key_tap("m", 3320);
    key_tap("OK", 3330);
    CHECK(strcmp(ui.profiles[0].name, "Tim") == 0 && app.screen == APP_PROFILE_EDIT);
    tap(100, 28 + 40 + 20, 3400);  // Alter
    key_tap("1", 3410);
    key_tap("0", 3420);
    key_tap("OK", 3430);
    CHECK(ui.profiles[0].age == 10);
    tap(180, 290, 3500);  // Fertig
    CHECK(app.screen == APP_PROFILES && ui.person == 0);
    tap(220, 12, 3600);
    CHECK(app.screen == APP_MENU);
    tap(120, 100, 3700);  // Messen
    CHECK(app.measuring);
    run_blocks(3.0, ANA_MAX_S + 4.0, 66.0, 500, &seq, 0);
    CHECK(app.screen == APP_RESULT && app.has_result);
    // Kind: Puls wird nicht bewertet
    CHECK(app.checks[CHK_PULSE].status == CHK_NA && app.checks[CHK_RHYTHM].status == CHK_OK);
    tap(220, 300, 130000);  // ›
    CHECK(app.page == PAGE_CHECK);
    tap(120, 50 + 23 + 11, 130100);  // „Puls in Ruhe“
    CHECK(app.screen == APP_CHECK_DETAIL && app.detail == CHK_PULSE);
}

int main(void) {
    test_touchcal();
    test_gestures();
    test_battery();
    test_flow(500, 0);
    test_flow(1000, 0);
    test_flow(500, 40);  // jeder 40. Block fehlt
    test_short_stop();
    test_battery_plausibility();
    test_portrait();
    test_settings();
    test_settings_codec();
    test_profile();
    test_keyboard();
    test_check();
    test_profiles_flow();
    test_profiles_portrait();
    if (failures) {
        fprintf(stderr, "%d Fehler\n", failures);
        return 1;
    }
    puts("alle UI-Tests bestanden");
    return 0;
}
