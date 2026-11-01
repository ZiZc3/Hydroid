// overlay_art.c — the FPS counter's font and colours, and the cursor: Ruffle Flash PS5's
// pointer (src/ui/gfx.rs pointer(): a white arrow with a dark edge and a soft shadow),
// filled with 4x4 samples a pixel as Ruffle's fill_polygon does.

#include <string.h>

#include "overlay_art.h"

const uint8_t overlay_font5x7[13][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, // F
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, // P
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, // S
};

void overlay_fps_color(int fps, float rgb[3]) {
    if (fps >= 50) { rgb[0] = 0.2f; rgb[1] = 1.0f; rgb[2] = 0.3f; }
    else if (fps >= 30) { rgb[0] = 1.0f; rgb[1] = 0.9f; rgb[2] = 0.1f; }
    else if (fps >= 20) { rgb[0] = 1.0f; rgb[1] = 0.55f; rgb[2] = 0.1f; }
    else { rgb[0] = 1.0f; rgb[1] = 0.2f; rgb[2] = 0.2f; }
}

static const float arrow[7][2] = { { 0, 0 }, { 0, 30 }, { 7.5f, 23 }, { 12.5f, 34.5f },
                                   { 17.5f, 32.5f }, { 12.5f, 21.5f }, { 22, 21.5f } };

static int inside(const float (*p)[2], int n, float x, float y) {
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i][1] > y) != (p[j][1] > y) && x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0])
            in = !in;
    return in;
}

// one of Ruffle's three fills, over what's there (straight alpha, 0..1)
static void fill(float *px, int w, int h, float tip, float scale, float dx, float dy, float grow, const float rgb[3],
                 float alpha) {
    float pts[7][2];
    for (int i = 0; i < 7; i++) {
        pts[i][0] = tip + (dx + (arrow[i][0] - 8) * grow + 8) * scale;
        pts[i][1] = tip + (dy + (arrow[i][1] - 15) * grow + 15) * scale;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) hits += inside(pts, 7, x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
            if (!hits) continue;
            float a = hits / 16.0f * alpha, *d = px + (y * w + x) * 4;
            float out_a = a + d[3] * (1 - a);
            for (int c = 0; c < 3; c++) d[c] = out_a > 0 ? (rgb[c] * a + d[c] * d[3] * (1 - a)) / out_a : 0;
            d[3] = out_a;
        }
}

void overlay_cursor_art(float *rgba, int w, int h, float tip, float scale) {
    static const float ink[3] = { 0x0B / 255.0f, 0x0B / 255.0f, 0x10 / 255.0f }, white[3] = { 1, 1, 1 };
    memset(rgba, 0, sizeof(float) * 4 * (size_t)w * (size_t)h);
    fill(rgba, w, h, tip, scale, 2, 3, 1.12f, ink, 0.35f); // shadow
    fill(rgba, w, h, tip, scale, 0, 0, 1.12f, ink, 0.9f);  // edge
    fill(rgba, w, h, tip, scale, 0, 0, 1.0f, white, 1.0f); // arrow
}

// ---- ShotBox (the debug tool): the game's picture saved at chosen seconds ---------------
// tools.txt "ShotBox 10 30 60" (seconds after the first frame; just "ShotBox": 10 30 60 120):
// shot_<s>s.bmp in the game's folder, 960x540 (FTP them; Windows opens BMP as it is).
#include "game_settings.h"
#include "android_shim.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double shot_t0;
static int shot_secs[16], shot_n = -1, shot_next;
// the shot due now: its second, else 0 (t0: the first frame)
int droid_shot_due(void) {
    if (shot_n < 0) {
        shot_n = 0;
        if (!droid_tool("ShotBox")) return 0;
        const char *a = droid_tool_args("ShotBox");
        if (!a[0]) a = "10 30 60 120";
        for (char *e; shot_n < 16;) {
            long v = strtol(a, &e, 10);
            if (e == a) break;
            if (v > 0) shot_secs[shot_n++] = (int)v;
            a = e;
        }
        droid_log("ShotBox: %d shot(s) planned", shot_n);
    }
    if (shot_next >= shot_n) return 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double now = t.tv_sec + t.tv_nsec / 1e9;
    if (!shot_t0) shot_t0 = now;
    if (now - shot_t0 < shot_secs[shot_next]) return 0;
    return shot_secs[shot_next++];
}
// pixels (4 bytes each, rows top first unless bottom_up, BGRA or RGBA) as a half-size BMP
void droid_shot_write(const uint8_t *px, int w, int h, size_t stride, int bgra, int bottom_up, int sec) {
    int ow = w / 2, oh = h / 2;
    size_t row = ((size_t)ow * 3 + 3) & ~(size_t)3;
    uint8_t *bmp = calloc(1, 54 + row * (size_t)oh);
    if (!bmp) return;
    uint32_t file = (uint32_t)(54 + row * (size_t)oh);
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &file, 4);
    hdr[10] = 54, hdr[14] = 40;
    memcpy(hdr + 18, &ow, 4);
    memcpy(hdr + 22, &oh, 4);
    hdr[26] = 1, hdr[28] = 24;
    memcpy(bmp, hdr, 54);
    for (int y = 0; y < oh; y++) { // a BMP's rows go bottom first
        int sy = bottom_up ? y * 2 : (oh - 1 - y) * 2;
        const uint8_t *src = px + (size_t)sy * stride;
        uint8_t *d = bmp + 54 + row * (size_t)y;
        for (int x = 0; x < ow; x++, d += 3) {
            const uint8_t *p = src + (size_t)x * 2 * 4;
            d[0] = bgra ? p[0] : p[2], d[1] = p[1], d[2] = bgra ? p[2] : p[0];
        }
    }
    char name[64], path[400];
    snprintf(name, sizeof(name), "shot_%ds.bmp", sec);
    FILE *f = fopen(game_path(path, sizeof(path), name), "wb");
    if (f) {
        fwrite(bmp, 1, file, f);
        fclose(f);
        droid_log("ShotBox: %s (%dx%d)", path, ow, oh);
    }
    free(bmp);
}
