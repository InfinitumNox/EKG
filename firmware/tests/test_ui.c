// Host-Tests der Bedienung: Touch-Kalibrierung, Gesten, Batterieanzeige, Speicherformat,
// Tastatur, Check-Liste und der Ablauf Menü -> Messung -> Auswertung mit einem künstlichen EKG.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_int.h"
#include "avatar.h"
#include "check.h"
#include "gesture.h"
#include "keyboard.h"
#include "private_data.h"
#include "settings.h"
#include "solo.h"
#include "protocol.h"
#include "theme.h"
#include "touchcal.h"

static int failures;

// Zufall für den Salt: im Test fest (nur der Ablauf zählt)
void priv_random_bytes(uint8_t *out, size_t n) {
    static uint32_t x = 12345u;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        out[i] = (uint8_t)(x >> 24);
    }
}

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
static priv_data_t pdata;

static double ecg_mv(double t, double hr) {
    double rr = 60.0 / hr, ph = fmod(t, rr) - 0.5 * rr;
    double v = 1.1 * exp(-0.5 * pow(ph / 0.012, 2)) - 0.15 * exp(-0.5 * pow((ph - 0.03) / 0.01, 2));
    v += 0.12 * exp(-0.5 * pow((ph + 0.16) / 0.025, 2)) + 0.3 * exp(-0.5 * pow((ph - 0.25) / 0.05, 2));
    return v;
}

// Signalquelle der Ablauf-Tests: Standard ist gleichmäßiger Puls; für die Atmung ersetzbar
static double (*ecg_fn)(double t, double hr) = ecg_mv;

static uint32_t mv_to_raw(double mv, int oversampling) {
    double v = mv / 1000.0 * 1100.0 + 1.65;
    return (uint32_t)lround(v / (3.3 / 4096.0 / oversampling));
}

static void run_blocks_flags(double t0, double t1, double hr, int rate, uint32_t *seq,
                             int skip_every, uint8_t flags) {
    const int n = rate / 20, os = 32000 / rate;
    uint32_t raw[64];
    for (double t = t0; t < t1 - 1e-9; t += 0.05) {
        for (int i = 0; i < n; i++) {
            raw[i] = mv_to_raw(ecg_fn(t + (double)i / rate, hr), os);
        }
        uint32_t s = (*seq)++;
        if (skip_every && s % (uint32_t)skip_every == (uint32_t)skip_every - 1) {
            continue;  // Block geht verloren
        }
        uint32_t now = (uint32_t)lround(t * 1000.0);
        app_feed(&app, raw, n, flags, (uint32_t)os, s);
        rect_t d[16];
        app_frame(&app, now, d, 16);
    }
}

static void run_blocks(double t0, double t1, double hr, int rate, uint32_t *seq, int skip_every) {
    run_blocks_flags(t0, t1, hr, rate, seq, skip_every, 0);
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
    memset(&pdata, 0, sizeof pdata);
    app.pd = &pdata;
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
    tap(90, 166, 10);  // Einstellungen (links in der Zeile mit „Elektroden“)
    CHECK(app.screen == APP_SETTINGS);
    const float px_mm = 320.0f / 57.6f;
    CHECK(fabsf(mon.px_per_sample - 25.0f * px_mm / 500.0f) < 1e-4f);  // 25 mm/s
    tap(160, 28 + 3 * 30 + 10, 20);  // Vorschub -> 50 mm/s
    CHECK(ui.speed == MON_SPEED_50 && app.req_save);
    CHECK(fabsf(mon.px_per_sample - 50.0f * px_mm / 500.0f) < 1e-4f);
    tap(160, 28 + 2 * 30 + 10, 30);  // Verstärkung Auto -> 5 mm/mV
    CHECK(ui.gain == MON_GAIN_5 && fabsf(mon.px_per_mv - 5.0f * px_mm) < 1e-3f);
    tap(160, 28 + 2 * 30 + 10, 40);  // -> 10 mm/mV
    CHECK(fabsf(mon.px_per_mv - 10.0f * px_mm) < 1e-3f);
    app.req_rotate = false;
    tap(160, 28 + 1 * 30 + 10, 50);  // Ausrichtung -> hochkant
    CHECK(ui.rotation == 1 && app.req_rotate && canvas.w == 240 && canvas.h == 320);
    CHECK(app.screen == APP_SETTINGS);
    tap(120, 28 + 1 * 40 + 10, 60);  // zurück auf quer (hochkant sind die Zeilen 40 px hoch)
    CHECK(ui.rotation == 0 && canvas.w == 320);
    CHECK(ui.avatar == 0);
    tap(160, 28 + 4 * 30 + 10, 65);  // Avatar -> Standardfigur
    CHECK(ui.avatar == 1 && app.req_save);
    tap(160, 28 + 4 * 30 + 10, 66);
    CHECK(ui.avatar == 0);
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
    a.avatar = 1;
    a.cables = 2;
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
    CHECK(b.avatar == 1 && b.cables == 2);
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
    CHECK(b.avatar == 0 && b.cables == 0);

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

// ------------------------------------------------------------------ Avatar und Elektroden-Anleitung
static int painted(const uint16_t *px, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        k += px[i] != 0;
    }
    return k;
}

static void test_avatar(void) {
    profile_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.name, sizeof p.name, "T");
    p.sex = SEX_FEMALE;
    p.height_cm = 160;
    p.weight_kg = 90;
    avatar_t a = avatar_for(&p, AVATAR_PROFILE);
    CHECK(a.sex == SEX_FEMALE && a.height_cm == 160 && a.bmi10 == 352);
    a = avatar_for(&p, AVATAR_STANDARD);  // Standard: normalgewichtig, Größe nach Geschlecht
    CHECK(a.sex == SEX_FEMALE && a.height_cm == 165 && a.bmi10 == 220);
    p.sex = SEX_MALE;
    a = avatar_for(&p, AVATAR_STANDARD);
    CHECK(a.sex == SEX_MALE && a.height_cm == 178 && a.bmi10 == 220);
    a = avatar_for(NULL, AVATAR_PROFILE);  // Gast: neutral, in beiden Modi
    CHECK(a.sex == SEX_UNKNOWN && a.bmi10 == 220);
    p.sex = SEX_UNKNOWN;
    p.height_cm = 0;
    p.weight_kg = 0;
    a = avatar_for(&p, AVATAR_PROFILE);  // ohne Größe und Gewicht: Normalfigur
    CHECK(a.sex == SEX_UNKNOWN && a.bmi10 == 220 && a.height_cm == 172);

    // Zeichnen: schwerer = mehr Fläche, kleiner = tiefer, Elektroden liegen am Körper
    enum { W = 124, H = 168 };
    static uint16_t px[W * H];
    canvas_t c = {px, W, H};
    avatar_sites_t sites;
    const avatar_t slim = {SEX_FEMALE, 170, 175}, heavy = {SEX_FEMALE, 170, 340};
    const avatar_t kid = {SEX_UNKNOWN, 110, 170};
    memset(px, 0, sizeof px);
    avatar_draw(&c, 0, 0, W, H, &slim, &sites);
    const int n_slim = painted(px, W * H);
    memset(px, 0, sizeof px);
    avatar_draw(&c, 0, 0, W, H, &heavy, NULL);
    const int n_heavy = painted(px, W * H);
    CHECK(n_slim > 1000 && n_heavy > n_slim * 12 / 10);
    CHECK(sites.ra.x < W / 2.0f && sites.rl.x < W / 2.0f && sites.la.x > W / 2.0f);
    CHECK(sites.ra.y < sites.la.y && sites.la.y == sites.rl.y && sites.r >= 3.6f);
    memset(px, 0, sizeof px);
    avatar_draw(&c, 0, 0, W, H, &kid, NULL);
    CHECK(painted(px, W * H) < n_slim);
    int first = -1;
    for (int i = 0; i < W * H && first < 0; i++) {
        first = px[i] ? i / W : -1;
    }
    CHECK(first > H / 4);  // 110 cm: die Figur beginnt deutlich tiefer als bei 190 cm
    // Figur bleibt in der Box (auch bei sehr breiter Figur)
    const avatar_t wide = {SEX_MALE, 190, 450};
    memset(px, 0, sizeof px);
    avatar_draw(&c, 0, 0, W, H, &wide, NULL);
    int edge = 0;
    for (int y = 0; y < H; y++) {
        edge += px[y * W] != 0 || px[y * W + W - 1] != 0;
    }
    CHECK(edge == 0);
}

static void test_leads(void) {
    setup_app();
    uint32_t seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    // Menü -> Elektroden (rechts neben dem letzten Ergebnis), Farben weiterschalten, Fertig
    tap(160, 120, 3000);
    tap(230, 166, 3100);  // Elektroden (rechts neben den Einstellungen)
    CHECK(app.screen == APP_LEADS && !app.leads_auto);
    tap(80, 222, 3200);
    CHECK(ui.cables == 1 && app.req_save);
    tap(80, 222, 3210);
    tap(80, 222, 3220);
    CHECK(ui.cables == 0 && app.screen == APP_LEADS);
    tap(240, 222, 3300);
    CHECK(app.screen == APP_LIVE);

    // Ohne Kontakt erscheint die Anleitung nach 4 s von selbst, nicht während einer Messung
    setup_app();
    seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    run_blocks_flags(3.0, 6.0, 66.0, 500, &seq, 0, EKG_FLAG_LO_PLUS);
    CHECK(app.screen == APP_LIVE);
    run_blocks_flags(6.0, 8.0, 66.0, 500, &seq, 0, EKG_FLAG_LO_PLUS);
    CHECK(app.screen == APP_LEADS && app.leads_auto && app.leads_shown == EKG_FLAG_LO_PLUS);
    run_blocks_flags(8.0, 8.5, 66.0, 500, &seq, 0, EKG_FLAG_LO_PLUS | EKG_FLAG_LO_MINUS);
    CHECK(app.leads_shown == (EKG_FLAG_LO_PLUS | EKG_FLAG_LO_MINUS));
    run_blocks(8.5, 9.5, 66.0, 500, &seq, 0);  // Kontakt, aber erst 1 s
    CHECK(app.screen == APP_LEADS);
    run_blocks(9.5, 11.5, 66.0, 500, &seq, 0);
    CHECK(app.screen == APP_LIVE && app.toast_shown);
    // wieder ab: nach dem Hinweis erneut, Fertig schließt und sie bleibt zu, bis Kontakt da war
    run_blocks_flags(11.5, 20.0, 66.0, 500, &seq, 0, EKG_FLAG_LO_MINUS);
    CHECK(app.screen == APP_LEADS);
    tap(240, 222, 20000);
    CHECK(app.screen == APP_LIVE && app.lead_dismissed);
    run_blocks_flags(20.0, 30.0, 66.0, 500, &seq, 0, EKG_FLAG_LO_MINUS);
    CHECK(app.screen == APP_LIVE);
    run_blocks(30.0, 33.0, 66.0, 500, &seq, 0);
    CHECK(!app.lead_dismissed);
    run_blocks_flags(33.0, 40.0, 66.0, 500, &seq, 0, EKG_FLAG_LO_MINUS);
    CHECK(app.screen == APP_LEADS);

    // Während der Messung bleibt die Kurve mit dem kleinen Hinweis stehen
    setup_app();
    seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    tap(160, 120, 3000);
    tap(160, 69, 3100);
    CHECK(app.measuring);
    run_blocks_flags(3.0, 12.0, 66.0, 500, &seq, 0, EKG_FLAG_LO_PLUS);
    CHECK(app.screen == APP_LIVE && app.measuring);

    // Hochkant: Menüeintrag und Schaltflächen an den neuen Stellen
    start_rotation = 1;
    setup_app();
    start_rotation = 0;
    seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    tap(120, 160, 3000);
    tap(171, 204, 3100);
    CHECK(app.screen == APP_LEADS);
    tap(60, 298, 3200);  // Farben
    CHECK(ui.cables == 1);
    tap(180, 298, 3300);  // Fertig
    CHECK(app.screen == APP_LIVE);
}

// ------------------------------------------------------------------ Atmung
// EKG mit Atmung: Der Schlagabstand schwankt mit der Atemfrequenz (Atemrhythmus der Herzfrequenz),
// die Höhe der R-Zacke ebenfalls.
static double g_resp_hz = 0.25, g_rsa = 0.06, g_amp_mod = 0.15;
static double beats_t[2400];
static int n_beats_t, beat_cursor;

static void make_beats(void) {
    n_beats_t = 0;
    beat_cursor = 0;
    for (double t = 0.4; t < 160.0 && n_beats_t < 2400; n_beats_t++) {
        beats_t[n_beats_t] = t;
        t += 0.85 * (1.0 + g_rsa * sin(2.0 * M_PI * g_resp_hz * t));
    }
}

static double breath_ecg(double t, double hr) {
    (void)hr;
    double v = 0.0;
    while (beat_cursor > 0 && beats_t[beat_cursor - 1] > t - 0.5) {
        beat_cursor--;
    }
    while (beat_cursor < n_beats_t && beats_t[beat_cursor] < t - 0.5) {
        beat_cursor++;
    }
    for (int i = beat_cursor; i < n_beats_t && beats_t[i] < t + 0.4; i++) {
        double ph = t - beats_t[i];
        double k = 1.0 + g_amp_mod * sin(2.0 * M_PI * g_resp_hz * beats_t[i]);
        v += k * (1.1 * exp(-0.5 * pow(ph / 0.012, 2)) - 0.15 * exp(-0.5 * pow((ph - 0.03) / 0.01, 2)) +
                  0.12 * exp(-0.5 * pow((ph + 0.16) / 0.025, 2)) +
                  0.3 * exp(-0.5 * pow((ph - 0.25) / 0.05, 2)));
    }
    return v;
}

static void breath_measure(double resp_hz, double rsa, double amp_mod, uint32_t *seq) {
    g_resp_hz = resp_hz;
    g_rsa = rsa;
    g_amp_mod = amp_mod;
    make_beats();
    ecg_fn = breath_ecg;
    setup_app();
    *seq = 0;
    run_blocks(0.0, 3.0, 70.0, 500, seq, 0);
    tap(160, 120, 3000);  // Menü
    CHECK(app.screen == APP_MENU && app.mode == MODE_EKG);
    tap(136, 28, 3050);  // Umschalter: Atmung
    CHECK(app.mode == MODE_BREATH);
    tap(160, 69, 3100);  // Atmung messen
    CHECK(app.measuring && app.screen == APP_BREATH_LIVE);
}

static void test_breath(void) {
    uint32_t seq;
    breath_measure(0.25, 0.06, 0.15, &seq);
    run_blocks(3.0, 40.0, 70.0, 500, &seq, 0);
    CHECK(app.bl.n > 30 && !app.bl.has_est);  // erst ab 45 s eine Schätzung
    run_blocks(40.0, 110.0, 70.0, 500, &seq, 0);
    CHECK(app.bl.has_est && fabs(app.bl.est_hz * 60.0 - 15.0) < 2.0);
    run_blocks(110.0, 3.0 + ANA_MAX_S + 2.0, 70.0, 500, &seq, 0);
    CHECK(app.screen == APP_RESULT && app.result_kind == 1 && app.has_breath);
    CHECK(app.breath.why == BR_OK && fabs(app.breath.rate_hz * 60.0 - 15.0) < 1.0);
    CHECK(app.breath.has_swing && app.breath.swing_bpm > 1.0);
    CHECK(app.breath.curve_n > 440 && app.breath.n_cycles >= 26 && app.breath.n_cycles <= 33);
    CHECK(app.breath.cycle_cv >= 0.0 && app.breath.cycle_cv < 0.15);
    CHECK(!app.has_result);  // die Auswertung hat das EKG-Ergebnis ersetzt
    // Seiten: Übersicht, Kurve, Hinweise
    for (int i = 0; i < 5; i++) {
        app_event(&app, (ui_event_t){EV_SWIPE_LEFT, 0, 0, 0, 0, true}, 131000 + (uint32_t)i);
    }
    CHECK(app.page == 2 && result_pages(&app) == 3);
    // zurück zur Kurve, Menü: „Letztes Ergebnis“ im Atem-Modus
    tap(100, 222, 131100);
    CHECK(app.screen == APP_LIVE);
    tap(160, 120, 131200);
    tap(60, 118, 131210);  // links in der Zeile, Modus ist noch Atmung
    CHECK(app.screen == APP_RESULT && app.result_kind == 1 && app.page == 0);
    tap(310, 12, 131300);
    CHECK(app.screen == APP_LIVE);
    // EKG-Modus: kein letztes EKG-Ergebnis (ersetzt)
    tap(160, 120, 131400);
    tap(60, 28, 131410);  // Umschalter EKG
    CHECK(app.mode == MODE_EKG);
    tap(160, 118, 131420);  // „Letztes Ergebnis“ ist gesperrt
    CHECK(app.screen == APP_MENU);
    tap(310, 12, 131500);

    // ohne Atemschwankung gibt es keinen Wert, aber ein Ergebnis mit Begründung
    breath_measure(0.25, 0.0, 0.0, &seq);
    run_blocks(3.0, 3.0 + ANA_MAX_S + 2.0, 70.0, 500, &seq, 0);
    CHECK(app.screen == APP_RESULT && app.has_breath && app.breath.why != BR_OK);
    ecg_fn = ecg_mv;

    // Stopp vor 30 s: zu kurz, nichts wird gespeichert
    breath_measure(0.25, 0.06, 0.15, &seq);
    run_blocks(3.0, 15.0, 70.0, 500, &seq, 0);
    tap(300, 12, 15000);
    CHECK(!app.measuring && app.screen == APP_LIVE && app.toast_shown);
    ecg_fn = ecg_mv;
}

static void test_coach(void) {
    setup_app();
    uint32_t seq = 0;
    run_blocks(0.0, 3.0, 66.0, 500, &seq, 0);
    tap(160, 120, 3000);
    tap(136, 28, 3010);  // Atmung
    app.req_save = false;
    tap(230, 118, 3100);  // Atem-Coach
    CHECK(app.screen == APP_COACH && app.coach.tempo == 6 && app.coach.dur_min == 2);
    rect_t d[16];
    for (uint32_t t = 3200; t < 8200; t += 100) {  // 5 s: Einatmen
        app_frame(&app, t, d, 16);
    }
    CHECK(app.coach.n_pulse >= 4);
    tap(297, 69, 8300);  // Tempo +
    CHECK(app.coach.tempo == 7);
    tap(207, 69, 8310);  // Tempo −
    tap(207, 69, 8320);
    CHECK(app.coach.tempo == 5);
    tap(283, 122, 8330);  // 5 min
    CHECK(app.coach.dur_min == 5 && app.screen == APP_COACH);
    for (uint32_t t = 8400; t < 8330 + 5u * 60000u - 500u; t += 500) {
        app_frame(&app, t, d, 16);
    }
    CHECK(app.screen == APP_COACH);
    for (uint32_t t = 8330 + 5u * 60000u - 500u; t < 8330 + 5u * 60000u + 1500u; t += 100) {
        app_frame(&app, t, d, 16);
    }
    CHECK(app.screen == APP_MENU && app.toast_shown && strstr(app.toast, "5 min"));
    CHECK(!app.req_save);  // nichts wird gespeichert
    // abbrechen mit dem Kreuz
    tap(160, 118, 400000);
    CHECK(app.screen == APP_MENU);  // in der Mitte zwischen den Hälften: nichts
    tap(230, 118, 400100);
    CHECK(app.screen == APP_COACH);
    tap(310, 12, 400200);
    CHECK(app.screen == APP_MENU);
}

// ------------------------------------------------------------------ Privater Bereich
static void test_private_data(void) {
    // SHA-256 mit bekannten Werten
    uint8_t h[32];
    priv_sha256((const uint8_t *)"abc", 3, h);
    const uint8_t abc[8] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea};
    CHECK(memcmp(h, abc, 8) == 0 && h[31] == 0xad);
    priv_sha256((const uint8_t *)"", 0, h);
    CHECK(h[0] == 0xe3 && h[1] == 0xb0 && h[31] == 0x55);
    char long_msg[200];
    memset(long_msg, 'a', sizeof long_msg);
    priv_sha256((const uint8_t *)long_msg, sizeof long_msg, h);  // über mehrere Blöcke
    CHECK(h[0] != 0 || h[1] != 0);

    priv_data_t d, e;
    priv_clear(&d);
    CHECK(!priv_check_pin(&d, "0000"));
    priv_set_pin(&d, "4711");
    CHECK(priv_check_pin(&d, "4711") && !priv_check_pin(&d, "4712") && !priv_check_pin(&d, "1147"));
    priv_session_t s1 = {.duration_s = 600, .hr_start = 70, .hr_peak = 125};
    for (int i = 0; i < PRIV_SESSIONS + 3; i++) {
        s1.hr_peak = (uint8_t)(100 + i);
        priv_add_session(&d, &s1);
    }
    CHECK(d.n == PRIV_SESSIONS && d.s[0].hr_peak == 103 && d.s[PRIV_SESSIONS - 1].hr_peak == 122);
    d.display_mode = 1;
    uint8_t buf[PRIV_RECORD_BYTES];
    CHECK(priv_encode(&d, buf, sizeof buf) == PRIV_RECORD_BYTES);
    CHECK(priv_encode(&d, buf, 100) == 0);
    priv_decode(buf, &e);
    CHECK(e.has_pin && e.display_mode == 1 && e.n == PRIV_SESSIONS);
    CHECK(priv_check_pin(&e, "4711") && e.s[5].hr_peak == 108);
    // beschädigt: leer, ohne PIN
    buf[100] ^= 1;
    priv_decode(buf, &e);
    CHECK(!e.has_pin && e.n == 0);
    memset(buf, 0xFF, sizeof buf);  // leerer Flash
    priv_decode(buf, &e);
    CHECK(!e.has_pin && e.n == 0);
}

static void test_solo(void) {
    priv_data_t d;
    priv_clear(&d);
    solo_model_t m;
    solo_build(&d, &m);
    CHECK(!m.learned && m.n == 0 && solo_percent(&m, 70, 100) == -1);
    const int peaks[] = {120, 124, 126};
    for (int i = 0; i < 3; i++) {
        priv_session_t x = {.duration_s = (uint16_t)(540 + 60 * i), .hr_start = 70,
                            .hr_peak = (uint8_t)peaks[i]};
        priv_add_session(&d, &x);
        solo_build(&d, &m);
        CHECK(m.n == i + 1 && m.learned == (i >= 2));
    }
    CHECK(m.confident && fabsf(m.gain - 54.0f) < 0.01f && m.peak == 124.0f);
    CHECK(m.t_med == 600 && m.t_lo == 570 && m.t_hi == 630);
    CHECK(solo_percent(&m, 70, 70) == 0 && solo_percent(&m, 70, 97) == 50);
    CHECK(solo_percent(&m, 70, 200) == 100 && solo_percent(&m, 70, 60) == 0);
    // nicht gewertete Sessions zählen nicht
    priv_session_t bad = {.duration_s = 100, .hr_start = 70, .hr_peak = 71, .flags = PRIV_EXCLUDED};
    priv_add_session(&d, &bad);
    solo_build(&d, &m);
    CHECK(m.n == 3 && fabsf(m.gain - 54.0f) < 0.01f);
    // stark verschiedene Anstiege: keine Prozentanzeige
    priv_clear(&d);
    const int pk[] = {90, 140, 105};
    for (int i = 0; i < 3; i++) {
        priv_session_t x = {.duration_s = 600, .hr_start = 70, .hr_peak = (uint8_t)pk[i]};
        priv_add_session(&d, &x);
    }
    solo_build(&d, &m);
    CHECK(m.learned && !m.confident && solo_percent(&m, 70, 100) == -1);

    // Auswertung einer Session aus dem Puls je Sekunde
    static uint8_t hr[700];
    for (int i = 0; i < 600; i++) {
        hr[i] = (uint8_t)(70 + (i < 60 ? 0 : (i - 60) * 55 / 540));
    }
    for (int i = 100; i < 110; i++) {
        hr[i] = 0;  // Lücke
    }
    priv_session_t out;
    CHECK(solo_summarize(hr, 600, &out));
    CHECK(out.duration_s == 600 && out.hr_start == 70 && out.hr_peak >= 123 && out.hr_peak <= 125);
    CHECK(out.curve[0] == 70 && out.curve[PRIV_CURVE - 1] >= 120);
    CHECK(fabsf(solo_start(hr, 600) - 70.0f) < 0.01f);
    const int t90 = solo_t90(hr, 600, 70.0f, 55.0f);
    CHECK(t90 > 520 && t90 < 560);
    CHECK(solo_t90(hr, 100, 70.0f, 200.0f) == -1);
    CHECK(!solo_summarize(hr, 10, &out));  // zu wenige Werte
    memset(hr, 0, sizeof hr);
    CHECK(!solo_summarize(hr, 300, &out));
}

// Tastenfeld der PIN-Eingabe (quer): Mitte der Taste
static void pad(char k, uint32_t now) {
    const char *keys = "789456123<0K";
    const int i = (int)(strchr(keys, k) - keys);
    tap(24 + (i % 3) * 91 + 45, 70 + (i / 3) * 42 + 21, now);
}

static void pad_pin(const char *digits, uint32_t now) {
    for (int i = 0; digits[i]; i++) {
        pad(digits[i], now + (uint32_t)i * 20u);
    }
    pad('K', now + 200u);
}

static void secret_taps(uint32_t t0, int n, uint32_t gap) {
    for (int i = 0; i < n; i++) {
        tap(160, 45, t0 + (uint32_t)i * gap);
    }
}

static void to_info(uint32_t t0) {
    tap(160, 120, t0);
    tap(90, 166, t0 + 10);
    tap(160, 223, t0 + 20);
    CHECK(app.screen == APP_INFO);
}

static double T;  // Zeit der Ablauf-Tests in s

static uint32_t seq_priv;

static void advance_seq(double dt) {
    run_blocks(T, T + dt, 70.0, 500, &seq_priv, 0);
    T += dt;
}

static void make_ramp(double t_from, double t_start, double dur, double hr0, double hr1) {
    n_beats_t = 0;
    beat_cursor = 0;
    g_amp_mod = 0.0;
    for (double t = t_from; t < t_start + dur + 120.0 && n_beats_t < 2400; n_beats_t++) {
        beats_t[n_beats_t] = t;
        double u = (t - t_start) / dur;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        t += 60.0 / (hr0 + (hr1 - hr0) * u * u);
    }
}

// Eine Session von 200 s: 40 s Ruhe, dann steigender Puls bis peak; speichert wie angegeben
static void solo_session(int peak, bool counted, bool check_percent) {
    make_ramp(T, T + 40.0, 155.0, 70.0, (double)peak);
    const double t0 = T;
    tap(160, 69, (uint32_t)(T * 1000.0));  // Solo-Session starten
    CHECK(app.screen == APP_PRIV_SESSION);
    advance_seq(179.0);
    if (check_percent) {
        CHECK(app.priv.model.confident);
        CHECK(app.priv.percent >= 70 && app.priv.percent <= 95);
    }
    advance_seq(t0 + 200.0 - T);
    tap(160, 212, (uint32_t)(T * 1000.0));  // Fertig
    CHECK(app.screen == APP_PRIV_DONE);
    CHECK(app.priv.last.duration_s >= 199 && app.priv.last.duration_s <= 201);
    CHECK(abs(app.priv.last.hr_start - 70) <= 3 && abs(app.priv.last.hr_peak - peak) <= 6);
    tap(counted ? 240 : 80, 216, (uint32_t)(T * 1000.0));
    CHECK(app.screen == APP_PRIV_HOME && app.req_priv_save);
    app.req_priv_save = false;
}

static void test_private_flow(void) {
    make_ramp(0.4, 1e9, 1.0, 70.0, 70.0);
    ecg_fn = breath_ecg;
    setup_app();
    seq_priv = 0;
    T = 0.0;
    advance_seq(3.0);

    // Zugang: vier schnelle Tipps genügen nicht, ein langsamer fünfter auch nicht
    to_info(3000);
    secret_taps(3100, 4, 150);
    tap(160, 45, 5200);  // zu spät: zählt neu
    CHECK(app.screen == APP_INFO);
    tap(310, 12, 6000);  // × oben rechts liegt außerhalb der Versionszeile
    CHECK(app.screen == APP_SETTINGS);
    tap(160, 223, 6100);
    secret_taps(6200, 5, 150);
    CHECK(app.screen == APP_PRIV_PIN && app.priv.pin_mode == 1);  // PIN festlegen

    // PIN festlegen: zweite Eingabe abweichend, dann richtig
    pad_pin("1234", 7000);
    CHECK(app.priv.pin_mode == 2);
    pad_pin("1235", 7500);
    CHECK(app.priv.pin_mode == 1 && !pdata.has_pin && app.toast_shown);
    pad_pin("1234", 8000);
    pad_pin("1234", 8500);
    CHECK(app.screen == APP_PRIV_HOME && pdata.has_pin && app.req_priv_save);
    CHECK(priv_check_pin(&pdata, "1234"));
    app.req_priv_save = false;

    // Verlassen, neu öffnen: PIN wird verlangt; falsche Eingaben sperren
    tap(310, 12, 9000);
    CHECK(app.screen == APP_LIVE);
    to_info(9100);
    secret_taps(9200, 5, 150);
    CHECK(app.screen == APP_PRIV_PIN && app.priv.pin_mode == 0);
    pad_pin("1111", 10000);
    pad_pin("2222", 10500);
    CHECK(app.screen == APP_PRIV_PIN && app.priv.fails == 2);
    pad_pin("3333", 11000);
    CHECK(app.priv.locked_until > 11000u);
    pad_pin("1234", 12000);  // gesperrt: auch die richtige PIN zählt nicht
    CHECK(app.screen == APP_PRIV_PIN);
    pad_pin("1234", 42000);
    CHECK(app.screen == APP_PRIV_HOME);

    // Sessions: drei lernen, die vierte zeigt die Annäherung
    T = 43.0;
    advance_seq(5.0);
    CHECK(!app.usb);
    const int peaks[] = {120, 124, 126, 122};
    for (int i = 0; i < 4; i++) {
        solo_session(peaks[i], true, i == 3);
        CHECK(pdata.n == i + 1);
        advance_seq(5.0);
    }
    solo_build(&pdata, &app.priv.model);
    CHECK(app.priv.model.n == 4 && app.priv.model.learned);

    // „Nicht werten“ speichert, geht aber nicht ins Modell ein
    solo_session(140, false, false);
    solo_build(&pdata, &app.priv.model);
    CHECK(pdata.n == 5 && (pdata.s[4].flags & PRIV_EXCLUDED) && app.priv.model.n == 4);
    advance_seq(5.0);

    // USB steckt: Start gesperrt; kommt USB während der Session, bricht sie ab
    app_set_status(&app, true, false);
    tap(160, 69, (uint32_t)(T * 1000.0));
    CHECK(app.screen == APP_PRIV_HOME && app.toast_shown);
    app_set_status(&app, false, false);
    advance_seq(3.0);
    make_ramp(T, T + 40.0, 155.0, 70.0, 120.0);
    tap(160, 69, (uint32_t)(T * 1000.0));
    CHECK(app.screen == APP_PRIV_SESSION);
    advance_seq(30.0);
    app_set_status(&app, true, false);
    advance_seq(2.0);
    CHECK(app.screen == APP_PRIV_HOME && pdata.n == 5);
    app_set_status(&app, false, false);
    advance_seq(60.0);  // Toast läuft ab

    // Verlauf, Anzeige, Löschen
    tap(80, 176, (uint32_t)(T * 1000.0));
    CHECK(app.screen == APP_PRIV_HISTORY);
    tap(310, 12, (uint32_t)(T * 1000.0 + 10));
    CHECK(app.screen == APP_PRIV_HOME);
    tap(236, 176, (uint32_t)(T * 1000.0 + 20));
    CHECK(pdata.display_mode == 1 && app.req_priv_save);
    tap(236, 216, (uint32_t)(T * 1000.0 + 30));  // Daten löschen: erst die Rückfrage
    CHECK(app.priv.prompt == 1 && pdata.n == 5);
    tap(100, 155, (uint32_t)(T * 1000.0 + 40));  // Abbrechen
    CHECK(app.priv.prompt == 0 && pdata.n == 5);
    tap(236, 216, (uint32_t)(T * 1000.0 + 50));
    tap(240, 155, (uint32_t)(T * 1000.0 + 60));  // Löschen
    CHECK(pdata.n == 0 && pdata.has_pin);

    // PIN ändern aus dem Bereich heraus
    tap(80, 216, (uint32_t)(T * 1000.0 + 70));
    CHECK(app.screen == APP_PRIV_PIN && app.priv.pin_mode == 1 && app.priv.changing);
    pad_pin("9876", (uint32_t)(T * 1000.0 + 100));
    pad_pin("9876", (uint32_t)(T * 1000.0 + 600));
    CHECK(app.screen == APP_PRIV_HOME && priv_check_pin(&pdata, "9876") && !priv_check_pin(&pdata, "1234"));

    // PIN vergessen: ‹ lange halten, bestätigen: alles gelöscht, neue PIN
    tap(310, 12, (uint32_t)(T * 1000.0 + 2000));
    to_info((uint32_t)(T * 1000.0 + 3000));
    secret_taps((uint32_t)(T * 1000.0 + 3100), 5, 150);
    CHECK(app.screen == APP_PRIV_PIN && app.priv.pin_mode == 0);
    app_event(&app, (ui_event_t){EV_LONG, 69, 217, 0, 0, true}, (uint32_t)(T * 1000.0 + 4500));
    CHECK(app.priv.prompt == 2);
    tap(240, 155, (uint32_t)(T * 1000.0 + 4600));
    CHECK(!pdata.has_pin && app.screen == APP_PRIV_PIN && app.priv.pin_mode == 1);
    ecg_fn = ecg_mv;
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
    test_avatar();
    test_leads();
    test_breath();
    test_coach();
    test_private_data();
    test_solo();
    test_private_flow();
    if (failures) {
        fprintf(stderr, "%d Fehler\n", failures);
        return 1;
    }
    puts("alle UI-Tests bestanden");
    return 0;
}
