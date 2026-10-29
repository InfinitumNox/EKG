#include "touchcal.h"

#include <math.h>

static double det3(const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// löst M·k = v mit der Cramerschen Regel
static bool solve3(const double m[3][3], const double v[3], double k[3]) {
    double d = det3(m);
    if (fabs(d) < 1e-9) {
        return false;
    }
    for (int col = 0; col < 3; col++) {
        double t[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                t[i][j] = j == col ? v[i] : m[i][j];
            }
        }
        k[col] = det3(t) / d;
    }
    return true;
}

bool touchcal_solve(const float *sx, const float *sy, const float *rx, const float *ry, int n,
                    touch_cal_t *out, float *max_err) {
    if (n < 3) {
        return false;
    }
    double m[3][3] = {{0}}, vx[3] = {0}, vy[3] = {0};
    for (int i = 0; i < n; i++) {
        double r[3] = {rx[i], ry[i], 1.0};
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                m[a][b] += r[a] * r[b];
            }
            vx[a] += r[a] * sx[i];
            vy[a] += r[a] * sy[i];
        }
    }
    double kx[3], ky[3];
    if (!solve3(m, vx, kx) || !solve3(m, vy, ky)) {
        return false;
    }
    *out = (touch_cal_t){(float)kx[0], (float)kx[1], (float)kx[2],
                         (float)ky[0], (float)ky[1], (float)ky[2]};
    float worst = 0.0f;
    for (int i = 0; i < n; i++) {
        int x, y;
        touchcal_apply(out, (int)rx[i], (int)ry[i], &x, &y);
        float e = hypotf((float)x - sx[i], (float)y - sy[i]);
        worst = e > worst ? e : worst;
    }
    *max_err = worst;
    return true;
}

void touchcal_apply(const touch_cal_t *cal, int rx, int ry, int *x, int *y) {
    *x = (int)lrintf(cal->a * (float)rx + cal->b * (float)ry + cal->c);
    *y = (int)lrintf(cal->d * (float)rx + cal->e * (float)ry + cal->f);
}
