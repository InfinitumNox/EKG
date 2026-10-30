// Flache Figur mit fließenden Umrissen. Rumpf, Beine und Arme sind Profile: Mittellinie und halbe
// Breite über der Höhe, zwischen Stützstellen kubisch interpoliert (stetige Steigung, keine
// Knicke). Kopf, Hals, Schultern und Füße sind Kreise und Kapseln; „smin“ verschmilzt alle Teile
// mit weichen Kehlen. Gerechnet wird in Körperhöhen (0 = Scheitel, 1 = Sohle), die Kanten-
// glättung folgt aus dem Abstand zum Rand.

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

// Breitenfaktor aus dem BMI: 22 = 1,0
static float build(const avatar_t *a) {
    return clampf(1.0f + 0.045f * ((float)a->bmi10 / 10.0f - 22.0f), 0.78f, 1.55f);
}

// wie stark eine Stelle mit dem BMI zulegt (f = 0: gar nicht, 1: voll, Taille 1,5)
static float part(float k, float f) {
    return 1.0f + (k - 1.0f) * f;
}

// ------------------------------------------------------------------ Grundformen
typedef struct {
    float x0, y0, x1, y1, r0, r1;
} cap_t;

// Abstand zum Rand einer Kapsel, deren Radius von r0 nach r1 wechselt (innen negativ)
static float sd_cap(const cap_t *c, float px, float py) {
    const float dx = c->x1 - c->x0, dy = c->y1 - c->y0;
    const float t = clampf(((px - c->x0) * dx + (py - c->y0) * dy) / (dx * dx + dy * dy + 1e-9f),
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

// ------------------------------------------------------------------ Profile
#define MAX_KEYS 10

typedef struct {
    int n;
    float y[MAX_KEYS], xc[MAX_KEYS], hw[MAX_KEYS];
} prof_t;

// Steigung an Stützstelle i (Mittel der angrenzenden Sekanten)
static float tangent(const float *ys, const float *vs, int n, int i) {
    if (i == 0) {
        return (vs[1] - vs[0]) / (ys[1] - ys[0]);
    }
    if (i == n - 1) {
        return (vs[n - 1] - vs[n - 2]) / (ys[n - 1] - ys[n - 2]);
    }
    return 0.5f * ((vs[i] - vs[i - 1]) / (ys[i] - ys[i - 1]) +
                   (vs[i + 1] - vs[i]) / (ys[i + 1] - ys[i]));
}

// kubische Hermite-Interpolation; dv bekommt die Ableitung nach y
static float hermite(const float *ys, const float *vs, int n, float y, float *dv) {
    if (y <= ys[0]) {
        *dv = 0.0f;
        return vs[0];
    }
    if (y >= ys[n - 1]) {
        *dv = 0.0f;
        return vs[n - 1];
    }
    int i = 0;
    while (y > ys[i + 1]) {
        i++;
    }
    const float h = ys[i + 1] - ys[i], t = (y - ys[i]) / h;
    const float m0 = tangent(ys, vs, n, i) * h, m1 = tangent(ys, vs, n, i + 1) * h;
    const float t2 = t * t, t3 = t2 * t;
    const float v = (2 * t3 - 3 * t2 + 1) * vs[i] + (t3 - 2 * t2 + t) * m0 +
                    (-2 * t3 + 3 * t2) * vs[i + 1] + (t3 - t2) * m1;
    *dv = ((6 * t2 - 6 * t) * vs[i] + (3 * t2 - 4 * t + 1) * m0 + (-6 * t2 + 6 * t) * vs[i + 1] +
           (3 * t2 - 2 * t) * m1) /
          h;
    return v;
}

// Abstand zum Rand eines Profils: waagrechter Abstand, mit der Randsteigung auf den echten
// Abstand umgerechnet; oben und unten gerade abgeschnitten
static float sd_prof(const prof_t *p, float px, float py) {
    float dxc, dhw;
    const float xc = hermite(p->y, p->xc, p->n, py, &dxc);
    const float hw = hermite(p->y, p->hw, p->n, py, &dhw);
    const float slope = dxc + (px >= xc ? dhw : -dhw);
    const float d = (fabsf(px - xc) - hw) / sqrtf(1.0f + slope * slope);
    return fmaxf(d, fmaxf(p->y[0] - py, py - p->y[p->n - 1]));
}

static void prof_add(prof_t *p, float y, float xc, float hw) {
    if (p->n < MAX_KEYS) {
        p->y[p->n] = y;
        p->xc[p->n] = xc;
        p->hw[p->n] = hw;
        p->n++;
    }
}

// ------------------------------------------------------------------ Körperbau
// Stützstellen für Frauen und Männer (y, halbe Breite bzw. Mitte, Zunahme mit dem BMI);
// neutral ist der Mittelwert. Frauen: Sanduhr mit schmaler Taille; Männer: V-Form.
typedef struct {
    float y, xc, hw, f;
} key_t2;

#define N_TORSO 10
static const key_t2 TORSO_F[N_TORSO] = {
    {0.150f, 0, 0.030f, 0.4f},  // Halsansatz
    {0.172f, 0, 0.058f, 0.4f},  // Nacken
    {0.198f, 0, 0.098f, 0.3f},  // Schulter
    {0.240f, 0, 0.086f, 0.8f},  // Achsel
    {0.280f, 0, 0.091f, 0.9f},  // Brust
    {0.314f, 0, 0.075f, 1.1f},  // unter der Brust
    {0.365f, 0, 0.063f, 1.5f},  // Taille
    {0.422f, 0, 0.088f, 1.3f},  // obere Hüfte
    {0.470f, 0, 0.106f, 1.0f},  // Hüfte
    {0.515f, 0, 0.098f, 1.0f},  // Schritt
};
static const key_t2 TORSO_M[N_TORSO] = {
    {0.150f, 0, 0.036f, 0.4f}, {0.170f, 0, 0.076f, 0.4f}, {0.198f, 0, 0.130f, 0.3f},
    {0.245f, 0, 0.115f, 0.8f}, {0.285f, 0, 0.108f, 0.9f}, {0.330f, 0, 0.094f, 1.3f},
    {0.385f, 0, 0.083f, 1.5f}, {0.440f, 0, 0.087f, 1.2f}, {0.482f, 0, 0.092f, 1.0f},
    {0.522f, 0, 0.089f, 1.0f},
};

#define N_LEG 10
static const key_t2 LEG_F[N_LEG] = {
    {0.440f, 0.050f, 0.052f, 1.1f},  // in der Hüfte
    {0.515f, 0.052f, 0.056f, 1.1f},  // Schritt
    {0.585f, 0.047f, 0.051f, 1.1f},  // Oberschenkel
    {0.665f, 0.040f, 0.041f, 1.0f},
    {0.735f, 0.034f, 0.029f, 0.7f},  // über dem Knie
    {0.762f, 0.033f, 0.026f, 0.6f},  // Knie
    {0.812f, 0.036f, 0.031f, 0.6f},  // Wade
    {0.880f, 0.034f, 0.022f, 0.5f},
    {0.942f, 0.031f, 0.014f, 0.4f},  // Knöchel
    {0.958f, 0.031f, 0.014f, 0.4f},
};
static const key_t2 LEG_M[N_LEG] = {
    {0.440f, 0.050f, 0.050f, 1.1f}, {0.522f, 0.050f, 0.052f, 1.1f},
    {0.600f, 0.047f, 0.048f, 1.1f}, {0.680f, 0.043f, 0.040f, 1.0f},
    {0.745f, 0.040f, 0.031f, 0.7f}, {0.772f, 0.040f, 0.029f, 0.6f},
    {0.822f, 0.043f, 0.035f, 0.6f}, {0.888f, 0.040f, 0.025f, 0.5f},
    {0.944f, 0.036f, 0.016f, 0.4f}, {0.958f, 0.036f, 0.016f, 0.4f},
};

static key_t2 blend_key(const key_t2 *f, const key_t2 *m, int sex) {
    if (sex == SEX_FEMALE) {
        return *f;
    }
    if (sex == SEX_MALE) {
        return *m;
    }
    return (key_t2){0.5f * (f->y + m->y), 0.5f * (f->xc + m->xc), 0.5f * (f->hw + m->hw),
                    0.5f * (f->f + m->f)};
}

typedef struct {
    prof_t torso, leg[2], arm[2], hair;
    bool long_hair, short_hair;
    cap_t neck, foot[2];
    float dx[2], dy, dr;  // Schultern (Deltamuskel)
    float hx, hy, hr;     // Schädel
    float jy, jr;         // Kiefer
    float reach;          // größte halbe Breite (Hände)
} figure_t;

static float torso_hw_at(const figure_t *f, float v) {
    float d;
    return hermite(f->torso.y, f->torso.hw, f->torso.n, v, &d);
}

static void build_figure(figure_t *f, const avatar_t *av) {
    const float k = build(av);
    const int sex = av->sex;
    const bool female = sex == SEX_FEMALE, male = sex == SEX_MALE;
    *f = (figure_t){0};

    for (int i = 0; i < N_TORSO; i++) {
        const key_t2 t = blend_key(&TORSO_F[i], &TORSO_M[i], sex);
        prof_add(&f->torso, t.y, 0.0f, t.hw * part(k, t.f));
    }
    for (int s = 0; s < 2; s++) {
        const float sg = s == 0 ? -1.0f : 1.0f;  // −: rechte Körperseite (links im Bild)
        for (int i = 0; i < N_LEG; i++) {
            const key_t2 l = blend_key(&LEG_F[i], &LEG_M[i], sex);
            const float spread = part(k, l.f >= 1.0f ? 0.75f : 0.40f);
            prof_add(&f->leg[s], l.y, sg * l.xc * spread, l.hw * part(k, l.f));
        }
        const float ay = f->leg[s].y[N_LEG - 1], ax = f->leg[s].xc[N_LEG - 1];
        f->foot[s] = (cap_t){ax, ay + 0.006f, ax + sg * 0.036f, 0.982f, 0.017f, 0.012f};
    }

    // Arme hängen locker neben dem Körper: Ellbogen auf Taillenhöhe, Hände neben der Hüfte
    const float sh = torso_hw_at(f, 0.198f), waist = torso_hw_at(f, female ? 0.365f : 0.385f);
    const float hip = torso_hw_at(f, female ? 0.470f : 0.482f);
    const float w_up = (male ? 0.031f : 0.027f) * part(k, 0.9f);
    const float w_el = (male ? 0.021f : 0.017f) * part(k, 0.7f);
    const float w_fa = (male ? 0.024f : 0.020f) * part(k, 0.7f);
    const float w_wr = (male ? 0.015f : 0.012f) * part(k, 0.5f);
    const float w_pa = (male ? 0.019f : 0.016f) * part(k, 0.4f);
    const float el = fmaxf(sh + 0.006f, waist + w_el + 0.020f);
    const float wr = fmaxf(el + 0.016f, hip + w_wr + 0.022f);
    for (int s = 0; s < 2; s++) {
        const float sg = s == 0 ? -1.0f : 1.0f;
        prof_t *a = &f->arm[s];
        prof_add(a, 0.215f, sg * (sh - 0.020f), w_up);
        prof_add(a, 0.262f, sg * (sh - 0.010f), w_up * 0.93f);
        prof_add(a, 0.305f, sg * (sh - 0.002f + 0.3f * (el - sh)), w_up * 0.84f);
        prof_add(a, 0.362f, sg * el, w_el);
        prof_add(a, 0.402f, sg * (el + 0.35f * (wr - el)), w_fa);
        prof_add(a, 0.468f, sg * wr, w_wr);
        prof_add(a, 0.500f, sg * (wr + 0.003f), w_pa);
        prof_add(a, 0.540f, sg * (wr + 0.004f), w_pa * 0.70f);
        prof_add(a, 0.562f, sg * (wr + 0.004f), w_pa * 0.35f);
        f->dx[s] = sg * (sh - 0.024f);
    }
    f->dy = 0.214f;
    f->dr = (male ? 0.033f : 0.027f) * part(k, 0.6f);
    f->reach = wr + w_pa + 0.012f;

    // Kopf: Schädel und Kiefer zu einem Oval verschmolzen; Kinder mit größerem Kopf
    const float headk = av->height_cm < 150 ? 1.0f + (150.0f - (float)av->height_cm) / 150.0f
                                            : 1.0f;
    const float rh = (female ? 0.053f : 0.056f) * headk;
    f->hx = 0.0f;
    f->hy = rh * 1.05f;
    f->hr = rh * 0.92f;
    f->jy = rh * 1.55f;
    f->jr = rh * (male ? 0.74f : 0.66f);
    f->neck = (cap_t){0.0f, rh * 1.75f, 0.0f, 0.176f, (male ? 0.027f : 0.021f) * part(k, 0.5f),
                      (male ? 0.036f : 0.028f) * part(k, 0.5f)};

    // Haare: Frauen lang über die Schultern, Männer kurz
    f->long_hair = female;
    f->short_hair = male;
    if (female) {
        prof_t *h = &f->hair;
        prof_add(h, f->hy, 0.0f, rh * 1.04f);
        prof_add(h, 0.120f, 0.0f, rh * 1.12f);
        prof_add(h, 0.170f, 0.0f, rh * 1.16f);
        prof_add(h, 0.215f, 0.0f, rh * 1.10f);
        prof_add(h, 0.245f, 0.0f, rh * 0.85f);
    }
}

static float body_sd(const figure_t *f, float u, float v) {
    float d = sd_prof(&f->torso, u, v);
    d = smin(d, sd_cap(&f->neck, u, v), 0.020f);
    const float head =
        smin(sd_circle(f->hx, f->hy, f->hr, u, v), sd_circle(f->hx, f->jy, f->jr, u, v), 0.028f);
    d = smin(d, head, 0.010f);
    for (int s = 0; s < 2; s++) {
        const float dl = smin(sd_prof(&f->leg[s], u, v), sd_cap(&f->foot[s], u, v), 0.010f);
        d = smin(d, dl, 0.016f);
        const float da = smin(sd_circle(f->dx[s], f->dy, f->dr, u, v), sd_prof(&f->arm[s], u, v),
                              0.014f);
        d = smin(d, da, 0.005f);
    }
    return d;
}

static float hair_sd(const figure_t *f, float u, float v) {
    if (f->long_hair) {
        return smin(sd_circle(f->hx, f->hy - 0.06f * f->hr, f->hr * 1.10f, u, v),
                    sd_prof(&f->hair, u, v), 0.012f);
    }
    return sd_circle(f->hx, f->hy - 0.22f * f->hr, f->hr * 1.06f, u, v);
}

// ------------------------------------------------------------------ Zeichnen
// Flache 2D-Figur: eine Farbe im Ton der Anzeige, Haare etwas dunkler, geglättete Ränder
static void cover(canvas_t *c, int x, int y, float d_px, uint32_t rgb) {
    const float cov = clampf(0.5f - d_px, 0.0f, 1.0f);
    if (cov > 0.0f) {
        gfx_blend(c, x, y, rgb, (int)(cov * 256.0f));
    }
}

void avatar_draw(canvas_t *c, int bx, int by, int bw, int bh, const avatar_t *av,
                 avatar_sites_t *sites) {
    figure_t f;
    build_figure(&f, av);
    float fh = (float)bh * clampf((float)av->height_cm / 190.0f, 0.50f, 1.0f);
    // Hände dürfen die Box nicht verlassen: breite Figuren werden etwas kleiner
    if (f.reach * fh > (float)bw / 2.0f - 2.0f) {
        fh = ((float)bw / 2.0f - 2.0f) / f.reach;
    }
    const float cx = (float)bx + (float)bw / 2.0f;
    const float top = (float)(by + bh) - fh;
    const uint32_t fill = gfx_mix(C_PANEL_EDGE, C_TEXT_FAINT, 128);
    const uint32_t hair = gfx_mix(fill, 0x000000u, 100);
    const float inv = 1.0f / fh;

    if (f.long_hair || f.short_hair) {
        const int x0 = (int)floorf(cx - 0.10f * fh), x1 = (int)ceilf(cx + 0.10f * fh);
        const int y1 = (int)ceilf(top + 0.25f * fh);
        for (int y = (int)floorf(top) - 2; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const float u = ((float)x + 0.5f - cx) * inv, v = ((float)y + 0.5f - top) * inv;
                cover(c, x, y, hair_sd(&f, u, v) * fh, hair);
            }
        }
    }

    const float half = f.reach * fh + 3.0f;
    const int x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
    for (int y = (int)floorf(top) - 1; y <= by + bh; y++) {
        const float v = ((float)y + 0.5f - top) * inv;
        for (int x = x0; x <= x1; x++) {
            const float u = ((float)x + 0.5f - cx) * inv;
            cover(c, x, y, body_sd(&f, u, v) * fh, fill);
        }
    }

    if (sites) {
        // Anker: RA rechts unter dem Schlüsselbein, LA links und RL rechts am unteren Rippenbogen
        // (Ableitung II). „Rechts“ der Person liegt im Bild links.
        const float ya = 0.232f, yr = av->sex == SEX_FEMALE ? 0.330f : 0.345f;
        const float xa = fminf(0.060f, 0.65f * torso_hw_at(&f, ya));
        const float xr = fminf(0.050f, 0.68f * torso_hw_at(&f, yr));
        sites->ra = (avatar_pt_t){cx - xa * fh, top + ya * fh};
        sites->la = (avatar_pt_t){cx + xr * fh, top + yr * fh};
        sites->rl = (avatar_pt_t){cx - xr * fh, top + yr * fh};
        sites->r = clampf(0.032f * fh, 3.6f, 7.0f);
    }
}
