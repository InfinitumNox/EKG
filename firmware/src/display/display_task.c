#include "display_task.h"

#include <stdio.h>

#include "feed.h"
#include "hardware/sync.h"
#include "hmi.h"
#include "ili9341.h"
#include "monitor.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "protocol.h"
#include "settings.h"
#include "splash.h"
#include "theme.h"

#define FRAME_MS 33u
#define TAP_MIN_MS 40u
#define TAP_MAX_MS 800u
#define LONG_PRESS_MS 2000u

// Tippen schaltet der Reihe nach: Tag (hell, für Tageslicht), dann Nacht mit 100/50/20 %
typedef struct {
    const theme_t *theme;
    int backlight;  // Promille
} look_t;
static const look_t LOOKS[] = {
    {&THEME_DAY, 1000},
    {&THEME_NIGHT, 1000},
    {&THEME_NIGHT, 500},
    {&THEME_NIGHT, 200},
};
#define N_LOOKS (sizeof LOOKS / sizeof LOOKS[0])

static uint16_t fb[SCREEN_W * SCREEN_H];
static monitor_t mon;
static uint32_t core1_stack[4096];  // 16 KB: der QRS-Detektor braucht mehr als die üblichen 2 KB
static ui_settings_t ui;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void drain_feed(void) {
    feed_block_t b;
    while (feed_pop(&b)) {
        monitor_feed(&mon, b.raw, b.n, b.flags, b.factor);
    }
}

static void push_all(void) {
    ili_push_rect(fb, SCREEN_W, 0, 0, SCREEN_W, SCREEN_H);
}

static void handle_touch(uint32_t now) {
    static bool was;
    static bool long_done;
    static uint32_t since;
    bool p = touch_pressed();
    if (p && !was) {
        since = now;
        long_done = false;
    }
    if (p && !long_done && now - since >= LONG_PRESS_MS) {
        long_done = true;  // 2 s gehalten: Bild drehen
        ui.rotation ^= 1u;
        ili_set_rotation(ui.rotation);
        monitor_draw_all(&mon);
        push_all();
        settings_request_save(&ui, now);
    }
    if (!p && was && !long_done) {
        uint32_t d = now - since;
        if (d >= TAP_MIN_MS && d <= TAP_MAX_MS) {  // kurz getippt: nächste Anzeigestufe
            const theme_t *before = LOOKS[ui.look].theme;
            ui.look = (uint8_t)((ui.look + 1u) % N_LOOKS);
            if (LOOKS[ui.look].theme != before) {
                g_theme = LOOKS[ui.look].theme;
                monitor_draw_all(&mon);
                push_all();
            }
            backlight_set(LOOKS[ui.look].backlight);
            settings_request_save(&ui, now);
        }
    }
    was = p;
}

static void core1_main(void) {
    flash_safe_execute_core_init();  // Kern 0 darf beim Speichern der Einstellungen anhalten
    settings_load(&ui);
    g_theme = LOOKS[ui.look].theme;
    hmi_init();
    backlight_set(0);
    ili_init(ui.rotation);
    g_link.display_id = ili_read_id();
    __dmb();
    g_link.display_up = true;

    canvas_t c = {fb, SCREEN_W, SCREEN_H};
    monitor_init(&mon, &c);
    mon.draw = false;  // während des Startbildschirms nur rechnen (Erkennung lernt schon)

    char version[16];
    snprintf(version, sizeof version, "%u.%u.%u", EKG_FW_VERSION_MAJOR, EKG_FW_VERSION_MINOR,
             EKG_FW_VERSION_PATCH);
    uint32_t t0 = now_ms();
    for (uint32_t t = 0; t < SPLASH_DURATION_MS; t = now_ms() - t0) {
        splash_render(&c, t, version);
        push_all();
        backlight_set(splash_backlight(t) * LOOKS[ui.look].backlight / 1000);
        drain_feed();
    }

    mon.draw = true;
    monitor_set_link(&mon, g_link.usb_mounted, g_link.streaming);
    monitor_draw_all(&mon);
    backlight_set(LOOKS[ui.look].backlight);
    uint32_t last = 0;
    for (;;) {
        drain_feed();
        uint32_t now = now_ms();
        handle_touch(now);
        if (now - last >= FRAME_MS) {
            last = now;
            monitor_set_link(&mon, g_link.usb_mounted, g_link.streaming);
            rect_t dirty[16];
            int n = monitor_frame(&mon, now, dirty, 16);
            for (int i = 0; i < n; i++) {
                ili_push_rect(fb, SCREEN_W, dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
            }
        }
    }
}

void display_launch(void) {
    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
}
