#include "display_task.h"

#include <stdio.h>

#include "app.h"
#include "feed.h"
#include "gesture.h"
#include "hardware/sync.h"
#include "hmi.h"
#include "ili9341.h"
#include "monitor.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "private_store.h"
#include "protocol.h"
#include "settings.h"
#include "splash.h"
#include "theme.h"
#include "touchcal.h"

#define FRAME_MS 33u
#define TOUCH_MS 20u
#define RECAL_MS 6000u  // so lange gedrückt halten: Touch neu einrichten

static uint16_t fb[SCREEN_W * SCREEN_H];
static canvas_t canvas = {fb, SCREEN_W, SCREEN_H};  // hochkant 240 × 320 (gleich viele Pixel)
static monitor_t mon;
static ana_recording_t rec;  // 2 min Rohdaten für die Auswertung
static ana_result_t res;
static app_t app;
static gesture_t gest;
static uint32_t core1_stack[4096];  // 16 KB: QRS-Detektor und Auswertung brauchen mehr als 2 KB
static ui_settings_t ui;
static priv_data_t pdata;
static char version[16];

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void drain_feed(void) {
    feed_block_t b;
    while (feed_pop(&b)) {
        app_feed(&app, b.raw, b.n, b.flags, b.factor, b.seq);
    }
}

static void push_all(void) {
    ili_push_rect(fb, canvas.w, 0, 0, canvas.w, canvas.h);
}

static void apply_requests(const rect_t *dirty, int n, uint32_t now) {
    if (app.req_rotate) {
        app.req_rotate = false;
        ili_set_rotation(ui.rotation);
        app.req_push_all = true;
    }
    if (app.req_backlight >= 0) {
        backlight_set(app.req_backlight);
        app.req_backlight = -1;
    }
    if (app.req_save) {
        app.req_save = false;
        settings_request_save(&ui, now);
    }
    if (app.req_priv_save) {
        app.req_priv_save = false;
        priv_request_save(app.pd, now);
    }
    if (app.req_push_all) {
        app.req_push_all = false;
        push_all();
        return;
    }
    for (int i = 0; i < n; i++) {
        ili_push_rect(fb, canvas.w, dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
    }
}

static void poll_touch(uint32_t now) {
    static uint32_t pressed_since;
    static bool was_pressed, recal_done;
    touch_sample_t s;
    touch_read(&s);
    int x = 0, y = 0;
    if (s.valid && ui.has_cal) {
        int xl, yl;
        touchcal_apply(&ui.cal, s.rx, s.ry, &xl, &yl);  // Queransicht
        touch_land_to_screen(ui.rotation == 1, xl, yl, &x, &y);
    }
    if (s.pressed && !was_pressed) {
        pressed_since = now;
        recal_done = false;
    }
    was_pressed = s.pressed;
    if (s.pressed && !recal_done && now - pressed_since >= RECAL_MS && app.screen != APP_CAL) {
        // Notausgang bei falscher Kalibrierung: lange halten richtet den Touch neu ein
        recal_done = true;
        gesture_init(&gest);
        app_calibrate(&app);
        apply_requests(NULL, 0, now);
        return;
    }
    ui_event_t ev = gesture_feed(&gest, s.pressed, s.valid, x, y, s.rx, s.ry, now);
    if (ev.type != EV_NONE && !recal_done) {
        app_event(&app, ev, now);
        apply_requests(NULL, 0, now);
    }
}

static void core1_main(void) {
    flash_safe_execute_core_init();  // Kern 0 darf beim Speichern der Einstellungen anhalten
    settings_load(&ui);
    priv_load(&pdata);
    hmi_init();
    backlight_set(0);
    ili_init(ui.rotation);
    g_link.display_id = ili_read_id();
    __dmb();
    g_link.display_up = true;

    snprintf(version, sizeof version, "%u.%u.%u", EKG_FW_VERSION_MAJOR, EKG_FW_VERSION_MINOR,
             EKG_FW_VERSION_PATCH);
    canvas.w = ui.rotation == 1 ? SCREEN_H : SCREEN_W;
    canvas.h = ui.rotation == 1 ? SCREEN_W : SCREEN_H;
    canvas_t *c = &canvas;
    monitor_init(&mon, c);
    mon.draw = false;  // während des Startbildschirms nur rechnen (Erkennung lernt schon)
    app.c = c;
    app.mon = &mon;
    app.rec = &rec;
    app.res = &res;
    app.ui = &ui;
    app.pd = &pdata;
    app.scratch = fb;
    app.scratch_size = sizeof fb;
    app.version = version;
    app.display_id = g_link.display_id;
    app_init(&app);  // setzt das Farbschema der gespeicherten Stufe
    gesture_init(&gest);

    uint32_t t0 = now_ms();
    for (uint32_t t = 0; t < SPLASH_DURATION_MS; t = now_ms() - t0) {
        splash_render(c, t, version);
        push_all();
        backlight_set(splash_backlight(t) * app_backlight(&app) / 1000);
        drain_feed();
    }

    uint32_t now = now_ms();
    uint32_t vsys_seq = 0;
    app_set_status(&app, g_link.usb_mounted, g_link.streaming);
    app_start(&app, true, now);
    app.req_backlight = app_backlight(&app);
    apply_requests(NULL, 0, now);
    uint32_t last_frame = 0, last_touch = 0;
    for (;;) {
        drain_feed();
        now = now_ms();
        if (now - last_touch >= TOUCH_MS) {
            last_touch = now;
            poll_touch(now);
        }
        if (now - last_frame >= FRAME_MS) {
            last_frame = now;
            app_set_status(&app, g_link.usb_mounted, g_link.streaming);
            if (g_link.vsys_seq != vsys_seq) {
                vsys_seq = g_link.vsys_seq;
                __dmb();
                app_vsys_sample(&app, g_link.vsys_mv, g_link.vsys_usb);
            }
            g_link.no_pause = app.measuring;
            rect_t dirty[16];
            int n = app_frame(&app, now, dirty, 16);
            apply_requests(dirty, n, now);
        }
    }
}

void display_launch(void) {
    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
}
