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

static void setup_app(void) {
    memset(&app, 0, sizeof app);
    memset(&ui, 0, sizeof ui);
    canvas_t *c = malloc(sizeof *c);
    *c = (canvas_t){fb, SCREEN_W, SCREEN_H};
    monitor_init(&mon, c);
    ui.has_cal = true;
    ui.look = 1;
    app.c = c;
    app.mon = &mon;
    app.rec = &rec;
    app.res = &res;
    app.ui = &ui;
    app.scratch = fb;
    app.scratch_size = sizeof fb;
    app.version = "test";
    app_init(&app);
    app_set_status(&app, false, false, 4300);
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
    run_blocks(3.0, 3.0 + ANA_MAX_S + 1.0, 72.0, rate, &seq, skip_every);
    // nach 2:10 voll: erst „läuft“, im nächsten Bild die Auswertung
    CHECK(!app.measuring);
    run_blocks(134.0, 135.0, 72.0, rate, &seq, 0);
    CHECK(app.screen == APP_RESULT);
    CHECK(app.has_result && app.result_status == ANA_OK);
    CHECK(res.ok && res.has_hrv);
    CHECK(fabs(res.hrv.mean_hr - 72.0) < 1.0);
    CHECK(res.n_beats > (skip_every ? 140 : 150) && res.n_beats < 160);
    CHECK(res.has_median && res.has_delineation);
    if (skip_every) {
        CHECK(res.gap_s > 1.0);
    } else {
        CHECK(res.gap_s == 0.0);
    }
    // Seiten durchblättern und zurück zur Live-Ansicht
    for (int i = 0; i < APP_PAGES; i++) {
        app_event(&app, (ui_event_t){EV_SWIPE_LEFT, 0, 0, 0, 0, true}, 136000);
    }
    CHECK(app.page == APP_PAGES - 1);
    tap(100, 222, 136100);  // „Live“
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
    if (failures) {
        fprintf(stderr, "%d Fehler\n", failures);
        return 1;
    }
    puts("alle UI-Tests bestanden");
    return 0;
}
