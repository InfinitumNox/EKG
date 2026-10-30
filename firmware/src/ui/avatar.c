// Flache Figur mit fließenden Umrissen. Rumpf, Beine, Arme und Haarsträhnen sind Profile
// (Mittellinie und halbe Breite über der Höhe), zwischen Stützstellen monoton kubisch
// interpoliert: stetige Steigung, kein Überschwingen an Hoch- und Tiefpunkten. Glieder enden rund
// und beginnen innerhalb des Rumpfs. Kopf, Hals und Füße sind Kreise und Kapseln; „smin“
// verschmilzt alle Teile mit weichen Kehlen. Die Haare liegen etwas dunkler darüber und lassen
// das Gesicht frei. Gerechnet wird in Körperhöhen (0 = Scheitel, 1 = Sohle); die
// Kantenglättung folgt aus dem Abstand zum Rand.

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
#define MAX_KEYS 11

typedef struct {
    int n;
    bool round_top, round_bottom;  // Enden als Halbkreis statt gerade
    float y[MAX_KEYS], xc[MAX_KEYS], hw[MAX_KEYS];
    float mx[MAX_KEYS], mw[MAX_KEYS];  // Steigungen an den Stützstellen
} prof_t;

// Steigungen nach Fritsch-Carlson (PCHIP): an Hoch- und Tiefpunkten waagrecht, sonst
// gewichtetes harmonisches Mittel der Sekanten – die Kurve bleibt zwischen den Stützstellen
static void pchip_slopes(const float *ys, const float *vs, int n, float *m) {
    for (int i = 0; i < n; i++) {
        if (i == 0 || i == n - 1) {
            const int a = i == 0 ? 0 : n - 2;
            const float d = (vs[a + 1] - vs[a]) / (ys[a + 1] - ys[a]);
            m[i] = 0.5f * d;  // flacher Auslauf an den Enden
            continue;
        }
        const float h0 = ys[i] - ys[i - 1], h1 = ys[i + 1] - ys[i];
        const float d0 = (vs[i] - vs[i - 1]) / h0, d1 = (vs[i + 1] - vs[i]) / h1;
        if (d0 * d1 <= 0.0f) {
            m[i] = 0.0f;
        } else {
            const float w0 = 2.0f * h1 + h0, w1 = h1 + 2.0f * h0;
            m[i] = (w0 + w1) / (w0 / d0 + w1 / d1);
        }
    }
}

static void prof_finish(prof_t *p) {
    pchip_slopes(p->y, p->xc, p->n, p->mx);
    pchip_slopes(p->y, p->hw, p->n, p->mw);
    if (p->round_top) {
        p->mx[0] = p->mw[0] = 0.0f;
    }
    if (p->round_bottom) {
        p->mx[p->n - 1] = p->mw[p->n - 1] = 0.0f;
    }
}

// kubische Hermite-Interpolation mit vorberechneten Steigungen; dv bekommt die Ableitung
static float hermite(const float *ys, const float *vs, const float *ms, int n, float y, float *dv) {
    if (y <= ys[0]) {
        *dv = ms[0];
        return vs[0];
    }
    if (y >= ys[n - 1]) {
        *dv = ms[n - 1];
        return vs[n - 1];
    }
    int i = 0;
    while (y > ys[i + 1]) {
        i++;
    }
    const float h = ys[i + 1] - ys[i], t = (y - ys[i]) / h;
    const float m0 = ms[i] * h, m1 = ms[i + 1] * h;
    const float t2 = t * t, t3 = t2 * t;
    const float v = (2 * t3 - 3 * t2 + 1) * vs[i] + (t3 - 2 * t2 + t) * m0 +
                    (-2 * t3 + 3 * t2) * vs[i + 1] + (t3 - t2) * m1;
    *dv = ((6 * t2 - 6 * t) * vs[i] + (3 * t2 - 4 * t + 1) * m0 + (-6 * t2 + 6 * t) * vs[i + 1] +
           (3 * t2 - 2 * t) * m1) /
          h;
    return v;
}

// Abstand zum Rand eines Profils: waagrechter Abstand, mit der Randsteigung auf den echten
// Abstand umgerechnet; Enden rund (Halbkreis) oder gerade
static float sd_prof(const prof_t *p, float px, float py) {
    const int n = p->n;
    if (p->round_bottom && py > p->y[n - 1]) {
        return sd_circle(p->xc[n - 1], p->y[n - 1], p->hw[n - 1], px, py);
    }
    if (p->round_top && py < p->y[0]) {
        return sd_circle(p->xc[0], p->y[0], p->hw[0], px, py);
    }
    float dxc, dhw;
    const float xc = hermite(p->y, p->xc, p->mx, n, py, &dxc);
    const float hw = hermite(p->y, p->hw, p->mw, n, py, &dhw);
    const float slope = dxc + (px >= xc ? dhw : -dhw);
    float d = (fabsf(px - xc) - hw) / sqrtf(1.0f + slope * slope);
    if (!p->round_top) {
        d = fmaxf(d, p->y[0] - py);
    }
    if (!p->round_bottom) {
        d = fmaxf(d, py - p->y[n - 1]);
    }
    return d;
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
// Proportionen nach dem Zeichenkanon (gut 7,5 Kopflängen): Kinn bei 0,12, Schultern bei 0,18,
// Brust 0,26, Taille 0,37, Hüfte 0,49, Schritt 0,51, Knie 0,75, Knöchel 0,95.
// Stützstellen (y, Mitte, halbe Breite, Zunahme mit dem BMI); neutral ist der Mittelwert.
typedef struct {
    float y, xc, hw, f;
} key_t2;

// Rumpf unterhalb der Schulterrundung; Hals und Schultern sind eigene Teile
#define N_TORSO 9
static const key_t2 TORSO_F[N_TORSO] = {
    {0.190f, 0, 0.088f, 0.3f},  // in der Schulterrundung (verdeckt)
    {0.218f, 0, 0.085f, 0.8f},  // Achsel
    {0.258f, 0, 0.093f, 0.9f},  // Brust
    {0.292f, 0, 0.079f, 1.1f},  // unter der Brust
    {0.338f, 0, 0.068f, 1.4f},
    {0.372f, 0, 0.064f, 1.5f},  // Taille
    {0.432f, 0, 0.088f, 1.2f},  // Hüftschwung
    {0.490f, 0, 0.103f, 1.0f},  // Hüfte (breiteste Stelle)
    {0.512f, 0, 0.101f, 1.0f},  // Übergang in die Oberschenkel
};
static const key_t2 TORSO_M[N_TORSO] = {
    {0.192f, 0, 0.122f, 0.3f},  // in der Schulterrundung
    {0.226f, 0, 0.118f, 0.8f},  // Achsel, breiter Rücken
    {0.266f, 0, 0.112f, 0.9f},  // Brust
    {0.310f, 0, 0.100f, 1.2f},
    {0.350f, 0, 0.090f, 1.4f},
    {0.392f, 0, 0.085f, 1.5f},  // Taille
    {0.442f, 0, 0.087f, 1.2f},
    {0.490f, 0, 0.092f, 1.0f},  // Hüfte
    {0.518f, 0, 0.091f, 1.0f},
};

// Beine: Mitte und halbe Breite; die Wade ist außen höher gewölbt als innen (Mitte wandert)
#define N_LEG 11
static const key_t2 LEG_F[N_LEG] = {
    {0.470f, 0.046f, 0.040f, 1.0f},   // in der Hüfte (rund, verdeckt)
    {0.512f, 0.0505f, 0.0505f, 1.0f}, // außen bündig mit der Hüfte
    {0.580f, 0.048f, 0.050f, 1.1f},   // Oberschenkel
    {0.650f, 0.042f, 0.042f, 1.0f},
    {0.712f, 0.036f, 0.0315f, 0.7f},
    {0.745f, 0.0335f, 0.027f, 0.6f},  // Knie
    {0.790f, 0.038f, 0.031f, 0.6f},   // Wade außen
    {0.832f, 0.0345f, 0.0285f, 0.6f}, // Wade innen
    {0.900f, 0.0325f, 0.0195f, 0.5f},
    {0.942f, 0.031f, 0.0140f, 0.4f},  // Knöchel
    {0.956f, 0.031f, 0.0150f, 0.4f},
};
static const key_t2 LEG_M[N_LEG] = {
    {0.475f, 0.044f, 0.038f, 1.0f},  {0.518f, 0.0455f, 0.0455f, 1.0f},
    {0.590f, 0.047f, 0.047f, 1.1f},  {0.660f, 0.044f, 0.041f, 1.0f},
    {0.722f, 0.041f, 0.0325f, 0.7f}, {0.752f, 0.040f, 0.029f, 0.6f},
    {0.798f, 0.045f, 0.035f, 0.6f},  {0.842f, 0.041f, 0.032f, 0.6f},
    {0.905f, 0.039f, 0.0215f, 0.5f}, {0.944f, 0.036f, 0.0160f, 0.4f},
    {0.956f, 0.036f, 0.0165f, 0.4f},
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

// Wert für Frauen, Männer oder (ohne Angabe) den Mittelwert
static float by_sex(int sex, float female, float male) {
    return sex == SEX_FEMALE ? female : sex == SEX_MALE ? male : 0.5f * (female + male);
}

typedef enum { HAIR_NONE, HAIR_SHORT, HAIR_LONG } hair_t;

typedef struct {
    prof_t torso, neck, leg[2], arm[2], hair;
    hair_t hair_style;
    bool ears;
    cap_t shoulder[2], foot[2];
    float hy, hr;  // Schädel
    float jy, jr;  // Kiefer
    float reach;   // größte halbe Breite (Hände)
} figure_t;

static float torso_hw_at(const figure_t *f, float v) {
    float d;
    return hermite(f->torso.y, f->torso.hw, f->torso.mw, f->torso.n, v, &d);
}

static void build_figure(figure_t *f, const avatar_t *av) {
    const float k = build(av);
    const int sex = av->sex;
    *f = (figure_t){0};

    for (int i = 0; i < N_TORSO; i++) {
        const key_t2 t = blend_key(&TORSO_F[i], &TORSO_M[i], sex);
        prof_add(&f->torso, t.y, 0.0f, t.hw * part(k, t.f));
    }
    prof_finish(&f->torso);

    for (int s = 0; s < 2; s++) {
        const float sg = s == 0 ? -1.0f : 1.0f;  // −: rechte Körperseite (links im Bild)
        prof_t *l = &f->leg[s];
        for (int i = 0; i < N_LEG; i++) {
            const key_t2 kk = blend_key(&LEG_F[i], &LEG_M[i], sex);
            const float spread = part(k, kk.f >= 1.0f ? 1.0f : 0.40f);
            prof_add(l, kk.y, sg * kk.xc * spread, kk.hw * part(k, kk.f));
        }
        l->round_top = true;
        l->round_bottom = true;
        prof_finish(l);
        // Fuß von vorn: kurz, leicht nach außen gedreht
        const float ay = l->y[N_LEG - 1], ax = l->xc[N_LEG - 1], aw = l->hw[N_LEG - 1];
        f->foot[s] = (cap_t){ax, ay + 0.010f, ax + sg * 0.022f, 0.986f, aw * 1.10f, aw * 0.82f};
    }

    // Schultern: je eine Rundung von der Halsgrube leicht abfallend nach außen; der Nacken
    // entsteht als Kehle zum Hals
    const float sh = by_sex(sex, 0.100f, 0.130f) * part(k, 0.3f);
    const float rs = by_sex(sex, 0.030f, 0.039f) * part(k, 0.3f);
    const float ys = 0.180f + 0.45f * rs, drop = by_sex(sex, 0.022f, 0.019f);
    for (int s = 0; s < 2; s++) {
        const float sg = s == 0 ? -1.0f : 1.0f;
        f->shoulder[s] = (cap_t){0.0f, ys - drop, sg * (sh - rs), ys, rs * 0.9f, rs};
    }

    // Arme leicht abgespreizt: Deltamuskel, Bizeps, Ellbogen auf Taillenhöhe, kräftiger
    // Unterarm, schmales Handgelenk, Hand bis zur Oberschenkelmitte; durchgehend Abstand zum Rumpf
    const float waist = torso_hw_at(f, by_sex(sex, 0.372f, 0.392f));
    const float hip = torso_hw_at(f, 0.490f);
    const float w_up = by_sex(sex, 0.025f, 0.031f) * part(k, 0.9f);
    const float w_el = by_sex(sex, 0.0165f, 0.021f) * part(k, 0.7f);
    const float w_fa = by_sex(sex, 0.0200f, 0.0255f) * part(k, 0.7f);
    const float w_wr = by_sex(sex, 0.0115f, 0.0150f) * part(k, 0.5f);
    const float w_pa = by_sex(sex, 0.0155f, 0.0195f) * part(k, 0.4f);
    const float y_el = by_sex(sex, 0.352f, 0.362f), y_wr = by_sex(sex, 0.478f, 0.488f);
    const float el = fmaxf(sh + 0.012f, waist + w_el + 0.034f);
    const float wr = fmaxf(el + 0.024f, hip + w_pa + 0.030f);
    for (int s = 0; s < 2; s++) {
        const float sg = s == 0 ? -1.0f : 1.0f;
        prof_t *a = &f->arm[s];
        const float x0 = sh - rs;
        prof_add(a, ys, sg * x0, rs);  // gleiche Rundung wie das Schulterende
        prof_add(a, ys + 0.045f, sg * (x0 + 0.30f * (el - x0)), w_up);         // Deltamuskel
        prof_add(a, ys + 0.100f, sg * (x0 + 0.62f * (el - x0)), w_up * 0.90f); // Bizeps
        prof_add(a, y_el, sg * el, w_el);                                       // Ellbogen
        prof_add(a, y_el + 0.032f, sg * (el + 0.22f * (wr - el)), w_fa);       // Unterarm
        prof_add(a, y_el + 0.085f, sg * (el + 0.62f * (wr - el)), w_fa * 0.78f);
        prof_add(a, y_wr, sg * wr, w_wr);                                       // Handgelenk
        prof_add(a, y_wr + 0.030f, sg * (wr + 0.003f), w_pa);                  // Handballen
        prof_add(a, y_wr + 0.064f, sg * (wr + 0.004f), w_pa * 0.86f);          // Finger
        prof_add(a, y_wr + 0.084f, sg * (wr + 0.004f), w_pa * 0.58f);
        a->round_top = true;
        a->round_bottom = true;
        prof_finish(a);
    }
    f->reach = wr + w_pa + 0.016f;

    // Kopf: eiförmig, Schädel oben breiter als der Kiefer; Kinder mit größerem Kopf
    const float headk = av->height_cm < 150 ? 1.0f + (150.0f - (float)av->height_cm) / 150.0f
                                            : 1.0f;
    f->hr = by_sex(sex, 0.0465f, 0.0495f) * headk;
    f->hy = f->hr * 1.06f;
    f->jr = f->hr * by_sex(sex, 0.64f, 0.74f);
    f->jy = f->hy + f->hr * by_sex(sex, 0.78f, 0.74f);
    f->ears = sex == SEX_MALE;  // bei Frauen verdecken die Haare die Ohren

    // Hals: kurz und kräftig genug, unten breiter
    {
        const float nk = part(k, 0.5f), base = by_sex(sex, 0.0205f, 0.0280f);
        prof_t *n = &f->neck;
        prof_add(n, f->jy, 0.0f, base * nk);
        prof_add(n, f->jy + f->jr * 0.85f, 0.0f, base * 1.02f * nk);
        prof_add(n, ys - drop - 0.004f, 0.0f, base * 1.18f * nk);
        prof_add(n, ys, 0.0f, base * 1.50f * nk);
        prof_finish(n);
    }

    // Lange Haare (Frauen): Volumen um den Kopf, fallen neben dem Hals bis auf die Schultern
    // und verschwinden dahinter
    f->hair_style = sex == SEX_FEMALE ? HAIR_LONG : sex == SEX_MALE ? HAIR_SHORT : HAIR_NONE;
    if (f->hair_style == HAIR_LONG) {
        const float hy = f->hy, hr = f->hr;
        prof_t *h = &f->hair;
        prof_add(h, hy - 0.02f * hr, 0.0f, hr * 1.17f);
        prof_add(h, hy + 0.70f * hr, 0.0f, hr * 1.16f);
        prof_add(h, hy + 1.45f * hr, 0.0f, hr * 1.12f);
        prof_add(h, ys - drop, 0.0f, hr * 1.30f);
        prof_add(h, ys + 0.020f, 0.0f, hr * 1.55f);
        prof_add(h, ys + 0.060f, 0.0f, hr * 1.55f);
        h->round_top = true;
        h->round_bottom = true;
        prof_finish(h);
    }
}

// Haare hinter dem Körper (nur lange Haare)
static float hair_back_sd(const figure_t *f, float u, float v) {
    return sd_prof(&f->hair, u, v);
}

// Haare vor dem Kopf: Haarkappe ohne Gesicht. Frauen: Mittelscheitel, der Pony fällt in zwei
// runden Hälften zu beiden Seiten, die sich am Scheitel berühren. Männer: kurz, Haaransatz als
// flacher Bogen mit Seitenscheitel.
static float hair_front_sd(const figure_t *f, float u, float v) {
    const float hy = f->hy, hr = f->hr;
    const bool lng = f->hair_style == HAIR_LONG;
    const float cap = sd_circle(0.0f, hy - 0.08f * hr, (lng ? 1.13f : 1.05f) * hr, u, v);
    const float face = smin(sd_circle(0.0f, hy + 0.25f * hr, 0.93f * hr, u, v),
                            sd_circle(0.0f, f->jy, f->jr * 1.10f, u, v), 0.020f);
    float open;  // sichtbare Stirn und Gesicht
    if (lng) {
        const float fl = sd_circle(-0.68f * hr, hy - 0.40f * hr, 0.68f * hr, u, v);
        const float fr = sd_circle(0.68f * hr, hy - 0.40f * hr, 0.68f * hr, u, v);
        const float above = (hy - 0.36f * hr) - v;  // über dem Scheitelpunkt nur Haar
        open = fmaxf(fmaxf(face, above), fmaxf(-fl, -fr));
    } else {
        // unterhalb dieses großen Kreises liegt Stirn, darüber Haar
        open = fmaxf(face, sd_circle(0.25f * hr, hy + 2.60f * hr, 2.95f * hr, u, v));
    }
    return fmaxf(cap, -open);
}

static float body_sd(const figure_t *f, float u, float v) {
    float d = smin(sd_cap(&f->shoulder[0], u, v), sd_cap(&f->shoulder[1], u, v), 0.010f);
    d = smin(d, sd_prof(&f->neck, u, v), 0.030f);
    d = smin(d, sd_prof(&f->torso, u, v), 0.014f);
    float head = smin(sd_circle(0.0f, f->hy, f->hr, u, v), sd_circle(0.0f, f->jy, f->jr, u, v),
                      0.030f);
    if (f->ears) {
        for (int s = 0; s < 2; s++) {
            const float ex = (s == 0 ? -1.0f : 1.0f) * f->hr * 0.96f;
            head = smin(head, sd_circle(ex * 0.97f, f->hy + 0.32f * f->hr, 0.16f * f->hr, u, v),
                        0.008f);
        }
    }
    d = smin(d, head, 0.012f);
    for (int s = 0; s < 2; s++) {
        const float dl = smin(sd_prof(&f->leg[s], u, v), sd_cap(&f->foot[s], u, v), 0.012f);
        d = fminf(d, dl);  // bündig mit der Hüfte: eine Kehle würde dort eine Beule machen
        d = smin(d, sd_prof(&f->arm[s], u, v), 0.008f);
    }
    return d;
}

// ------------------------------------------------------------------ Zeichnen
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
    const uint32_t hair = gfx_mix(fill, 0x000000u, 105);
    const float inv = 1.0f / fh;

    const int hx0 = (int)floorf(cx - 0.11f * fh), hx1 = (int)ceilf(cx + 0.11f * fh);
    const int hy0 = (int)floorf(top) - 3, hy1 = (int)ceilf(top + 0.30f * fh);
    if (f.hair_style == HAIR_LONG) {
        for (int y = hy0; y <= hy1; y++) {
            const float v = ((float)y + 0.5f - top) * inv;
            for (int x = hx0; x <= hx1; x++) {
                const float u = ((float)x + 0.5f - cx) * inv;
                const float cov = clampf(0.5f - hair_back_sd(&f, u, v) * fh, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    gfx_blend(c, x, y, hair, (int)(cov * 256.0f));
                }
            }
        }
    }

    const float half = f.reach * fh + 3.0f;
    const int x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
    for (int y = (int)floorf(top) - 2; y <= by + bh; y++) {
        const float v = ((float)y + 0.5f - top) * inv;
        for (int x = x0; x <= x1; x++) {
            const float u = ((float)x + 0.5f - cx) * inv;
            const float cov = clampf(0.5f - body_sd(&f, u, v) * fh, 0.0f, 1.0f);
            if (cov > 0.0f) {
                gfx_blend(c, x, y, fill, (int)(cov * 256.0f));
            }
        }
    }
    if (f.hair_style != HAIR_NONE) {
        for (int y = hy0; y <= (int)ceilf(top + 0.12f * fh); y++) {
            const float v = ((float)y + 0.5f - top) * inv;
            for (int x = hx0; x <= hx1; x++) {
                const float u = ((float)x + 0.5f - cx) * inv;
                const float cov = clampf(0.5f - hair_front_sd(&f, u, v) * fh, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    gfx_blend(c, x, y, hair, (int)(cov * 256.0f));
                }
            }
        }
    }

    if (sites) {
        // Anker: RA rechts unter dem Schlüsselbein, LA links und RL rechts am unteren Rippenbogen
        // (Ableitung II). „Rechts“ der Person liegt im Bild links.
        const float ya = by_sex(av->sex, 0.214f, 0.222f), yr = by_sex(av->sex, 0.312f, 0.325f);
        const float xa = fminf(0.060f, 0.65f * torso_hw_at(&f, ya));
        const float xr = fminf(0.050f, 0.68f * torso_hw_at(&f, yr));
        sites->ra = (avatar_pt_t){cx - xa * fh, top + ya * fh};
        sites->la = (avatar_pt_t){cx + xr * fh, top + yr * fh};
        sites->rl = (avatar_pt_t){cx - xr * fh, top + yr * fh};
        sites->r = clampf(0.032f * fh, 3.6f, 7.0f);
    }
}
