// Vorschau der Display-Bildschirme am Laptop: rendert mit demselben Code wie das Gerät
// in PPM-Dateien. Aufruf über tools/display_preview.py.
//
//   display_preview [tag] splash <t_ms> <out.ppm>
//   display_preview [tag] monitor <blocks.bin> <out_prefix> <t_ms> [<t_ms> …]
//   display_preview [tag] app <blocks.bin> <out_prefix> <script>
//
// app: spielt die Bedienung mit eingerichtetem Touch durch. Skript je Zeile:
//   <t_ms> tap <x> <y> | long | swipe_l | swipe_r | shot <name>
//
// „tag“ wählt das helle Farbschema, sonst wird das dunkle gezeichnet.
//
// blocks.bin: je Block uint8 n, uint8 flags, uint16 Faktor, uint32 Zeit (ms), n × uint32 Rohwert

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "monitor.h"
#include "protocol.h"
#include "splash.h"
#include "theme.h"

static uint16_t px[SCREEN_W * SCREEN_H];

static void write_ppm(const char *path, const canvas_t *c) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (int i = 0; i < c->w * c->h; i++) {
        uint16_t p = c->px[i];
        unsigned char rgb[3] = {(unsigned char)(((p >> 11) & 31) * 255 / 31),
                                (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                (unsigned char)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static monitor_t mon;
static ana_recording_t rec;
static ana_result_t res;
static ui_settings_t ui;
static app_t app;

typedef struct {
    uint32_t t;
    char what[16];
    int x, y;
    char name[64];
} step_t;

static int run_app(const char *bins, const char *prefix, const char *script, const char *ver) {
    FILE *f = fopen(bins, "rb");
    FILE *sf = fopen(script, "r");
    if (!f || !sf) {
        perror("app");
        return 1;
    }
    static step_t steps[256];
    int ns = 0;
    char line[256];
    while (ns < 256 && fgets(line, sizeof line, sf)) {
        step_t *st = &steps[ns];
        memset(st, 0, sizeof *st);
        unsigned t;
        if (sscanf(line, "%u %15s", &t, st->what) < 2) {
            continue;
        }
        st->t = t;
        if (strcmp(st->what, "tap") == 0) {
            sscanf(line, "%*u %*s %d %d", &st->x, &st->y);
        } else if (strcmp(st->what, "shot") == 0) {
            sscanf(line, "%*u %*s %63s", st->name);
        }
        ns++;
    }
    fclose(sf);
    canvas_t c = {px, SCREEN_W, SCREEN_H};
    monitor_init(&mon, &c);
    ui.has_cal = true;
    ui.look = g_theme == &THEME_DAY ? 0 : 1;
    app.c = &c;
    app.mon = &mon;
    app.rec = &rec;
    app.res = &res;
    app.ui = &ui;
    app.scratch = px;
    app.scratch_size = sizeof px;
    app.version = ver;
    app.display_id = 0x9341u;
    app_init(&app);
    app_set_status(&app, false, false, 4380);
    app_start(&app, true, 0);
    int next = 0;
    uint32_t last_frame = 0, seq = 0;
    for (;;) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) {
            break;
        }
        int n = hdr[0];
        uint8_t flags = hdr[1];
        uint32_t factor = (uint32_t)(hdr[2] | hdr[3] << 8);
        uint32_t t = (uint32_t)(hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24);
        uint32_t raw[64];
        if (fread(raw, 4, (size_t)n, f) != (size_t)n) {
            break;
        }
        if (app.screen != APP_ANALYZING) {
            app_feed(&app, raw, n, flags, factor, seq++);
        } else {
            seq++;  // während der Auswertung kommt das Display nicht dazu
        }
        while (next < ns && t >= steps[next].t) {
            step_t *st = &steps[next++];
            ui_event_t ev = {EV_NONE, st->x, st->y, 0, 0, true};
            if (strcmp(st->what, "tap") == 0) {
                ev.type = EV_TAP;
            } else if (strcmp(st->what, "long") == 0) {
                ev.type = EV_LONG;
            } else if (strcmp(st->what, "swipe_l") == 0) {
                ev.type = EV_SWIPE_LEFT;
            } else if (strcmp(st->what, "swipe_r") == 0) {
                ev.type = EV_SWIPE_RIGHT;
            } else if (strcmp(st->what, "shot") == 0) {
                char path[512];
                snprintf(path, sizeof path, "%s_%s.ppm", prefix, st->name);
                write_ppm(path, &c);
                continue;
            }
            app_event(&app, ev, t);
        }
        if (t - last_frame >= 33) {
            rect_t dirty[16];
            app_frame(&app, t, dirty, 16);
            app.req_push_all = app.req_save = app.req_rotate = false;
            app.req_backlight = -1;
            last_frame = t;
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    canvas_t c = {px, SCREEN_W, SCREEN_H};
    if (argc > 1 && strcmp(argv[1], "tag") == 0) {
        g_theme = &THEME_DAY;
        argc--;
        argv++;
    }
    char version[16];
    snprintf(version, sizeof version, "%u.%u.%u", EKG_FW_VERSION_MAJOR, EKG_FW_VERSION_MINOR,
             EKG_FW_VERSION_PATCH);
    if (argc == 5 && strcmp(argv[1], "app") == 0) {
        return run_app(argv[2], argv[3], argv[4], version);
    }
    if (argc == 4 && strcmp(argv[1], "splash") == 0) {
        splash_render(&c, (uint32_t)atoi(argv[2]), version);
        write_ppm(argv[3], &c);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "monitor") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            perror(argv[2]);
            return 1;
        }
        monitor_init(&mon, &c);
        monitor_set_link(&mon, true, false);
        monitor_draw_all(&mon);
        int next_shot = 4;
        uint32_t shot_t = (uint32_t)atoi(argv[next_shot]);
        uint32_t last_frame = 0;
        for (;;) {
            unsigned char hdr[8];
            if (fread(hdr, 1, 8, f) != 8) {
                break;
            }
            int n = hdr[0];
            uint8_t flags = hdr[1];
            uint32_t factor = (uint32_t)(hdr[2] | hdr[3] << 8);
            uint32_t t = (uint32_t)(hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24);
            uint32_t raw[64];
            if (fread(raw, 4, (size_t)n, f) != (size_t)n) {
                break;
            }
            monitor_feed(&mon, raw, n, flags, factor);
            if (t - last_frame >= 33) {
                rect_t dirty[16];
                monitor_frame(&mon, t, dirty, 16);
                last_frame = t;
            }
            while (next_shot < argc && t >= shot_t) {
                char path[512];
                snprintf(path, sizeof path, "%s_%05u.ppm", argv[3], shot_t);
                write_ppm(path, &c);
                if (++next_shot < argc) {
                    shot_t = (uint32_t)atoi(argv[next_shot]);
                }
            }
        }
        fclose(f);
        return 0;
    }
    fprintf(stderr, "Aufruf: display_preview [tag] splash <t> <out> | monitor <bin> <prefix> <t…> | "
                    "app <bin> <prefix> <skript>\n");
    return 2;
}
