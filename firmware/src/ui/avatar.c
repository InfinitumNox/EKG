// Figur wie eine Gliederpuppe: Jedes Körperteil ist ein Abstandsfeld (Kapsel mit wechselndem
// Radius, Kreis, Rumpfprofil); „smin“ verschmilzt sie ohne Knick, die Kantenglättung folgt aus
// dem Abstand zum Rand. Die Rundung entsteht aus der Randrichtung mit Licht von links oben.

#include "avatar.h"

#include <math.h>
#include <stdbool.h>

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

// ------------------------------------------------------------------ Abstandsfelder
typedef struct {
    float x0, y0, x1, y1, r0, r1;
} cap_t;

// Abstand zum Rand einer Kapsel, deren Radius von r0 nach r1 wechselt (innen negativ)
static float sd_cap(const cap_t *c, float px, float py) {
    const float dx = c->x1 - c->x0, dy = c->y1 - c->y0;
    const float t = clampf(((px - c->x0) * dx + (py - c->y0) * dy) / (dx * dx + dy * dy + 1e-6f),
                           0.0f, 1.0f);
    const float qx = c->x0 + t * dx - px, qy = c->y0 + t * dy - py;
    return sqrtf(qx * qx + qy * qy) - (c->r0 + t * (c->r1 - c->r0));
}

static float sd_circle(float cx, float cy, float r, float px, float py) {
    const float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy) - r;
}

// weiches Minimum: verbindet zwei Teile mit einer Kehle der Größe k
static float smin(float a, float b, float k) {
    const float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1.0f - h);
}

// ------------------------------------------------------------------ Rumpf
typedef struct {
    float sh, ch, wa, hi;  // halbe Breite an Schulter, Brust, Taille, Hüfte (mal Körperhöhe)
    float waist_y;
} torso_t;

static torso_t torso_of(const avatar_t *a) {
    const float k = build(a);
    torso_t t;
    switch (a->sex) {
        case SEX_MALE:
            t = (torso_t){0.128f, 0.110f, 0.086f, 0.088f, 0.400f};
            break;
        case SEX_FEMALE:
            t = (torso_t){0.102f, 0.088f, 0.066f, 0.100f, 0.385f};
            break;
        default:
            t = (torso_t){0.115f, 0.099f, 0.076f, 0.094f, 0.395f};
            break;
    }
    t.sh *= part(k, 0.32f);
    t.ch *= part(k, 0.85f);
    t.wa *= part(k, 1.50f);
    t.hi *= part(k, 1.00f);
    return t;
}

static float smooth01(float u) {
    return u * u * (3.0f - 2.0f * u);
}

#define TORSO_TOP 0.150f
#define TORSO_BOTTOM 0.545f

// halbe Rumpfbreite (mal Körperhöhe) in Höhe v: geglättet zwischen den Schlüsselstellen, die
// alle Hoch- oder Tiefpunkte der Umrisslinie sind
static float torso_hw(const torso_t *t, float v) {
    const float ys[6] = {TORSO_TOP, 0.192f, 0.275f, t->waist_y, 0.510f, TORSO_BOTTOM};
    const float ws[6] = {0.034f, t->sh, t->ch, t->wa, t->hi, t->hi * 0.94f};
    v = clampf(v, ys[0], ys[5]);
    for (int i = 1; i < 6; i++) {
        if (v <= ys[i]) {
            const float u = (v - ys[i - 1]) / (ys[i] - ys[i - 1]);
            return clampf(ws[i - 1] + (ws[i] - ws[i - 1]) * smooth01(u), 0.0f, 0.22f);
        }
    }
    return ws[5];
}

// ------------------------------------------------------------------ Figur
typedef struct {
    float fh, cx, top;
    bool female;
    torso_t t;
    cap_t arm[2][4];  // Oberarm, Unterarm, Hand, Daumen
    cap_t leg[2][4];  // Oberschenkel, Wade oben, Wade unten, Fuß
    cap_t neck;
    float hx, hy, hr;  // Schädel
    float jx, jy, jr;  // Kiefer
    float bx[2], by, br;  // Brust (nur Frauen)
} figure_t;

static float torso_sd(const figure_t *f, float px, float py) {
    const float v = (py - f->top) / f->fh;
    const float hw = torso_hw(&f->t, v) * f->fh;
    const float e = 0.004f;
    const float slope = (torso_hw(&f->t, v + e) - torso_hw(&f->t, v - e)) / (2.0f * e);
    const float dx = (fabsf(px - f->cx) - hw) / sqrtf(1.0f + slope * slope);
    const float dt = f->top + TORSO_TOP * f->fh - py;
    const float db = py - (f->top + TORSO_BOTTOM * f->fh);
    return fmaxf(fmaxf(dx, dt), db);
}

// Rumpf mit Brust
static float chest_sd(const figure_t *f, float px, float py) {
    float d = torso_sd(f, px, py);
    if (f->female) {
        for (int s = 0; s < 2; s++) {
            d = smin(d, sd_circle(f->bx[s], f->by, f->br, px, py), 0.016f * f->fh);
        }
    }
    return d;
}

static float body_sd(const figure_t *f, float px, float py) {
    const float fh = f->fh;
    float d = chest_sd(f, px, py);
    d = smin(d, sd_cap(&f->neck, px, py), 0.024f * fh);
    const float head = smin(sd_circle(f->hx, f->hy, f->hr, px, py),
                            sd_circle(f->jx, f->jy, f->jr, px, py), 0.028f * fh);
    d = smin(d, head, 0.012f * fh);
    for (int s = 0; s < 2; s++) {
        float da = smin(sd_cap(&f->arm[s][0], px, py), sd_cap(&f->arm[s][1], px, py), 0.014f * fh);
        da = smin(da, sd_cap(&f->arm[s][2], px, py), 0.008f * fh);
        da = smin(da, sd_cap(&f->arm[s][3], px, py), 0.004f * fh);
        d = smin(d, da, 0.014f * fh);
        float dl = smin(sd_cap(&f->leg[s][0], px, py), sd_cap(&f->leg[s][1], px, py), 0.022f * fh);
        dl = smin(dl, sd_cap(&f->leg[s][2], px, py), 0.016f * fh);
        dl = smin(dl, sd_cap(&f->leg[s][3], px, py), 0.008f * fh);
        d = smin(d, dl, 0.022f * fh);
    }
    return d;
}

static void build_figure(figure_t *f, const avatar_t *av, float k, float fh, float cx,
                         float top) {
    f->fh = fh;
    f->cx = cx;
    f->top = top;
    f->female = av->sex == SEX_FEMALE;
    f->t = torso_of(av);
    const torso_t *t = &f->t;
    const float headk = av->height_cm < 150 ? 1.0f + (150.0f - (float)av->height_cm) / 150.0f
                                            : 1.0f;
    const float rh = 0.056f * headk;  // Schädelradius (mal Körperhöhe)
#define PX(u) (cx + (u) * fh)
#define PY(v) (top + (v) * fh)
    f->hx = cx;
    f->hy = PY(rh * 1.05f);
    f->hr = rh * 0.92f * fh;
    f->jx = cx;
    f->jy = PY(rh * 1.55f);
    f->jr = rh * 0.70f * fh;
    f->neck = (cap_t){cx, PY(rh * 1.75f), cx, PY(0.176f), 0.026f * part(k, 0.5f) * fh,
                      0.033f * part(k, 0.5f) * fh};
    f->by = PY(0.292f);
    f->br = 0.034f * part(k, 0.9f) * fh;
    f->bx[0] = PX(-0.040f * part(k, 0.6f));
    f->bx[1] = PX(0.040f * part(k, 0.6f));

    const float body_hw = fmaxf(t->ch, t->wa), hand_hw = fmaxf(body_hw, t->hi);
    for (int s = 0; s < 2; s++) {
        const float sg = s == 0 ? -1.0f : 1.0f;  // −: rechte Körperseite (links im Bild)
        const float ua = 0.028f * part(k, 0.95f), fa = 0.021f * part(k, 0.90f);
        const float sx = sg * (t->sh - 0.022f), ex = sg * (body_hw + 0.056f);
        const float wx = sg * (hand_hw + 0.074f);
        f->arm[s][0] = (cap_t){PX(sx), PY(0.208f), PX(ex), PY(0.375f), ua * fh, fa * 1.08f * fh};
        f->arm[s][1] = (cap_t){PX(ex), PY(0.375f), PX(wx), PY(0.512f), fa * 1.08f * fh,
                               0.0135f * part(k, 0.6f) * fh};
        f->arm[s][2] = (cap_t){PX(wx), PY(0.520f), PX(wx + sg * 0.009f), PY(0.568f),
                               0.0165f * fh, 0.0125f * fh};
        f->arm[s][3] = (cap_t){PX(wx - sg * 0.012f), PY(0.530f), PX(wx - sg * 0.014f),
                               PY(0.552f), 0.0085f * fh, 0.0065f * fh};
        // Beine: Hüftgelenk, Knie, Wadenbauch, Knöchel (leicht nach innen); Fuß nach außen
        const float hxp = sg * t->hi * 0.50f, kxp = sg * t->hi * 0.46f, mxp = sg * t->hi * 0.44f,
                    axp = sg * t->hi * 0.42f;
        const float thigh = fmaxf(t->hi * 0.52f, 0.04f), knee = 0.033f * part(k, 0.7f);
        f->leg[s][0] = (cap_t){PX(hxp), PY(0.540f), PX(kxp), PY(0.742f), thigh * fh,
                               knee * 1.05f * fh};
        f->leg[s][1] = (cap_t){PX(kxp), PY(0.742f), PX(mxp), PY(0.800f), knee * 1.05f * fh,
                               knee * 1.12f * fh};
        f->leg[s][2] = (cap_t){PX(mxp), PY(0.800f), PX(axp), PY(0.942f), knee * 1.12f * fh,
                               0.0185f * part(k, 0.5f) * fh};
        f->leg[s][3] = (cap_t){PX(axp), PY(0.966f), PX(axp + sg * 0.040f), PY(0.980f),
                               0.0195f * fh, 0.0145f * fh};
    }
#undef PX
#undef PY
}

// ------------------------------------------------------------------ Farben und Licht
// Hautton, Kleidung und Haar sind für beide Anzeigen gleich; die Figur soll wie ein Mensch aussehen.
#define SKIN 0xD9A98Au
#define CLOTH 0x34476Eu
#define HAIR_DARK 0x3A2A20u
#define HAIR_LONG 0x59392Au

static uint32_t lit(uint32_t base, float i) {
    // i 0 … 1: dunkel bis hell; 0,6 ist die Grundfarbe
    if (i < 0.6f) {
        return gfx_mix(0x000000u, base, (int)(i / 0.6f * 256.0f));
    }
    return gfx_mix(base, 0xFFFFFFu, (int)((i - 0.6f) / 0.4f * 70.0f));
}

// Licht von links oben: Randrichtung (gx, gy aus dem Abstandsfeld) und Tiefe t (0 am Rand, 1 innen)
static float light(float gx, float gy, float t) {
    const float nz = sqrtf(clampf(1.0f - (1.0f - t) * (1.0f - t), 0.0f, 1.0f));
    const float s = sqrtf(clampf(1.0f - nz * nz, 0.0f, 1.0f));
    const float len = sqrtf(gx * gx + gy * gy) + 1e-6f;
    const float nx = gx / len * s, ny = gy / len * s;
    const float dot = nx * -0.45f + ny * -0.55f + nz * 0.70f;
    return clampf(0.22f + 0.78f * dot, 0.0f, 1.0f) * 0.92f + 0.08f;
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
    figure_t f;
    build_figure(&f, av, k, fh, cx, top);
    const float depth = fmaxf(2.5f, 0.060f * fh);
    const bool female = av->sex == SEX_FEMALE;

    // Haare hinter dem Kopf: Frauen lang, sonst kurz
    {
        const uint32_t hair = female ? HAIR_LONG : HAIR_DARK;
        const cap_t mane = {cx, f.hy, cx, top + 0.205f * fh, f.hr * 1.16f, f.hr * 0.98f};
        const int y1 = (int)ceilf(top + 0.235f * fh);
        const int x0 = (int)floorf(cx - 0.12f * fh), x1 = (int)ceilf(cx + 0.12f * fh);
        for (int y = (int)floorf(top) - 2; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                const float d = female ? sd_cap(&mane, px, py)
                                       : sd_circle(cx, f.hy - 0.22f * f.hr, f.hr * 1.06f, px, py);
                const float cov = clampf(0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    const float tt = clampf(-d / (0.5f * f.hr), 0.0f, 1.0f);
                    gfx_blend(c, x, y, lit(hair, 0.45f + 0.30f * tt), (int)(cov * 256.0f));
                }
            }
        }
    }

    // Körper Pixel für Pixel
    const float half = reach * fh + 3.0f;
    const int x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
    const int y0 = (int)floorf(top) - 1, y1 = by + bh;
    const float waistband = top + (female ? 0.468f : 0.458f) * fh, hem = top + 0.628f * fh;
    const float bra_top = top + 0.262f * fh, bra_bottom = top + 0.322f * fh;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            const float d = body_sd(&f, px, py);
            const float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) {
                continue;
            }
            const float gx = body_sd(&f, px + 1.0f, py) - d, gy = body_sd(&f, px, py + 1.0f) - d;
            const float i = light(gx, gy, clampf(-d / depth, 0.0f, 1.0f));
            uint32_t base = SKIN;
            // Shorts (Rumpf und Beine) und bei Frauen ein Top über der Brust
            float cloth = 0.0f;
            if (py > waistband - 1.0f && py < hem + 1.0f) {
                float dc = torso_sd(&f, px, py);
                for (int s = 0; s < 2; s++) {
                    dc = fminf(dc, sd_cap(&f.leg[s][0], px, py));
                }
                cloth = clampf(0.5f - dc, 0.0f, 1.0f) * clampf(py - waistband + 0.5f, 0.0f, 1.0f) *
                        clampf(hem - py + 0.5f, 0.0f, 1.0f);
            } else if (female && py > bra_top - 1.0f && py < bra_bottom + 1.0f) {
                cloth = clampf(0.5f - chest_sd(&f, px, py), 0.0f, 1.0f) *
                        clampf(py - bra_top + 0.5f, 0.0f, 1.0f) *
                        clampf(bra_bottom - py + 0.5f, 0.0f, 1.0f);
            }
            if (cloth > 0.0f) {
                base = gfx_mix(SKIN, CLOTH, (int)(cloth * 256.0f));
            }
            gfx_blend(c, x, y, lit(base, i), (int)(cov * 256.0f));
        }
    }

    if (sites) {
        // Anker: RA rechts unter dem Schlüsselbein, LA links und RL rechts am unteren Rippenbogen
        // (Ableitung II). „Rechts“ der Person liegt im Bild links.
        const float ya = 0.232f, yr = 0.360f;
        const float xa = fminf(0.062f, 0.70f * torso_hw(&t, ya));
        const float xr = fminf(0.056f, 0.70f * torso_hw(&t, yr));
        sites->ra = (avatar_pt_t){cx - xa * fh, top + ya * fh};
        sites->la = (avatar_pt_t){cx + xr * fh, top + yr * fh};
        sites->rl = (avatar_pt_t){cx - xr * fh, top + yr * fh};
        sites->r = clampf(0.032f * fh, 3.6f, 7.0f);
    }
}
