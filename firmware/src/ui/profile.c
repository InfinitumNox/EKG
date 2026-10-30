#include "profile.h"

#include <stdio.h>
#include <string.h>

bool profile_used(const profile_t *p) {
    return p->name[0] != '\0';
}

void profile_clear(profile_t *p) {
    memset(p, 0, sizeof *p);
}

void profile_sanitize(profile_t *p) {
    p->name[PROFILE_NAME_BYTES - 1] = '\0';
    if (p->age > PROFILE_AGE_MAX) {
        p->age = 0;
    }
    if (p->sex > SEX_FEMALE) {
        p->sex = SEX_UNKNOWN;
    }
    if (p->height_cm < PROFILE_HEIGHT_MIN || p->height_cm > PROFILE_HEIGHT_MAX) {
        p->height_cm = 0;
    }
    if (p->weight_kg < PROFILE_WEIGHT_MIN || p->weight_kg > PROFILE_WEIGHT_MAX) {
        p->weight_kg = 0;
    }
}

int profile_bmi10(const profile_t *p) {
    if (p->height_cm == 0 || p->weight_kg == 0) {
        return -1;
    }
    const long h2 = (long)p->height_cm * p->height_cm;
    return (int)(((long)p->weight_kg * 100000L + h2 / 2) / h2);
}

void profile_summary(const profile_t *p, char *buf, size_t n) {
    size_t used = 0;
    buf[0] = '\0';
    char part[24];
    for (int i = 0; i < 4; i++) {
        part[0] = '\0';
        switch (i) {
            case 0:
                if (p->age) {
                    snprintf(part, sizeof part, "%u J.", p->age);
                }
                break;
            case 1:
                if (p->sex) {
                    snprintf(part, sizeof part, "%s", p->sex == SEX_MALE ? "m" : "w");
                }
                break;
            case 2:
                if (p->height_cm) {
                    snprintf(part, sizeof part, "%u cm", p->height_cm);
                }
                break;
            default:
                if (p->weight_kg) {
                    snprintf(part, sizeof part, "%u kg", p->weight_kg);
                }
                break;
        }
        if (!part[0]) {
            continue;
        }
        int w = snprintf(buf + used, n - used, "%s%s", used ? " · " : "", part);
        if (w < 0 || (size_t)w >= n - used) {
            return;
        }
        used += (size_t)w;
    }
    if (!used) {
        snprintf(buf, n, "keine Angaben");
    }
}
