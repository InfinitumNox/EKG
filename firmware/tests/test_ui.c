// Host-Tests der Bedienung: Touch-Kalibrierung, Gesten, Batterieanzeige und der Ablauf
// Menü -> Messung -> Auswertung mit einem künstlichen EKG.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "gesture.h"
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
    if (failures) {
        fprintf(stderr, "%d Fehler\n", failures);
        return 1;
    }
    puts("alle UI-Tests bestanden");
    return 0;
}
