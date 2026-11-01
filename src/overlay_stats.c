// overlay_stats.c — the in-game stats panel (overlay_stats.h). A pill with the frame rate, or a
// panel: the frame rate, the frame time, the slowest frame of the last half second, the CPU,
// and a line of the last frames' times. Outfit (the menus' font), baked once a scale with
// stb_truetype. Drawn on a thread of its own 15 times a second: the game's render thread only
// takes the newest picture (drawn there, a 4K panel cost milliseconds a frame: Geometry Dash's
// frame times jumped, and the panel showed its own cost).

#include "overlay_stats.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "android_shim.h"
#include "overlay_art.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// ---- the numbers
#define HIST 96 // the line's frames
static float hist[HIST]; // frame times, ms
static int hist_at;
static double last_frame, win_start, cpu_win_start;
static int win_frames, shown_fps = -1;
static float win_worst, shown_ms, shown_worst, shown_cpu = -1;
static double cpu_start;

static double cpu_s(void) { // all the app's threads' CPU time
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) return -1;
    return t.tv_sec + t.tv_nsec / 1e9;
}

void overlay_stats_tick(void) {
    double now = now_s();
    if (last_frame) {
        float ms = (float)((now - last_frame) * 1000);
        hist[hist_at++ % HIST] = ms;
        if (ms > win_worst) win_worst = ms;
    }
    last_frame = now;
    if (!win_start) {
        win_start = cpu_win_start = now;
        cpu_start = cpu_s();
    }
    win_frames++;
    if (now - win_start >= 0.5) {
        shown_fps = (int)(win_frames / (now - win_start) + 0.5);
        shown_ms = (float)((now - win_start) * 1000 / win_frames);
        shown_worst = win_worst;
        double c = cpu_s();
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (c >= 0 && cpu_start >= 0 && cores > 0)
            shown_cpu = (float)((c - cpu_start) / ((now - cpu_win_start) * cores) * 100);
        cpu_start = c, cpu_win_start = now;
        win_start = now;
        win_frames = 0;
        win_worst = 0;
    }
}

// ---- the font: Outfit, two sizes, baked at the scale
typedef struct {
    stbtt_bakedchar ch[96];
    unsigned char *atlas;
    int aw, ah;
    float px;
} face;
static face big, small;
static float faces_scale;
static unsigned char *ttf_semi, *ttf_bold;

static unsigned char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static void bake(face *f, const unsigned char *ttf, float px) {
    free(f->atlas);
    f->px = px;
    f->aw = 1024, f->ah = px > 60 ? 512 : 256; // (every letter at 4K: the puzzle's panel has words in the big face)
    f->atlas = calloc((size_t)f->aw * f->ah, 1);
    if (f->atlas) stbtt_BakeFontBitmap(ttf, 0, px, f->atlas, f->aw, f->ah, 32, 96, f->ch);
}

static int faces_ready(float scale) {
    if (!ttf_semi) {
        ttf_semi = read_file(APP0 "/assets/fonts/Outfit-SemiBold.ttf");
        ttf_bold = read_file(APP0 "/assets/fonts/Outfit-Bold.ttf");
        if (!ttf_semi || !ttf_bold) {
            static int said;
            if (!said++) droid_log("stats overlay: no Outfit in assets/fonts");
            return 0;
        }
    }
    if (faces_scale != scale) {
        bake(&big, ttf_bold, 34 * scale);
        bake(&small, ttf_semi, 17 * scale);
        faces_scale = scale;
    }
    return big.atlas && small.atlas;
}

// ---- the picture: premultiplied RGBA floats while it's drawn
#define IMG_MAX_W 1600 // (the puzzle's address panel at 4K; the stats panel: 600 x 264)
#define IMG_MAX_H 440
static float px[IMG_MAX_W * IMG_MAX_H * 4];
static uint8_t out[IMG_MAX_W * IMG_MAX_H * 4];
static int iw, ih;

static void blend(int x, int y, float r, float g, float b, float a) { // "over", premultiplied
    if (x < 0 || y < 0 || x >= iw || y >= ih || a <= 0) return;
    float *p = px + (y * iw + x) * 4;
    p[0] = r * a + p[0] * (1 - a), p[1] = g * a + p[1] * (1 - a), p[2] = b * a + p[2] * (1 - a);
    p[3] = a + p[3] * (1 - a);
}

static float rrect_cover(float x, float y, float w, float h, float r, float fx, float fy) { // 0..1 inside
    float cx = fx - (x + w / 2), cy = fy - (y + h / 2);
    float qx = fabsf(cx) - w / 2 + r, qy = fabsf(cy) - h / 2 + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
    return fminf(fmaxf(0.5f - d, 0), 1);
}

static void rrect(float x, float y, float w, float h, float r, float cr, float cg, float cb, float a) {
    for (int j = (int)y; j < (int)(y + h + 1); j++)
        for (int i = (int)x; i < (int)(x + w + 1); i++) {
            float c = rrect_cover(x, y, w, h, r, i + 0.5f, j + 0.5f);
            if (c > 0) blend(i, j, cr, cg, cb, a * c);
        }
}

static float text_w(const face *f, const char *s) {
    float w = 0;
    for (; *s; s++)
        if ((unsigned char)*s >= 32 && (unsigned char)*s < 128) w += f->ch[*s - 32].xadvance;
    return w;
}

static void text(const face *f, float x, float baseline, const char *s, float r, float g, float b, float a) {
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s >= 128) continue;
        const stbtt_bakedchar *c = &f->ch[*s - 32];
        int gx = (int)floorf(x + c->xoff + 0.5f), gy = (int)floorf(baseline + c->yoff + 0.5f);
        for (int j = c->y0; j < c->y1; j++)
            for (int i = c->x0; i < c->x1; i++) {
                float cov = f->atlas[j * f->aw + i] / 255.0f;
                if (cov > 0) blend(gx + (i - c->x0), gy + (j - c->y0), r, g, b, a * cov);
            }
        x += c->xadvance;
    }
}

// a soft green, yellow, orange or red for the frame rate (overlay_art.c's colours, a little lighter)
static void fps_rgb(int fps, float rgb[3]) {
    overlay_fps_color(fps, rgb);
    for (int k = 0; k < 3; k++) rgb[k] = rgb[k] * 0.8f + 0.2f;
}

// the picture the thread drew last (shared_img, its size and version, under pub_lock), and the
// copy the render thread reads (read_img)
static uint8_t shared_img[IMG_MAX_W * IMG_MAX_H * 4], read_img[IMG_MAX_W * IMG_MAX_H * 4];
static int shared_w, shared_h, shared_mode, shared_premul, read_w, read_h;
static float shared_scale;
static unsigned shared_ver, read_ver;
static pthread_mutex_t pub_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int want_mode, want_premul; // what the render thread asks for
static volatile float want_scale;

static void to_bytes(int premul);

static int draw(int mode, float scale, int premul) { // into px / out (iw x ih); 0: nothing yet
    if (mode == STATS_OFF || shown_fps < 0) return 0;
    if (!faces_ready(scale)) return 0;
    const float S = scale;
    const float soft[3] = { 160 / 255.0f, 200 / 255.0f, 182 / 255.0f };
    float rgb[3];
    fps_rgb(shown_fps, rgb);
    char fps[8];
    snprintf(fps, sizeof(fps), "%d", shown_fps > 999 ? 999 : shown_fps);
    if (mode == STATS_FPS) { // a pill: the number, "FPS"
        float tw = text_w(&big, fps) + text_w(&small, " FPS");
        iw = (int)(tw + 40 * S), ih = (int)(52 * S);
    } else {
        iw = (int)(300 * S), ih = (int)(132 * S);
    }
    if (iw > IMG_MAX_W) iw = IMG_MAX_W;
    if (ih > IMG_MAX_H) ih = IMG_MAX_H;
    memset(px, 0, sizeof(float) * 4 * iw * ih);
    // the glass: dark green, a faint rim, a light along its top
    const float R = (mode == STATS_FPS ? ih / 2.0f : 18 * S);
    rrect(0, 0, iw - 1, ih - 1, R, 0.36f, 0.86f, 0.58f, 0.30f);
    rrect(1 * S, 1 * S, iw - 1 - 2 * S, ih - 1 - 2 * S, R - S, 0.035f, 0.12f, 0.085f, 0.86f);
    for (int j = (int)(2 * S); j < (int)(ih * 0.4f); j++) // the top's light, fading down
        for (int i = (int)(2 * S); i < iw - (int)(2 * S); i++) {
            float c = rrect_cover(2 * S, 2 * S, iw - 1 - 4 * S, ih - 1 - 4 * S, R - 2 * S, i + 0.5f, j + 0.5f);
            if (c > 0) blend(i, j, 1, 1, 1, 0.06f * c * (1 - j / (ih * 0.4f)));
        }
    if (mode == STATS_FPS) {
        float x = 20 * S, base = ih / 2.0f + big.px * 0.36f;
        text(&big, x, base, fps, rgb[0], rgb[1], rgb[2], 1);
        text(&small, x + text_w(&big, fps) + 4 * S, base, "FPS", soft[0], soft[1], soft[2], 1);
    } else {
        float x = 18 * S, base = 46 * S;
        text(&big, x, base, fps, rgb[0], rgb[1], rgb[2], 1);
        text(&small, x + text_w(&big, fps) + 6 * S, base, "FPS", soft[0], soft[1], soft[2], 1);
        // the right column: frame time, the slowest frame, the CPU
        char l1[32], l2[32], l3[32];
        snprintf(l1, sizeof(l1), "%.1f ms", shown_ms);
        snprintf(l2, sizeof(l2), "slowest %.0f ms", shown_worst);
        if (shown_cpu >= 0) snprintf(l3, sizeof(l3), "CPU %.0f%%", shown_cpu);
        else l3[0] = 0;
        float rx = iw - 18 * S;
        text(&small, rx - text_w(&small, l1), 26 * S, l1, 0.93f, 0.95f, 1, 1);
        text(&small, rx - text_w(&small, l2), 46 * S, l2, soft[0], soft[1], soft[2], 1);
        if (l3[0]) text(&small, x, 70 * S, l3, soft[0], soft[1], soft[2], 0.9f); // the CPU, under the frame rate
        // the line: the last frames' times, 16.7 ms on the guide, taller is slower
        float gx = 18 * S, gy = 80 * S, gw = iw - 36 * S, gh = 36 * S;
        for (int i = (int)gx; i < (int)(gx + gw); i++) blend(i, (int)(gy + gh * 0.5f), 1, 1, 1, 0.10f); // 16.7 ms
        float prev = -1;
        for (int k = 0; k < HIST; k++) {
            float ms = hist[(hist_at + k) % HIST];
            if (ms <= 0) { // no frame there yet
                prev = -1;
                continue;
            }
            float v = gy + gh - fminf(ms / 33.4f, 1) * gh; // 33 ms at the top
            float xx = gx + gw * k / (HIST - 1);
            if (prev >= 0) {
                float x0 = gx + gw * (k - 1) / (HIST - 1);
                for (float t = 0; t <= 1; t += 0.5f / (xx - x0 + 1)) {
                    float lx = x0 + (xx - x0) * t, ly = prev + (v - prev) * t;
                    float c[3];
                    fps_rgb(ms > 0 ? (int)(1000 / ms) : 60, c);
                    for (int dy = -1; dy <= 1; dy++)
                        blend((int)lx, (int)(ly + dy * S * 0.7f), c[0], c[1], c[2], dy ? 0.45f : 1);
                }
            }
            prev = v;
        }
    }
    to_bytes(premul);
    return 1;
}

// out: bytes, premultiplied, or solid on the panel's colour with the corners cut
static void to_bytes(int premul) {
    const float bg[3] = { 0.035f, 0.12f, 0.085f };
    for (int i = 0; i < iw * ih; i++) {
        const float *p = px + i * 4;
        uint8_t *o = out + i * 4;
        if (premul) {
            for (int c = 0; c < 4; c++) o[c] = (uint8_t)(fminf(p[c], 1) * 255 + 0.5f);
        } else {
            float a = p[3];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(fminf(p[c] + bg[c] * (1 - a), 1) * 255 + 0.5f);
            o[3] = a > 0.5f ? 255 : 0;
        }
    }
}

static void *stats_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec nap = { 0, 66 * 1000 * 1000 }; // 15 times a second
        nanosleep(&nap, NULL);
        int mode = want_mode, premul = want_premul;
        float scale = want_scale;
        if (!draw(mode, scale, premul)) continue;
        pthread_mutex_lock(&pub_lock);
        memcpy(shared_img, out, (size_t)iw * ih * 4);
        shared_w = iw, shared_h = ih, shared_mode = mode, shared_premul = premul, shared_scale = scale;
        shared_ver++;
        pthread_mutex_unlock(&pub_lock);
    }
    return NULL;
}

const uint8_t *overlay_stats_image(int mode, float scale, int premul, int *w, int *h, unsigned *version) {
    static int started;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    want_mode = mode, want_premul = premul, want_scale = scale;
    if (mode == STATS_OFF) return NULL;
    if (!started) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256 << 10);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&t, &a, stats_thread, NULL) == 0 ? 1 : -1;
        pthread_attr_destroy(&a);
    }
    // the newest picture, if it's of what's asked (a mode or size change shows from the next one)
    pthread_mutex_lock(&pub_lock);
    if (shared_ver != read_ver && shared_mode == mode && shared_premul == premul && shared_scale == scale) {
        memcpy(read_img, shared_img, (size_t)shared_w * shared_h * 4);
        read_w = shared_w, read_h = shared_h, read_ver = shared_ver;
    }
    pthread_mutex_unlock(&pub_lock);
    if (!read_ver) return NULL;
    *w = read_w, *h = read_h, *version = read_ver;
    return read_img;
}
