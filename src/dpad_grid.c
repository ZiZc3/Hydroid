// dpad_grid.c — the D-pad on a board of tiles (Candy Crush's candies): the cursor steps one tile,
// landing on its centre. The tiles' size and place are found in the picture itself, a region
// around the cursor read back at a swap (gl_overlay.inc): tiles are colourful, the gaps between
// them dull, so the colourfulness across the region repeats with the tiles' pitch
// (autocorrelation) and peaks at their centres (its phase). A board of any size, any level.

#include "dpad_grid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "android_shim.h"

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static volatile int wanted;
static volatile float want_x, want_y;
static dpad_grid last;

void dpad_grid_request(float cx, float cy) {
    want_x = cx, want_y = cy;
    wanted = 1;
}

int dpad_grid_wanted(float *cx, float *cy) {
    if (!wanted) return 0;
    wanted = 0;
    *cx = want_x, *cy = want_y;
    return 1;
}

// the profile's pitch (autocorrelation: the first strong peak) and the phase of its peaks
static int profile_grid(const float *p, int n, int min_lag, int max_lag, float *pitch, float *phase, float *conf) {
    float mean = 0;
    for (int i = 0; i < n; i++) mean += p[i];
    mean /= n;
    float var = 0;
    for (int i = 0; i < n; i++) var += (p[i] - mean) * (p[i] - mean);
    if (var <= 1e-3f || max_lag >= n / 2) return 0;
    static float r[2048];
    float best = 0;
    for (int L = min_lag; L <= max_lag; L++) {
        float s = 0;
        for (int i = 0; i + L < n; i++) s += (p[i] - mean) * (p[i + L] - mean);
        r[L] = s / var * n / (n - L);
        if (r[L] > best) best = r[L];
    }
    if (best < 0.25f) return 0;
    int lag = 0;
    for (int L = min_lag + 1; L < max_lag && !lag; L++) // the first peak near the best (not its multiples)
        if (r[L] >= r[L - 1] && r[L] >= r[L + 1] && r[L] >= 0.8f * best) lag = L;
    if (!lag) return 0;
    // the peak between samples (a parabola through it)
    float a = r[lag - 1], b = r[lag], c = r[lag + 1], d = a - 2 * b + c;
    float L = lag + (d < 0 ? 0.5f * (a - c) / d : 0);
    double sx = 0, cx = 0;
    for (int i = 0; i < n; i++) {
        double ang = 2 * M_PI * i / L;
        sx += (p[i] - mean) * sin(ang);
        cx += (p[i] - mean) * cos(ang);
    }
    float ph = (float)(atan2(sx, cx) / (2 * M_PI) * L);
    if (ph < 0) ph += L;
    *pitch = L, *phase = ph, *conf = b;
    return 1;
}

void dpad_grid_analyze(const uint8_t *px, int w, int h, int bottom_up, float x0, float y0, float unit) {
    static float col[2048], row[2048];
    if (w > 2048 || h > 2048) return;
    memset(col, 0, sizeof(float) * (size_t)w);
    memset(row, 0, sizeof(float) * (size_t)h);
    for (int y = 0; y < h; y++) {
        const uint8_t *r = px + (size_t)(bottom_up ? h - 1 - y : y) * w * 4;
        for (int x = 0; x < w; x++, r += 4) {
            int mx = r[0] > r[1] ? (r[0] > r[2] ? r[0] : r[2]) : (r[1] > r[2] ? r[1] : r[2]);
            int mn = r[0] < r[1] ? (r[0] < r[2] ? r[0] : r[2]) : (r[1] < r[2] ? r[1] : r[2]);
            float sat = (float)(mx - mn); // colourfulness
            col[x] += sat, row[y] += sat;
        }
    }
    float pxw, phx, cfx, pyh, phy, cfy;
    const int lo = (int)(26 / unit), hi_x = (int)(220 / unit), hi_y = (int)(220 / unit);
    int ok = profile_grid(col, w, lo, hi_x < w / 2 - 1 ? hi_x : w / 2 - 1, &pxw, &phx, &cfx) &&
             profile_grid(row, h, lo, hi_y < h / 2 - 1 ? hi_y : h / 2 - 1, &pyh, &phy, &cfy);
    if (ok && (pxw / pyh > 1.35f || pyh / pxw > 1.35f)) ok = 0; // tiles are about square: else not a board
    dpad_grid g = { 0 };
    g.at = now_s();
    g.ok = ok;
    g.x0 = x0, g.y0 = y0, g.x1 = x0 + w * unit, g.y1 = y0 + h * unit;
    if (ok) {
        g.pitch_x = pxw * unit, g.pitch_y = pyh * unit;
        g.cx = x0 + phx * unit, g.cy = y0 + phy * unit; // a tile's centre
    }
    last = g;
    static int told;
    if (told++ < 12)
        droid_log("dpad: the board around the cursor: %s (tiles %.0fx%.0f px, a centre at %.0f,%.0f; sure %.2f / %.2f)",
                  ok ? "found" : "none", g.pitch_x, g.pitch_y, g.cx, g.cy, ok ? cfx : 0, ok ? cfy : 0);
}

int dpad_grid_get(dpad_grid *g) {
    *g = last;
    return last.at > 0;
}
