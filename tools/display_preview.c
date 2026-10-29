// Vorschau der Display-Bildschirme am Laptop: rendert mit demselben Code wie das Gerät
// in PPM-Dateien. Aufruf über tools/display_preview.py.
//
//   display_preview [tag] splash <t_ms> <out.ppm>
//   display_preview [tag] monitor <blocks.bin> <out_prefix> <t_ms> [<t_ms> …]
//
// „tag“ wählt das helle Farbschema, sonst wird das dunkle gezeichnet.
//
// blocks.bin: je Block uint8 n, uint8 flags, uint16 Faktor, uint32 Zeit (ms), n × uint32 Rohwert

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    fprintf(stderr, "Aufruf: display_preview [tag] splash <t_ms> <out.ppm> | monitor <bin> <prefix> <t…>\n");
    return 2;
}
