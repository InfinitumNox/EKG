#include "avatar.h"

#include <math.h>

#include "theme.h"

#define NORMAL_BMI10 220

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

avatar_t avatar_for(const profile_t *p, int mode) {
    avatar_t a = {SEX_UNKNOWN, 172, NORMAL_BMI10};
    if (!p) {
        return a;
    }
    a.sex = p->sex;
    a.height_cm = p->sex == SEX_MALE ? 178 : p->sex == SEX_FEMALE ? 165 : 172;
    if (mode == AVATAR_PROFILE) {
        if (p->height_cm) {
            a.height_cm = p->height_cm;
        }
        int bmi = profile_bmi10(p);
        if (bmi >= 0) {
            a.bmi10 = bmi;
        }
    }
    return a;
}

// Breitenfaktor aus dem BMI: 22 = 1,0; Bauch und Hüfte legen stärker zu als die Schultern
static float build(const avatar_t *a) {
    return clampf(1.0f + 0.045f * ((float)a->bmi10 / 10.0f - 22.0f), 0.78f, 1.55f);
}

static float part(float k, float f) {
    return 1.0f + (k - 1.0f) * f;
}

typedef struct {
    float sh, ch, wa, hi;  // halbe Breite an Schulter, Brust, Taille, Hüfte (mal Körperhöhe)
    float waist_y;
} torso_t;

static torso_t torso_of(const avatar_t *a) {
    const float k = build(a);
    torso_t t;
    switch (a->sex) {
        case SEX_MALE:
            t = (torso_t){0.125f, 0.108f, 0.088f, 0.090f, 0.400f};
            break;
        case SEX_FEMALE:
            t = (torso_t){0.105f, 0.092f, 0.070f, 0.102f, 0.385f};
            break;
        default:
            t = (torso_t){0.115f, 0.100f, 0.080f, 0.094f, 0.400f};
            break;
    }
    t.sh *= part(k, 0.55f);
    t.ch *= part(k, 0.80f);
    t.wa *= part(k, 1.50f);
    t.hi *= part(k, 1.00f);
    return t;
}

// halbe Rumpfbreite (mal Körperhöhe) in Höhe v (0 = Scheitel, 1 = Sohle)
static float torso_hw(const torso_t *t, float v) {
    const float ys[6] = {0.150f, 0.185f, 0.270f, t->waist_y, 0.510f, 0.540f};
    const float ws[6] = {0.040f, t->sh, t->ch, t->wa, t->hi, t->hi * 0.97f};
    if (v <= ys[0] || v >= ys[5]) {
        return 0.0f;
    }
    for (int i = 1; i < 6; i++) {
        if (v <= ys[i]) {
            float u = (v - ys[i - 1]) / (ys[i] - ys[i - 1]);
            return clampf(ws[i - 1] + u * (ws[i] - ws[i - 1]), 0.0f, 0.22f);
        }
    }
    return 0.0f;
}

void avatar_draw(canvas_t *c, int bx, int by, int bw, int bh, const avatar_t *av,
                 avatar_sites_t *sites) {
    const float k = build(av);
    const torso_t t = torso_of(av);
    float fh = (float)bh * clampf((float)av->height_cm / 190.0f, 0.50f, 1.0f);
    // Arme und Hände dürfen die Box nicht verlassen: breite Figuren werden etwas kleiner
    const float reach = fmaxf(fmaxf(t.ch, t.wa), t.hi) + 0.110f;
    if (reach * fh > (float)bw / 2.0f - 2.0f) {
        fh = ((float)bw / 2.0f - 2.0f) / reach;
    }
    const float cx = (float)bx + (float)bw / 2.0f;
    const float top = (float)(by + bh) - fh;
    const uint32_t fill = gfx_mix(C_PANEL_EDGE, C_TEXT_FAINT, 128);
    const uint32_t hair = gfx_mix(fill, 0x000000u, 100);
#define X(u) (cx + (u) * fh)
#define Y(v) (top + (v) * fh)

    // Kinder haben einen größeren Kopf im Verhältnis zum Körper
    const float headk = av->height_cm < 150 ? 1.0f + (150.0f - (float)av->height_cm) / 150.0f : 1.0f;
    const float rh = 0.062f * headk;  // Kopfradius

    // Haare hinter dem Körper
    if (av->sex == SEX_FEMALE) {
        gfx_line(c, cx, Y(rh), cx, Y(0.205f), rh * 2.3f * fh, hair, OPAQUE);
    } else if (av->sex == SEX_MALE) {
        gfx_disc(c, cx, Y(rh * 0.95f), rh * 1.05f * fh, hair, OPAQUE);
    }

    // Beine und Füße
    const float legw = t.hi * 0.98f, shin = 0.050f * part(k, 0.70f);
    for (int s = -1; s <= 1; s += 2) {
        const float f = (float)s;
        const float hx = f * t.hi * 0.50f, kx = f * (t.hi * 0.50f - 0.004f), ax = f * t.hi * 0.45f;
        gfx_line(c, X(hx), Y(0.515f), X(kx * 1.0f), Y(0.640f), legw * 0.97f * fh, fill, OPAQUE);
        gfx_line(c, X(kx), Y(0.640f), X(kx), Y(0.745f), (legw * 0.78f) * fh, fill, OPAQUE);
        gfx_line(c, X(kx), Y(0.745f), X(ax), Y(0.955f), shin * fh, fill, OPAQUE);
        gfx_line(c, X(ax), Y(0.972f), X(ax + f * 0.050f), Y(0.972f), 0.034f * fh, fill, OPAQUE);
    }

    // Arme: leicht vom Körper weg, damit die Brust frei bleibt
    const float body_hw = fmaxf(t.ch, t.wa), hand_hw = fmaxf(body_hw, t.hi);
    for (int s = -1; s <= 1; s += 2) {
        const float f = (float)s;
        gfx_line(c, X(f * (t.sh - 0.012f)), Y(0.200f), X(f * (body_hw + 0.058f)), Y(0.370f),
                 0.052f * part(k, 0.8f) * fh, fill, OPAQUE);
        gfx_line(c, X(f * (body_hw + 0.058f)), Y(0.370f), X(f * (hand_hw + 0.080f)), Y(0.520f),
                 0.044f * part(k, 0.7f) * fh, fill, OPAQUE);
        gfx_disc(c, X(f * (hand_hw + 0.080f)), Y(0.535f), 0.026f * fh, fill, OPAQUE);
        gfx_disc(c, X(f * (t.sh - 0.014f)), Y(0.198f), 0.030f * fh * part(k, 0.5f), fill, OPAQUE);
    }

    // Rumpf zeilenweise: kantengeglättet an den Seiten
    const int j0 = (int)floorf(Y(0.150f)), j1 = (int)ceilf(Y(0.540f));
    for (int j = j0; j <= j1; j++) {
        const float v = ((float)j + 0.5f - top) / fh;
        const float hw = torso_hw(&t, v) * fh;
        if (hw <= 0.0f) {
            continue;
        }
        const float xl = cx - hw, xr = cx + hw;
        const int il = (int)ceilf(xl), ir = (int)floorf(xr);
        if (ir > il) {
            gfx_fill(c, il, j, ir - il, 1, fill);
        }
        gfx_blend(c, il - 1, j, fill, (int)(((float)il - xl) * 256.0f));
        gfx_blend(c, ir, j, fill, (int)((xr - (float)ir) * 256.0f));
    }

    // Hals und Kopf
    gfx_line(c, cx, Y(rh * 2.0f - 0.02f), cx, Y(0.170f), 0.046f * part(k, 0.5f) * fh, fill,
             OPAQUE);
    if (av->sex == SEX_MALE) {
        gfx_disc(c, cx, Y(rh * 1.10f), rh * 0.93f * fh, fill, OPAQUE);
    } else {
        gfx_disc(c, cx, Y(rh), rh * fh, fill, OPAQUE);
    }

    if (sites) {
        // Anker: RA rechts unter dem Schlüsselbein, LA links und RL rechts am unteren Rippenbogen
        // (Ableitung II). „Rechts“ der Person liegt im Bild links.
        const float ya = 0.240f, yr = 0.360f;
        const float xa = fminf(0.062f, 0.70f * torso_hw(&t, ya));
        const float xr2 = fminf(0.056f, 0.70f * torso_hw(&t, yr));
        sites->ra = (avatar_pt_t){X(-xa), Y(ya)};
        sites->la = (avatar_pt_t){X(xr2), Y(yr)};
        sites->rl = (avatar_pt_t){X(-xr2), Y(yr)};
        sites->r = clampf(0.032f * fh, 3.6f, 7.0f);
    }
#undef X
#undef Y
}
