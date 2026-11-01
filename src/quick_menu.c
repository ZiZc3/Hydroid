// quick_menu.c — the in-game Quick menu (quick_menu.h). In a game app it opens only while
// /app0/keyedit.txt is there (making the game's keymap). Hold Options: the game pauses at its
// next swap, its frame frozen behind frosted glass, a dark green glass card fading up over it:
// the game's icon and name, then Resume, Stats overlay (Off / FPS / Full), Keymap Editor and
// Close Game.
//
// All of it drawn on the CPU (the card with Outfit, as the stats panel): the swaps of GL,
// ANGLE and Vulkan only put two pictures on the screen. The card's parts that don't move (its
// glass; its text, redrawn when a value changes) are kept, the gliding highlight put between
// them each frame.

#include "quick_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "android_shim.h"
#include "game_settings.h"
#include "overlay_stats.h"
#include "ui_sound.h"
#include "ui_icons.h"
#include "keymap.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

void droid_menu_pad(uint32_t *buttons, uint8_t *lx, uint8_t *ly); // app_main.c: the DualSense, read now
void droid_pad_rumble(float strength, int ms);                     // app_main.c
int droid_import_missing(const char *name);                        // app_main.c
int sceKernelUsleep(uint32_t us);

volatile int droid_game_paused;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float smoother(float v) { // 0..1, eased both ends
    v = clamp01(v);
    return v * v * v * (v * (v * 6 - 15) + 10);
}
static float ease_to(float v, float to, float dt, float rate) { return to + (v - to) * expf(-rate * dt); }

// ---- the game, and the requests ------------------------------------------------------------
static char game_name[128] = "Game", game_id[128];
static uint8_t *game_icon;
static int game_icon_size;
static volatile int requested, resumed;
static volatile double requested_at;

void quick_menu_set_game(const char *name, const char *id, const uint8_t *icon, int size) {
    snprintf(game_name, sizeof(game_name), "%s", name && name[0] ? name : "Game");
    snprintf(game_id, sizeof(game_id), "%s", id ? id : "");
    free(game_icon);
    game_icon = NULL, game_icon_size = 0;
    if (icon && size > 0 && (game_icon = malloc((size_t)size * size * 4))) {
        memcpy(game_icon, icon, (size_t)size * size * 4);
        game_icon_size = size;
    }
}

void quick_menu_request(void) {
    requested_at = now_s();
    requested = 1;
}

int quick_menu_wanted(void) {
    extern int droid_single_game; // app_main.c: a one-game app has no Quick menu,
    static int keyedit = -1;      // unless /app0/keyedit.txt is there (making its keymap; then removed)
    if (keyedit < 0) {
        struct stat st;
        keyedit = droid_single_game && stat(APP0 "/keyedit.txt", &st) == 0;
        if (keyedit) droid_log("quick menu: on in this game app (keyedit.txt: the Keymap Editor)");
    }
    if (droid_single_game && !keyedit) return 0;
    static double test_at = -1; // PC tests: DROID_QM_TEST=<s> opens it that long after the first frame
    if (test_at < 0) {
        const char *e = getenv("DROID_QM_TEST");
        test_at = e ? now_s() + atof(e) : 0;
    }
    if (test_at > 0 && now_s() >= test_at) {
        test_at = 0;
        return 1;
    }
    if (!requested) return 0;
    if (now_s() - requested_at > 1.0) { // no swap for a second (a film, a load): not later out of nowhere
        requested = 0;
        return 0;
    }
    return 1;
}

int quick_menu_resumed(void) {
    if (!resumed) return 0;
    resumed = 0;
    return 1;
}

// ---- the font: Outfit, three sizes, baked at the screen's scale -----------------------------
typedef struct {
    stbtt_bakedchar ch[96];
    unsigned char *atlas;
    int aw, ah;
} face;
static face f_title, f_body, f_small;
static float faces_scale;

static unsigned char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static int bake(face *f, const unsigned char *ttf, float px) {
    free(f->atlas);
    f->atlas = NULL;
    for (int aw = 512, ah = 256; ah <= 2048; ah *= 2) { // as big an atlas as the size needs
        if (ah > aw * 2) aw *= 2;
        f->atlas = calloc((size_t)aw * ah, 1);
        if (!f->atlas) return 0;
        f->aw = aw, f->ah = ah;
        if (stbtt_BakeFontBitmap(ttf, 0, px, f->atlas, aw, ah, 32, 96, f->ch) > 0) return 1;
        free(f->atlas);
        f->atlas = NULL;
    }
    return 0;
}

static int faces_ready(float s) {
    static unsigned char *semi, *bold;
    if (!semi) semi = read_file(APP0 "/assets/fonts/Outfit-SemiBold.ttf");
    if (!bold) bold = read_file(APP0 "/assets/fonts/Outfit-Bold.ttf");
    if (!semi || !bold) {
        droid_log("quick menu: no Outfit in assets/fonts");
        return 0;
    }
    if (faces_scale != s) {
        if (!bake(&f_title, bold, 38 * s) || !bake(&f_body, semi, 30 * s) || !bake(&f_small, semi, 22 * s)) return 0;
        faces_scale = s;
    }
    return 1;
}

// ---- drawing into premultiplied RGBA layers -------------------------------------------------
typedef struct {
    uint8_t *p;
    int w, h;
} layer;
typedef struct {
    float r, g, b;
} rgb;
#define RGB(r, g, b) ((rgb){ (r) / 255.0f, (g) / 255.0f, (b) / 255.0f })
static rgb rgb_mix(rgb a, rgb b, float t) { return (rgb){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }

static inline void put(layer *L, int x, int y, rgb c, float a) { // "over"
    if ((unsigned)x >= (unsigned)L->w || (unsigned)y >= (unsigned)L->h || a <= 0.002f) return;
    if (a > 1) a = 1;
    uint8_t *d = L->p + ((size_t)y * L->w + x) * 4;
    float k = 1 - a;
    d[0] = (uint8_t)(c.r * a * 255 + d[0] * k + 0.5f);
    d[1] = (uint8_t)(c.g * a * 255 + d[1] * k + 0.5f);
    d[2] = (uint8_t)(c.b * a * 255 + d[2] * k + 0.5f);
    d[3] = (uint8_t)(a * 255 + d[3] * k + 0.5f);
}

static float sd_rrect(float px, float py, float x, float y, float w, float h, float r) {
    float qx = fabsf(px - (x + w / 2)) - w / 2 + r, qy = fabsf(py - (y + h / 2)) - h / 2 + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

// a rounded box, its colour going from top to bottom; soft: its edge's width (1: sharp, more: a shadow)
static void rrect(layer *L, float x, float y, float w, float h, float r, float soft, rgb top, rgb bottom, float a) {
    int x0 = (int)floorf(x - soft), x1 = (int)ceilf(x + w + soft), y0 = (int)floorf(y - soft), y1 = (int)ceilf(y + h + soft);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > L->w) x1 = L->w;
    if (y1 > L->h) y1 = L->h;
    for (int j = y0; j < y1; j++) {
        rgb c = rgb_mix(top, bottom, clamp01((j + 0.5f - y) / h));
        for (int i = x0; i < x1; i++) {
            float d = sd_rrect(i + 0.5f, j + 0.5f, x, y, w, h, r);
            float cov = clamp01(0.5f - d / soft);
            if (cov > 0) put(L, i, j, c, a * cov);
        }
    }
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6f));
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}

static void line(layer *L, float ax, float ay, float bx, float by, float width, rgb c, float a) {
    int x0 = (int)floorf(fminf(ax, bx) - width), x1 = (int)ceilf(fmaxf(ax, bx) + width);
    int y0 = (int)floorf(fminf(ay, by) - width), y1 = (int)ceilf(fmaxf(ay, by) + width);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            float d = seg_dist(i + 0.5f, j + 0.5f, ax, ay, bx, by) - width / 2;
            float cov = clamp01(0.5f - d);
            if (cov > 0) put(L, i, j, c, a * cov);
        }
}

static void ring(layer *L, float cx, float cy, float rad, float width, rgb c, float a) {
    for (int j = (int)(cy - rad - width); j < (int)(cy + rad + width + 1); j++)
        for (int i = (int)(cx - rad - width); i < (int)(cx + rad + width + 1); i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - rad) - width / 2;
            float cov = clamp01(0.5f - d);
            if (cov > 0) put(L, i, j, c, a * cov);
        }
}

// a filled triangle, 4x4 samples a pixel
static int in_tri(float px, float py, const float *t) {
    float d1 = (px - t[2]) * (t[1] - t[3]) - (t[0] - t[2]) * (py - t[3]);
    float d2 = (px - t[4]) * (t[3] - t[5]) - (t[2] - t[4]) * (py - t[5]);
    float d3 = (px - t[0]) * (t[5] - t[1]) - (t[4] - t[0]) * (py - t[1]);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}
static void tri(layer *L, const float *t, rgb c, float a) {
    float x0 = fminf(t[0], fminf(t[2], t[4])), x1 = fmaxf(t[0], fmaxf(t[2], t[4]));
    float y0 = fminf(t[1], fminf(t[3], t[5])), y1 = fmaxf(t[1], fmaxf(t[3], t[5]));
    for (int j = (int)y0; j <= (int)y1; j++)
        for (int i = (int)x0; i <= (int)x1; i++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) n += in_tri(i + (sx + 0.5f) / 4, j + (sy + 0.5f) / 4, t);
            if (n) put(L, i, j, c, a * n / 16.0f);
        }
}

static float text_w(const face *f, const char *s) {
    float w = 0;
    for (; *s; s++)
        if ((unsigned char)*s >= 32 && (unsigned char)*s < 128) w += f->ch[*s - 32].xadvance;
    return w;
}

static void text(layer *L, const face *f, float x, float baseline, const char *s, rgb c, float a) {
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s >= 128) continue;
        const stbtt_bakedchar *g = &f->ch[*s - 32];
        int gx = (int)floorf(x + g->xoff + 0.5f), gy = (int)floorf(baseline + g->yoff + 0.5f);
        for (int j = g->y0; j < g->y1; j++)
            for (int i = g->x0; i < g->x1; i++) {
                float cov = f->atlas[j * f->aw + i] / 255.0f;
                if (cov > 0) put(L, gx + (i - g->x0), gy + (j - g->y0), c, a * cov);
            }
        x += g->xadvance;
    }
}

// the text, cut with "..." to fit max
static void text_fit(layer *L, const face *f, float x, float baseline, float max, const char *s, rgb c, float a) {
    if (text_w(f, s) <= max) {
        text(L, f, x, baseline, s, c, a);
        return;
    }
    char buf[160];
    size_t n = strlen(s) < sizeof(buf) - 4 ? strlen(s) : sizeof(buf) - 4;
    while (n > 0) {
        memcpy(buf, s, n);
        while (n > 0 && buf[n - 1] == ' ') n--;
        strcpy(buf + n, "...");
        if (text_w(f, buf) <= max) break;
        n--;
    }
    text(L, f, x, baseline, buf, c, a);
}

// the game's icon (straight-alpha RGBA, size x size) into a rounded square, bilinear
static void icon(layer *L, float x, float y, float s, float r) {
    if (!game_icon) return;
    const int n = game_icon_size;
    for (int j = (int)y; j < (int)(y + s + 1); j++)
        for (int i = (int)x; i < (int)(x + s + 1); i++) {
            float cov = clamp01(0.5f - sd_rrect(i + 0.5f, j + 0.5f, x, y, s, s, r));
            if (cov <= 0) continue;
            float u = (i + 0.5f - x) / s * n - 0.5f, v = (j + 0.5f - y) / s * n - 0.5f;
            int u0 = (int)floorf(u), v0 = (int)floorf(v);
            float fu = u - u0, fv = v - v0, acc[4] = { 0 };
            for (int k = 0; k < 4; k++) {
                int uu = u0 + (k & 1), vv = v0 + (k >> 1);
                uu = uu < 0 ? 0 : uu >= n ? n - 1 : uu;
                vv = vv < 0 ? 0 : vv >= n ? n - 1 : vv;
                float w = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                const uint8_t *p = game_icon + ((size_t)vv * n + uu) * 4;
                float pa = p[3] / 255.0f;
                for (int q = 0; q < 3; q++) acc[q] += p[q] / 255.0f * pa * w; // premultiplied for the filter
                acc[3] += pa * w;
            }
            if (acc[3] <= 0) continue;
            rgb c = { acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3] };
            put(L, i, j, c, acc[3] * cov);
        }
}

// ---- the menu --------------------------------------------------------------------------------
enum { IT_RESUME, IT_STATS, IT_CONTROLS, IT_CLOSE, IT_COUNT };
static const char *const item_names[IT_COUNT] = { "Resume", "Stats overlay", "Keymap Editor", "Close Game" };
static const char *const stats_names[3] = { "Off", "FPS", "Full" };

// the card's size in the 1080p screen's pixels; the pictures have room around it for its shadow
#define CW 640
#define CH 586
#define MARGIN 64
#define ITEM_Y 184
#define ITEM_STEP 80
#define PB_UP 0x10
#define PB_RIGHT 0x20
#define PB_DOWN 0x40
#define PB_LEFT 0x80
#define PB_OPTIONS 0x8
#define PB_TRIANGLE 0x1000
#define PB_CIRCLE 0x2000
#define PB_CROSS 0x4000

// the colours: the theme (settings.txt theme: the default green, or dark grey), set at the open
typedef struct {
    rgb text, dimtext, soft, green, surface, line, pill, pill_on, chip, chip_on, disc, sym;
    float line_a, disc_a, shadow_a;
    int tint_r, tint_g, tint_b;   // the frost behind the card: the colour it leans to,
    float tint_k, dim_k;          // how much, and how much darker
} palette;
static const palette palettes[2] = {
    { { 236 / 255.0f, 244 / 255.0f, 240 / 255.0f }, { 196 / 255.0f, 214 / 255.0f, 205 / 255.0f }, { 160 / 255.0f, 200 / 255.0f, 182 / 255.0f },
      { 61 / 255.0f, 220 / 255.0f, 132 / 255.0f }, { 11 / 255.0f, 33 / 255.0f, 24 / 255.0f }, { 1, 1, 1 },
      { 36 / 255.0f, 98 / 255.0f, 69 / 255.0f }, { 52 / 255.0f, 130 / 255.0f, 92 / 255.0f }, { 25 / 255.0f, 62 / 255.0f, 45 / 255.0f },
      { 46 / 255.0f, 122 / 255.0f, 86 / 255.0f }, { 1, 1, 1 }, { 230 / 255.0f, 240 / 255.0f, 235 / 255.0f }, 0.07f, 0.12f, 0.45f,
      4, 22, 15, 0.22f, 0.36f },
    { { 238 / 255.0f, 238 / 255.0f, 238 / 255.0f }, { 200 / 255.0f, 200 / 255.0f, 200 / 255.0f }, { 170 / 255.0f, 170 / 255.0f, 170 / 255.0f },
      { 61 / 255.0f, 220 / 255.0f, 132 / 255.0f }, { 30 / 255.0f, 30 / 255.0f, 30 / 255.0f }, { 1, 1, 1 },
      { 72 / 255.0f, 72 / 255.0f, 72 / 255.0f }, { 96 / 255.0f, 96 / 255.0f, 96 / 255.0f }, { 50 / 255.0f, 50 / 255.0f, 50 / 255.0f },
      { 84 / 255.0f, 84 / 255.0f, 84 / 255.0f }, { 1, 1, 1 }, { 236 / 255.0f, 236 / 255.0f, 236 / 255.0f }, 0.07f, 0.12f, 0.45f,
      10, 10, 10, 0.22f, 0.40f },
};
static const palette *P = &palettes[0];
#define C_TEXT (P->text)
#define C_DIMTEXT (P->dimtext)
#define C_SOFT (P->soft)
#define C_GREEN (P->green)

enum { ST_OPENING, ST_CLOSING, ST_LEAVING };
static struct {
    int on, state, sel, action;
    float s;              // screen pixels per 1080p pixel
    int sw, sh;           // the screen
    double last, next_at; // the frames' clock
    float o, blur, black; // open 0..1; the backdrop's frost; the black at the end (leaving)
    float sel_anim, kick, press;
    uint32_t held;
    double repeat_at, opened_at;
    // the backdrop: the frozen frame (sharp), its frosted copy, and this frame's mix
    uint8_t *sharp, *frost, *back;
    int bw, bh;
    unsigned back_ver;
    float back_blur, back_black;
    // the card: its glass (kept), its text (redrawn on a change), and both with the highlight
    layer glass, words, card;
    unsigned card_ver;
    float drawn_sel, drawn_kick, drawn_press;
    int drawn_stats, drawn_selected;
    // the keymap editor (Edit controls): its mode, the switch to and from it, what's being placed
    int mode;           // 0 the menu, 1 the editor
    int switching;      // 1 to the editor, 2 back to the menu (the card fades out, the other fades in)
    float ek;           // the editor's backdrop, 0..1
    keymap_t km;
    float ex, ey;       // its crosshair (1920x1080)
    int hold, tray;     // what's held (a button's index, 100 the joystick, -1), the tray's choice
    float tray_anim, drawn_tray;
    float tray_dx, tray_dy; // where the touchpad moved the tray (1080p pixels from its place at the bottom)
    uint8_t *edit_base; // the frame clear, a touch darker
    int test_step; // PC tests: DROID_QM_KEYS
    int frames;
    float slowest_gap, slowest_work; // (the log: is it holding 60?)
} m;

static void fx(int sound, float gain, float buzz, int ms) {
    if (app_settings.ui_sounds && sound >= 0) ui_sound_play(sound, gain);
    droid_pad_rumble(buzz, ms);
}

// the frozen frame: to at most 1080 rows (4K: 2x2 averaged), and a frosted copy (an eighth of the
// size blurred three times, stretched back smoothly)
static int backdrop_make(const uint8_t *px, int w, int h, size_t stride, int bgra, int bottom_up) {
    if (phone_view() && h > w && m.sw > m.sh) {
        // the phone view's frame (the game's 1080x1920): first put in the middle of a TV-shaped
        // picture, black at the sides, as the game is shown (not stretched over the screen)
        const int ch = h > 1080 ? 1080 : h, cw = ch * m.sw / m.sh;
        int rx, ry, rw, rh;
        view_rect(cw, ch, &rx, &ry, &rw, &rh);
        uint8_t *tv = calloc((size_t)cw * ch, 4);
        if (!tv) return 0;
        const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
        for (int y = 0; y < rh; y++) {
            const int sy0 = y * h / rh, sy1 = (y + 1) * h / rh > sy0 ? (y + 1) * h / rh : sy0 + 1;
            for (int x = 0; x < rw; x++) {
                const int sx0 = x * w / rw, sx1 = (x + 1) * w / rw > sx0 ? (x + 1) * w / rw : sx0 + 1;
                int acc[3] = { 0 }, n = 0;
                for (int sy = sy0; sy < sy1 && sy < h; sy++) {
                    const uint8_t *row = px + (size_t)(bottom_up ? h - 1 - sy : sy) * stride;
                    for (int sx = sx0; sx < sx1 && sx < w; sx++, n++) {
                        const uint8_t *p = row + (size_t)sx * 4;
                        acc[0] += p[ri], acc[1] += p[1], acc[2] += p[bi];
                    }
                }
                uint8_t *d = tv + ((size_t)(ry + y) * cw + rx + x) * 4;
                for (int c = 0; c < 3; c++) d[c] = (uint8_t)(n ? acc[c] / n : 0);
            }
        }
        for (size_t i = 0; i < (size_t)cw * ch; i++) tv[i * 4 + 3] = 255;
        const int ok = backdrop_make(tv, cw, ch, (size_t)cw * 4, 0, 0); // (wide now: made as any other)
        free(tv);
        return ok;
    }
    int f = h > 1080 ? (h + 1079) / 1080 : 1;
    m.bw = w / f, m.bh = h / f;
    size_t n = (size_t)m.bw * m.bh * 4;
    m.sharp = malloc(n), m.frost = malloc(n), m.back = malloc(n);
    if (!m.sharp || !m.frost || !m.back) return 0;
    const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (int y = 0; y < m.bh; y++)
        for (int x = 0; x < m.bw; x++) {
            int acc[3] = { 0 };
            for (int dy = 0; dy < f; dy++) {
                int sy = y * f + dy;
                const uint8_t *row = px + (size_t)(bottom_up ? h - 1 - sy : sy) * stride;
                for (int dx = 0; dx < f; dx++) {
                    const uint8_t *p = row + (size_t)(x * f + dx) * 4;
                    acc[0] += p[ri], acc[1] += p[1], acc[2] += p[bi];
                }
            }
            uint8_t *d = m.sharp + ((size_t)y * m.bw + x) * 4;
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)(acc[c] / (f * f));
            d[3] = 255;
        }
    // the frost: an eighth, three box blurs (about a gaussian), back up bilinear
    const int sw = (m.bw + 7) / 8, sh = (m.bh + 7) / 8;
    float *a = malloc(sizeof(float) * 3 * sw * sh), *b = malloc(sizeof(float) * 3 * sw * sh);
    if (!a || !b) {
        free(a), free(b);
        return 0;
    }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            float acc[3] = { 0 };
            int cnt = 0;
            for (int dy = 0; dy < 8; dy++)
                for (int dx = 0; dx < 8; dx++) {
                    int sx = x * 8 + dx, sy = y * 8 + dy;
                    if (sx >= m.bw || sy >= m.bh) continue;
                    const uint8_t *p = m.sharp + ((size_t)sy * m.bw + sx) * 4;
                    acc[0] += p[0], acc[1] += p[1], acc[2] += p[2];
                    cnt++;
                }
            for (int c = 0; c < 3; c++) a[(y * sw + x) * 3 + c] = acc[c] / (cnt ? cnt : 1);
        }
    for (int pass = 0; pass < 3; pass++) {
        const int R = 2;
        for (int y = 0; y < sh; y++) // across
            for (int x = 0; x < sw; x++)
                for (int c = 0; c < 3; c++) {
                    float s = 0;
                    for (int k = -R; k <= R; k++) {
                        int xx = x + k < 0 ? 0 : x + k >= sw ? sw - 1 : x + k;
                        s += a[(y * sw + xx) * 3 + c];
                    }
                    b[(y * sw + x) * 3 + c] = s / (2 * R + 1);
                }
        for (int y = 0; y < sh; y++) // down
            for (int x = 0; x < sw; x++)
                for (int c = 0; c < 3; c++) {
                    float s = 0;
                    for (int k = -R; k <= R; k++) {
                        int yy = y + k < 0 ? 0 : y + k >= sh ? sh - 1 : y + k;
                        s += b[(yy * sw + x) * 3 + c];
                    }
                    a[(y * sw + x) * 3 + c] = s / (2 * R + 1);
                }
    }
    for (int y = 0; y < m.bh; y++) {
        float v = (y + 0.5f) / 8 - 0.5f;
        int v0 = (int)floorf(v);
        float fv = v - v0;
        int va = v0 < 0 ? 0 : v0 >= sh ? sh - 1 : v0, vb = v0 + 1 >= sh ? sh - 1 : v0 + 1 < 0 ? 0 : v0 + 1;
        for (int x = 0; x < m.bw; x++) {
            float u = (x + 0.5f) / 8 - 0.5f;
            int u0 = (int)floorf(u);
            float fu = u - u0;
            int ua = u0 < 0 ? 0 : u0 >= sw ? sw - 1 : u0, ub = u0 + 1 >= sw ? sw - 1 : u0 + 1 < 0 ? 0 : u0 + 1;
            uint8_t *d = m.frost + ((size_t)y * m.bw + x) * 4;
            for (int c = 0; c < 3; c++) {
                float top = a[(va * sw + ua) * 3 + c] * (1 - fu) + a[(va * sw + ub) * 3 + c] * fu;
                float bot = a[(vb * sw + ua) * 3 + c] * (1 - fu) + a[(vb * sw + ub) * 3 + c] * fu;
                float val = top * (1 - fv) + bot * fv;
                d[c] = (uint8_t)(val > 255 ? 255 : val + 0.5f);
            }
            d[3] = 255;
        }
    }
    free(a), free(b);
    return 1;
}

// this frame's backdrop: the frame going to frost as the menu opens (dimmed, a touch of green),
// then to black when the game is left
static void backdrop_update(void) {
    if (m.back_ver && m.blur == m.back_blur && m.black == m.back_black) return;
    m.back_blur = m.blur, m.back_black = m.black;
    const float k = smoother(m.blur);
    const int fk = (int)(k * 256), dim = (int)((1 - P->dim_k * k) * (1 - smoother(m.black)) * 256);
    const int tint = (int)(P->tint_k * k * 256), tr = P->tint_r, tg = P->tint_g, tb = P->tint_b; // the theme's green
    const size_t n = (size_t)m.bw * m.bh;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = m.sharp + i * 4, *f = m.frost + i * 4;
        uint8_t *d = m.back + i * 4;
        int r = s[0] + (((f[0] - s[0]) * fk) >> 8), g = s[1] + (((f[1] - s[1]) * fk) >> 8), b = s[2] + (((f[2] - s[2]) * fk) >> 8);
        r += ((tr - r) * tint) >> 8, g += ((tg - g) * tint) >> 8, b += ((tb - b) * tint) >> 8;
        d[0] = (uint8_t)((r * dim) >> 8), d[1] = (uint8_t)((g * dim) >> 8), d[2] = (uint8_t)((b * dim) >> 8);
        d[3] = 255;
    }
    m.back_ver++;
}

static int layer_new(layer *L, int w, int h) {
    L->w = w, L->h = h;
    L->p = calloc((size_t)w * h, 4);
    return L->p != NULL;
}

// a face button, as in the hints: a faint disc, its symbol
static void face_button(layer *L, float cx, float cy, int which, float S) {
    rrect(L, cx - 19 * S, cy - 19 * S, 38 * S, 38 * S, 19 * S, 1.5f, P->disc, P->disc, P->disc_a);
    rgb c = P->sym;
    if (which == PB_CROSS) {
        line(L, cx - 8 * S, cy - 8 * S, cx + 8 * S, cy + 8 * S, 3.5f * S, c, 1);
        line(L, cx - 8 * S, cy + 8 * S, cx + 8 * S, cy - 8 * S, 3.5f * S, c, 1);
    } else {
        ring(L, cx, cy, 9 * S, 3.2f * S, c, 1);
    }
}

// the item's icon (Hugeicons), centred at (cx, cy): the 128 px mask, each pixel the
// cell's area under it averaged (a sharp line at any size)
static void item_glyph(layer *L, int item, float cx, float cy, float S, rgb c, float a) {
    static const int ids[IT_COUNT] = { UI_ICON_PLAY, UI_ICON_CHART, UI_ICON_GAMEPAD, UI_ICON_HOME };
    const int id = ids[item], cell = UI_ICONS_B_CELL;
    const uint8_t *m0 = ui_icons_big + (size_t)(id / UI_ICON_COLS) * cell * UI_ICONS_B_W + (size_t)(id % UI_ICON_COLS) * cell;
    const int size = (int)(36 * S + 0.5f), x0 = (int)(cx - size / 2.0f), y0 = (int)(cy - size / 2.0f);
    const float f = (float)cell / size;
    for (int j = 0; j < size; j++) {
        int sy0 = (int)(j * f), sy1 = (int)((j + 1) * f);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int i = 0; i < size; i++) {
            int sx0 = (int)(i * f), sx1 = (int)((i + 1) * f);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int sum = 0, n = 0;
            for (int y = sy0; y < sy1 && y < cell; y++)
                for (int x = sx0; x < sx1 && x < cell; x++) sum += m0[(size_t)y * UI_ICONS_B_W + x], n++;
            if (sum) put(L, x0 + i, y0 + j, c, a * sum / (255.0f * n));
        }
    }
}

// the card: drawn once. Flat, as Android's own menus: a solid dark green surface, a soft shadow
static void draw_glass(void) {
    layer *L = &m.glass;
    const float S = m.s, x = MARGIN * S, y = MARGIN * S, w = CW * S, h = CH * S;
    rrect(L, x - 6 * S, y + 18 * S, w + 12 * S, h, 36 * S, 48 * S, RGB(0, 0, 0), RGB(0, 0, 0), P->shadow_a); // its shadow
    rrect(L, x, y, w, h, 32 * S, 1, P->surface, P->surface, 0.98f); // the surface
    rrect(L, x + 40 * S, y + 156 * S, w - 80 * S, 2 * S, 1 * S, 1, P->line, P->line, P->line_a); // under the header
    rrect(L, x + 40 * S, y + (CH - 82) * S, w - 80 * S, 2 * S, 1 * S, 1, P->line, P->line, P->line_a * 0.8f); // over the hints
}

// the text and the pictures: the header, the items (the chosen one lit), the stats value, the hints
static void draw_words(void) {
    layer *L = &m.words;
    memset(L->p, 0, (size_t)L->w * L->h * 4);
    const float S = m.s, x = MARGIN * S, y = MARGIN * S;
    // the header: the icon, the name, "Paused"
    const float is = 88 * S, ix = x + 40 * S, iy = y + 34 * S;
    if (game_icon) {
        icon(L, ix, iy, is, 22 * S);
    } else {
        rrect(L, ix, iy, is, is, 22 * S, 1, RGB(40, 150, 100), RGB(18, 80, 56), 1);
    }
    text_fit(L, &f_title, ix + is + 26 * S, iy + 44 * S, (CW - 40 - 88 - 26 - 40) * S, game_name, C_TEXT, 1);
    text(L, &f_small, ix + is + 28 * S, iy + 80 * S, "Paused", C_GREEN, 1);
    // the items
    for (int k = 0; k < IT_COUNT; k++) {
        float lit = 1 - fminf(fabsf(m.sel_anim - k), 1);
        float cy = y + (ITEM_Y + k * ITEM_STEP + 34) * S;
        rgb c = rgb_mix(C_DIMTEXT, C_TEXT, lit);
        item_glyph(L, k, x + 76 * S, cy, S, rgb_mix(C_SOFT, C_TEXT, lit), 1);
        text(L, &f_body, x + 112 * S, cy + 10 * S, item_names[k], c, 1);
        if (k == IT_STATS) { // the value in a pill on the right, < > around it; a change slides it in
            int v = app_settings.stats < 0 ? 0 : app_settings.stats > 2 ? 2 : app_settings.stats;
            const float vw = 184 * S, vh = 50 * S, vx = x + (CW - 44) * S - vw, vy = cy - vh / 2;
            rgb chip = rgb_mix(P->chip, P->chip_on, lit); // a solid chip (lighter on the filled row)
            rrect(L, vx, vy, vw, vh, 25 * S, 1, chip, chip, 1);
            const char *s = stats_names[v];
            float kick = m.kick, tw = text_w(&f_small, s);
            text(L, &f_small, vx + vw / 2 - tw / 2 + kick * 30 * S, cy + 8 * S, s, C_TEXT, 1 - fabsf(kick));
            rgb ac = rgb_mix(C_SOFT, C_TEXT, lit);
            float aa = 0.5f + 0.5f * lit, cs = 6 * S;
            for (int side = -1; side <= 1; side += 2) {
                float chx = side < 0 ? vx + 28 * S : vx + vw - 28 * S;
                line(L, chx + side * -cs / 2, cy - cs, chx + side * cs / 2, cy, 3 * S, ac, aa);
                line(L, chx + side * cs / 2, cy, chx + side * -cs / 2, cy + cs, 3 * S, ac, aa);
            }
        }
    }
    // the hints, in the middle at the bottom
    const char *l1 = "Select", *l2 = "Resume";
    float w1 = 46 * S + text_w(&f_small, l1), w2 = 46 * S + text_w(&f_small, l2), gap = 40 * S;
    float hx = x + (CW * S - (w1 + gap + w2)) / 2, hy = y + (CH - 42) * S;
    face_button(L, hx + 19 * S, hy, PB_CROSS, S);
    text(L, &f_small, hx + 46 * S, hy + 8 * S, l1, C_TEXT, 1);
    hx += w1 + gap;
    face_button(L, hx + 19 * S, hy, PB_CIRCLE, S);
    text(L, &f_small, hx + 46 * S, hy + 8 * S, l2, C_TEXT, 1);
}

// this frame's card: the glass, the highlight gliding between the items, the words over them
static void card_update(void) {
    int stats = app_settings.stats;
    int words_stale = m.sel_anim != m.drawn_sel || m.kick != m.drawn_kick || stats != m.drawn_stats || !m.card_ver;
    if (!words_stale && m.press == m.drawn_press && m.sel == m.drawn_selected) return;
    if (words_stale) draw_words();
    m.drawn_sel = m.sel_anim, m.drawn_kick = m.kick, m.drawn_stats = stats, m.drawn_press = m.press, m.drawn_selected = m.sel;
    layer *C = &m.card;
    memcpy(C->p, m.glass.p, (size_t)C->w * C->h * 4);
    const float S = m.s, x = MARGIN * S, y = MARGIN * S;
    const float hy = y + (ITEM_Y + m.sel_anim * ITEM_STEP) * S, ph = (ITEM_STEP - 12) * S;
    rgb pill = rgb_mix(P->pill, P->pill_on, m.press); // a filled pill (a press lights it)
    rrect(C, x + 24 * S, hy, (CW - 48) * S, ph, ph / 2, 1.5f, pill, pill, 1);
    // the words over it ("over", premultiplied)
    const size_t n = (size_t)C->w * C->h;
    const uint8_t *w = m.words.p;
    uint8_t *d = C->p;
    for (size_t i = 0; i < n; i++, w += 4, d += 4) {
        if (!w[3]) continue;
        int k = 255 - w[3];
        d[0] = (uint8_t)(w[0] + (d[0] * k + 127) / 255);
        d[1] = (uint8_t)(w[1] + (d[1] * k + 127) / 255);
        d[2] = (uint8_t)(w[2] + (d[2] * k + 127) / 255);
        d[3] = (uint8_t)(w[3] + (d[3] * k + 127) / 255);
    }
    m.card_ver++;
}

// ---- Keymap Editor, the keymap editor (Quick menu > Keymap Editor): the game's frame, clear and a touch darker,
// the keymap's buttons on it as DualSense buttons, the joystick as its circle; a tray at the
// bottom to take them from (✕ ○ □ △ L1 R1 L2 R2, the D-pad, the left and right sticks' joysticks;
// the touchpad drags it out of the way). The stick or the D-pad moves the crosshair, ✕ puts the
// tray's button there (or picks a placed one up, and puts it down), □ takes one away, L1 / R1 the
// tray, L2 / R2 a joystick's size, △ saves (touchmap.txt: every host plays it), ○ leaves it as it was.
#define TW 1420
#define TH 190
#define KM_STICK_KEY -1  // the left stick's joystick (the tray's)
#define KM_STICK2_KEY -2 // the right stick's
#define PB_L1 0x400
#define PB_R1 0x800
#define PB_L2 0x100
#define PB_R2 0x200
#define PB_SQUARE 0x8000
static const int tray_keys[] = { 96, 97, 99, 100, 102, 103, KEY_L2, KEY_R2, 19, 20, 21, 22, KM_STICK_KEY, KM_STICK2_KEY };
#define TRAY_N ((int)(sizeof(tray_keys) / sizeof(tray_keys[0])))

static rgb key_rgb(int key) { // the DualSense's own colours for the face buttons
    switch (key) {
    case 96: return RGB(124, 178, 232);  // Cross
    case 97: return RGB(240, 110, 110);  // Circle
    case 99: return RGB(226, 140, 200);  // Square
    case 100: return RGB(80, 214, 170);  // Triangle
    case KM_STICK_KEY: return RGB(61, 220, 132);
    case KM_STICK2_KEY: return RGB(170, 255, 205);
    }
    return RGB(232, 236, 234);
}

// a button's picture, centred at (cx, cy), S: its scale (1: 1080p)
static void key_glyph(layer *L, int key, float cx, float cy, float S, float a, float lit) {
    const rgb col = key_rgb(key), white = RGB(240, 244, 242);
    const float r = 30 * S, w = 3.6f * S;
    rrect(L, cx - r - 3 * S, cy - r + 3 * S, 2 * r + 6 * S, 2 * r + 6 * S, r + 3 * S, 8 * S, RGB(0, 0, 0), RGB(0, 0, 0), 0.35f * a);
    rrect(L, cx - r, cy - r, 2 * r, 2 * r, r, 1.5f, RGB(14, 20, 18), RGB(14, 20, 18), (0.78f + 0.12f * lit) * a);
    ring(L, cx, cy, r - 1.5f * S, 2.6f * S + 1.5f * S * lit, col, a);
    const float k = 10 * S;
    switch (key) {
    case 96: // ✕
        line(L, cx - k, cy - k, cx + k, cy + k, w, col, a);
        line(L, cx - k, cy + k, cx + k, cy - k, w, col, a);
        break;
    case 97: ring(L, cx, cy, k + 1 * S, w, col, a); break; // ○
    case 99: // □
        line(L, cx - k, cy - k, cx + k, cy - k, w, col, a), line(L, cx - k, cy + k, cx + k, cy + k, w, col, a);
        line(L, cx - k, cy - k, cx - k, cy + k, w, col, a), line(L, cx + k, cy - k, cx + k, cy + k, w, col, a);
        break;
    case 100: // △
        line(L, cx, cy - k * 1.15f, cx - k * 1.15f, cy + k * 0.85f, w, col, a);
        line(L, cx, cy - k * 1.15f, cx + k * 1.15f, cy + k * 0.85f, w, col, a);
        line(L, cx - k * 1.15f, cy + k * 0.85f, cx + k * 1.15f, cy + k * 0.85f, w, col, a);
        break;
    case 102: case 103: case KEY_L2: case KEY_R2: { // L1 / R1 / L2 / R2: the letter and its number
        const float lx = cx - 11 * S, t = cy - 10 * S, b = cy + 10 * S, ww = 3 * S;
        line(L, lx, t, lx, b, ww, white, a);
        if (key == 102 || key == KEY_L2) {
            line(L, lx, b, lx + 9 * S, b, ww, white, a);
        } else {
            line(L, lx, t, lx + 7 * S, t, ww, white, a), line(L, lx + 7 * S, t, lx + 9 * S, t + 5 * S, ww, white, a);
            line(L, lx + 9 * S, t + 5 * S, lx + 7 * S, t + 10 * S, ww, white, a), line(L, lx, t + 10 * S, lx + 7 * S, t + 10 * S, ww, white, a);
            line(L, lx + 4 * S, t + 10 * S, lx + 10 * S, b, ww, white, a);
        }
        if (key == 102 || key == 103) { // 1
            line(L, cx + 9 * S, t, cx + 9 * S, b, ww, white, a), line(L, cx + 5 * S, t + 4 * S, cx + 9 * S, t, ww, white, a);
        } else { // 2
            const float x0 = cx + 4 * S, x1 = cx + 13 * S;
            line(L, x0, t + 2 * S, x0 + 3 * S, t, ww, white, a), line(L, x0 + 3 * S, t, x1, t + 3 * S, ww, white, a);
            line(L, x1, t + 3 * S, x1, t + 7 * S, ww, white, a), line(L, x1, t + 7 * S, x0, b, ww, white, a);
            line(L, x0, b, x1, b, ww, white, a);
        }
        break;
    }
    case 19: case 20: case 21: case 22: { // the D-pad: an arrow
        const float d = 11 * S;
        float t[6];
        if (key == 19) t[0] = cx, t[1] = cy - d, t[2] = cx - d, t[3] = cy + d * 0.6f, t[4] = cx + d, t[5] = cy + d * 0.6f;
        else if (key == 20) t[0] = cx, t[1] = cy + d, t[2] = cx - d, t[3] = cy - d * 0.6f, t[4] = cx + d, t[5] = cy - d * 0.6f;
        else if (key == 21) t[0] = cx - d, t[1] = cy, t[2] = cx + d * 0.6f, t[3] = cy - d, t[4] = cx + d * 0.6f, t[5] = cy + d;
        else t[0] = cx + d, t[1] = cy, t[2] = cx - d * 0.6f, t[3] = cy - d, t[4] = cx - d * 0.6f, t[5] = cy + d;
        tri(L, t, white, a);
        break;
    }
    default: { // a joystick: a stick in its ring, L or R beside
        ring(L, cx, cy, k * 1.2f, 2.4f * S, white, a);
        rrect(L, cx - 6 * S, cy - 6 * S, 12 * S, 12 * S, 6 * S, 1.5f, col, col, a);
        const float lx = cx + 15 * S, t = cy + 6 * S, b = cy + 18 * S, ww = 2.4f * S;
        line(L, lx, t, lx, b, ww, white, a);
        if (key == KM_STICK_KEY) line(L, lx, b, lx + 6 * S, b, ww, white, a);
        else line(L, lx, t, lx + 5 * S, t + 3 * S, ww, white, a), line(L, lx + 5 * S, t + 3 * S, lx, t + 6 * S, ww, white, a),
             line(L, lx + 2 * S, t + 6 * S, lx + 6 * S, b, ww, white, a);
        break;
    }
    }
}

static int editor_marker_at(float x, float y) { // the placed one under (x, y): a button's index, 100 the joystick, -1
    for (int i = 0; i < m.km.count; i++) {
        float dx = m.km.button[i].x - x, dy = m.km.button[i].y - y;
        if (dx * dx + dy * dy < 34 * 34) return i;
    }
    if (m.km.stick_r > 0) {
        float dx = m.km.stick_x - x, dy = m.km.stick_y - y;
        if (dx * dx + dy * dy < 44 * 44) return 100;
    }
    if (m.km.stick2_r > 0) {
        float dx = m.km.stick2_x - x, dy = m.km.stick2_y - y;
        if (dx * dx + dy * dy < 44 * 44) return 101;
    }
    return -1;
}

// the editor's backdrop: the frame (clear, a touch darker) and what's placed on it, the crosshair
static void editor_compose(void) {
    const size_t n = (size_t)m.bw * m.bh;
    if (!m.edit_base) return;
    // from the menu's frost to the editor's clear frame, as it opens
    const float k = smoother(m.ek);
    if (k >= 1) {
        memcpy(m.back, m.edit_base, n * 4);
    } else {
        const int fk = (int)(k * 256), dim = (int)((1 - P->dim_k) * 256), tint = (int)(P->tint_k * 256);
        for (size_t i = 0; i < n; i++) {
            const uint8_t *f = m.frost + i * 4, *e = m.edit_base + i * 4;
            int r = f[0] + (((P->tint_r - f[0]) * tint) >> 8), g = f[1] + (((P->tint_g - f[1]) * tint) >> 8),
                b = f[2] + (((P->tint_b - f[2]) * tint) >> 8);
            r = (r * dim) >> 8, g = (g * dim) >> 8, b = (b * dim) >> 8;
            uint8_t *d = m.back + i * 4;
            d[0] = (uint8_t)(r + (((e[0] - r) * fk) >> 8));
            d[1] = (uint8_t)(g + (((e[1] - g) * fk) >> 8));
            d[2] = (uint8_t)(b + (((e[2] - b) * fk) >> 8));
            d[3] = 255;
        }
    }
    layer L = { m.back, m.bw, m.bh };
    const float u = m.bw / 1920.0f; // the backdrop's pixels per 1080p pixel
    if (m.km.stick_r > 0) { // the joystick: its reach, its centre
        const int lit = m.hold == 100;
        ring(&L, m.km.stick_x * u, m.km.stick_y * u, m.km.stick_r * u, (3 + 2 * lit) * u, RGB(61, 220, 132), 0.85f * k);
        rrect(&L, (m.km.stick_x - m.km.stick_r) * u, (m.km.stick_y - m.km.stick_r) * u, 2 * m.km.stick_r * u, 2 * m.km.stick_r * u,
              m.km.stick_r * u, 1.5f, RGB(61, 220, 132), RGB(61, 220, 132), 0.10f * k);
        key_glyph(&L, KM_STICK_KEY, m.km.stick_x * u, m.km.stick_y * u, u, k, lit);
    }
    if (m.km.stick2_r > 0) { // the right stick's
        const int lit = m.hold == 101;
        ring(&L, m.km.stick2_x * u, m.km.stick2_y * u, m.km.stick2_r * u, (3 + 2 * lit) * u, RGB(170, 255, 205), 0.85f * k);
        rrect(&L, (m.km.stick2_x - m.km.stick2_r) * u, (m.km.stick2_y - m.km.stick2_r) * u, 2 * m.km.stick2_r * u,
              2 * m.km.stick2_r * u, m.km.stick2_r * u, 1.5f, RGB(170, 255, 205), RGB(170, 255, 205), 0.10f * k);
        key_glyph(&L, KM_STICK2_KEY, m.km.stick2_x * u, m.km.stick2_y * u, u, k, lit);
    }
    for (int i = 0; i < m.km.count; i++)
        key_glyph(&L, m.km.button[i].keycode, m.km.button[i].x * u, m.km.button[i].y * u, u, k, m.hold == i);
    // the crosshair
    const float cx = m.ex * u, cy = m.ey * u;
    ring(&L, cx, cy, 24 * u, 5 * u, RGB(0, 0, 0), 0.45f * k);
    ring(&L, cx, cy, 24 * u, 2.6f * u, RGB(255, 255, 255), k);
    line(&L, cx - 36 * u, cy, cx - 14 * u, cy, 2.4f * u, RGB(255, 255, 255), k);
    line(&L, cx + 14 * u, cy, cx + 36 * u, cy, 2.4f * u, RGB(255, 255, 255), k);
    line(&L, cx, cy - 36 * u, cx, cy - 14 * u, 2.4f * u, RGB(255, 255, 255), k);
    line(&L, cx, cy + 14 * u, cx, cy + 36 * u, 2.4f * u, RGB(255, 255, 255), k);
    m.back_ver++;
}

// the tray card (the card layers at the tray's size): the buttons to take, the help
static void tray_glass(void) {
    layer *L = &m.glass;
    memset(L->p, 0, (size_t)L->w * L->h * 4);
    const float S = m.s, x = MARGIN * S, y = MARGIN * S, w = TW * S, h = TH * S;
    rrect(L, x - 6 * S, y + 18 * S, w + 12 * S, h, 36 * S, 48 * S, RGB(0, 0, 0), RGB(0, 0, 0), P->shadow_a);
    rrect(L, x, y, w, h, 32 * S, 1, P->surface, P->surface, 0.98f);
}
static void tray_words(void) {
    layer *L = &m.words;
    memset(L->p, 0, (size_t)L->w * L->h * 4);
    const float S = m.s, x = MARGIN * S, y = MARGIN * S;
    const float tile = 76 * S, gap = 12 * S, row_w = TRAY_N * tile + (TRAY_N - 1) * gap;
    float tx = x + (TW * S - row_w) / 2, ty = y + 24 * S;
    for (int i = 0; i < TRAY_N; i++) {
        float lit = 1 - fminf(fabsf(m.tray_anim - i), 1);
        key_glyph(L, tray_keys[i], tx + i * (tile + gap) + tile / 2, ty + tile / 2, S * 0.92f, 1, lit);
    }
    // the help: each a button's picture (a face button's disc, a shoulder's pill with its name, the
    // touchpad's outline), then what it does
    static const struct { int b; const char *what; } hints[] = {
        { PB_CROSS, "Place / move" }, { PB_SQUARE, "Remove" }, { -1, "Choose" }, { -2, "Joystick size" },
        { PB_TRIANGLE, "Save" }, { PB_CIRCLE, "Cancel" }, { -3, "Move this" },
    };
    const int NH = 7;
    const float pill_w = 44 * S, pill_h = 30 * S, pill_gap = 6 * S, label_gap = 10 * S, between = 36 * S;
    float widths[7], total = 0;
    for (int i = 0; i < NH; i++) {
        float pic = hints[i].b > 0 ? 38 * S : hints[i].b == -3 ? 46 * S : 2 * pill_w + pill_gap;
        widths[i] = pic + label_gap + text_w(&f_small, hints[i].what);
        total += widths[i] + (i < NH - 1 ? between : 0);
    }
    float hx = x + (TW * S - total) / 2, hy = y + (TH - 38) * S;
    for (int i = 0; i < NH; i++) {
        float pic;
        if (hints[i].b > 0) { // a face button
            pic = 38 * S;
            rrect(L, hx, hy - 19 * S, 38 * S, 38 * S, 19 * S, 1.5f, P->disc, P->disc, P->disc_a);
            const float cx = hx + 19 * S, cy = hy, k = 8 * S;
            if (hints[i].b == PB_CROSS) line(L, cx - k, cy - k, cx + k, cy + k, 3.5f * S, P->sym, 1), line(L, cx - k, cy + k, cx + k, cy - k, 3.5f * S, P->sym, 1);
            else if (hints[i].b == PB_CIRCLE) ring(L, cx, cy, 9 * S, 3.2f * S, P->sym, 1);
            else if (hints[i].b == PB_TRIANGLE) {
                line(L, cx, cy - 9 * S, cx - 9 * S, cy + 7 * S, 3 * S, P->sym, 1), line(L, cx, cy - 9 * S, cx + 9 * S, cy + 7 * S, 3 * S, P->sym, 1);
                line(L, cx - 9 * S, cy + 7 * S, cx + 9 * S, cy + 7 * S, 3 * S, P->sym, 1);
            } else {
                line(L, cx - k, cy - k, cx + k, cy - k, 3 * S, P->sym, 1), line(L, cx - k, cy + k, cx + k, cy + k, 3 * S, P->sym, 1);
                line(L, cx - k, cy - k, cx - k, cy + k, 3 * S, P->sym, 1), line(L, cx + k, cy - k, cx + k, cy + k, 3 * S, P->sym, 1);
            }
        } else if (hints[i].b == -3) { // the touchpad: a wide rounded outline
            pic = 46 * S;
            rrect(L, hx, hy - 15 * S, pic, 30 * S, 9 * S, 1.5f, P->disc, P->disc, P->disc_a);
            line(L, hx + 8 * S, hy - 9 * S, hx + pic - 8 * S, hy - 9 * S, 2.2f * S, P->sym, 1);
            line(L, hx + 8 * S, hy + 9 * S, hx + pic - 8 * S, hy + 9 * S, 2.2f * S, P->sym, 1);
            line(L, hx + 6 * S, hy - 7 * S, hx + 6 * S, hy + 7 * S, 2.2f * S, P->sym, 1);
            line(L, hx + pic - 6 * S, hy - 7 * S, hx + pic - 6 * S, hy + 7 * S, 2.2f * S, P->sym, 1);
        } else { // two shoulders: pills with their names
            pic = 2 * pill_w + pill_gap;
            const char *names[2] = { hints[i].b == -1 ? "L1" : "L2", hints[i].b == -1 ? "R1" : "R2" };
            for (int k = 0; k < 2; k++) {
                const float px = hx + k * (pill_w + pill_gap);
                rrect(L, px, hy - pill_h / 2, pill_w, pill_h, 9 * S, 1.5f, P->disc, P->disc, P->disc_a * 1.4f);
                text(L, &f_small, px + pill_w / 2 - text_w(&f_small, names[k]) / 2, hy + 7 * S, names[k], P->sym, 1);
            }
        }
        text(L, &f_small, hx + pic + label_gap, hy + 8 * S, hints[i].what, C_TEXT, 1);
        hx += widths[i] + between;
    }
}
static void tray_update(void) {
    if (m.tray_anim == m.drawn_tray && m.card_ver) return;
    m.drawn_tray = m.tray_anim;
    tray_words();
    layer *C = &m.card;
    memcpy(C->p, m.glass.p, (size_t)C->w * C->h * 4);
    const float S = m.s, x = MARGIN * S, y = MARGIN * S, tile = 76 * S, gap = 12 * S, row_w = TRAY_N * tile + (TRAY_N - 1) * gap;
    const float px = x + (TW * S - row_w) / 2 + m.tray_anim * (tile + gap) - 7 * S, py = y + 24 * S - 7 * S;
    rrect(C, px, py, tile + 14 * S, tile + 14 * S, 26 * S, 1.5f, P->pill, P->pill, 1); // the chosen one's tile
    const size_t n = (size_t)C->w * C->h;
    const uint8_t *w = m.words.p;
    uint8_t *d = C->p;
    for (size_t i = 0; i < n; i++, w += 4, d += 4) {
        if (!w[3]) continue;
        int k = 255 - w[3];
        d[0] = (uint8_t)(w[0] + (d[0] * k + 127) / 255), d[1] = (uint8_t)(w[1] + (d[1] * k + 127) / 255);
        d[2] = (uint8_t)(w[2] + (d[2] * k + 127) / 255), d[3] = (uint8_t)(w[3] + (d[3] * k + 127) / 255);
    }
    m.card_ver++;
}

// the card layers made again at this mode's size (the menu's card, or the tray)
static int card_layers(int tray) {
    free(m.glass.p), free(m.words.p), free(m.card.p);
    m.glass.p = m.words.p = m.card.p = NULL;
    const int cw = (int)(((tray ? TW : CW) + 2 * MARGIN) * m.s), ch = (int)(((tray ? TH : CH) + 2 * MARGIN) * m.s);
    if (!layer_new(&m.glass, cw, ch) || !layer_new(&m.words, cw, ch) || !layer_new(&m.card, cw, ch)) return 0;
    m.drawn_sel = -1, m.drawn_tray = -1;
    if (tray) tray_glass();
    else draw_glass();
    m.card_ver++;
    return 1;
}

static void km_convert(int to_tv) { // the keymap's points: the game's screen <-> the TV's picture
    void (*f)(float, float, float *, float *) = to_tv ? view_to_tv : view_from_tv;
    for (int i = 0; i < m.km.count; i++) f(m.km.button[i].x, m.km.button[i].y, &m.km.button[i].x, &m.km.button[i].y);
    if (m.km.stick_r > 0) f(m.km.stick_x, m.km.stick_y, &m.km.stick_x, &m.km.stick_y);
    if (m.km.stick2_r > 0) f(m.km.stick2_x, m.km.stick2_y, &m.km.stick2_x, &m.km.stick2_y);
    // (a joystick's reach: in the game's pixels, as the TV's scale goes)
    const float k = phone_view() ? (to_tv ? 1080.0f / droid_game_h : (float)droid_game_h / 1080.0f) : 1;
    m.km.stick_r *= k, m.km.stick2_r *= k;
}
static void editor_enter(void) {
    keymap_read(&m.km);
    km_convert(1);
    m.ex = 960, m.ey = 540, m.hold = -1;
    m.tray = 0, m.tray_anim = 0;
    const size_t n = (size_t)m.bw * m.bh;
    if (!m.edit_base) m.edit_base = malloc(n * 4);
    if (m.edit_base) // the frame clear, a touch darker
        for (size_t i = 0; i < n * 4; i++) m.edit_base[i] = (i & 3) == 3 ? 255 : (uint8_t)((m.sharp[i] * 200) >> 8);
    droid_log("quick menu: Keymap Editor (joysticks: left %s, right %s; %d buttons)", m.km.stick_r > 0 ? "yes" : "no",
              m.km.stick2_r > 0 ? "yes" : "no", m.km.count);
}

static void editor_place(void) {
    const int key = tray_keys[m.tray];
    if (key == KM_STICK_KEY) {
        m.km.stick_x = m.ex, m.km.stick_y = m.ey;
        if (m.km.stick_r <= 0) m.km.stick_r = 130;
        return;
    }
    if (key == KM_STICK2_KEY) {
        m.km.stick2_x = m.ex, m.km.stick2_y = m.ey;
        if (m.km.stick2_r <= 0) m.km.stick2_r = 130;
        return;
    }
    for (int i = 0; i < m.km.count; i++) // each button once: placed again, it moves
        if (m.km.button[i].keycode == key) {
            m.km.button[i].x = m.ex, m.km.button[i].y = m.ey;
            return;
        }
    if (m.km.count < KEYMAP_MAX) {
        m.km.button[m.km.count].keycode = key;
        m.km.button[m.km.count].x = m.ex, m.km.button[m.km.count].y = m.ey;
        m.km.count++;
    }
}
static void editor_remove(int idx) {
    if (idx == 100) {
        m.km.stick_r = 0;
    } else if (idx == 101) {
        m.km.stick2_r = 0;
    } else if (idx >= 0 && idx < m.km.count) {
        memmove(&m.km.button[idx], &m.km.button[idx + 1], sizeof(m.km.button[0]) * (size_t)(m.km.count - idx - 1));
        m.km.count--;
    }
    m.hold = -1;
}

// one frame of the editor's buttons (b: held now, pressed: just now); 1: it closes (saved or not)
void droid_menu_touch(int *n, float *x, float *y); // app_main.c: the touchpad's first finger (0..1)
static int editor_input(uint32_t b, uint32_t pressed, float sx, float sy, float dt) {
    { // the tray: a finger on the touchpad drags it (out of the way of a game's button)
        int n = 0;
        float tx = 0, ty = 0;
        droid_menu_touch(&n, &tx, &ty);
        static int was;
        static float lx, ly;
        if (n > 0 && was) {
            m.tray_dx += (tx - lx) * 1920 * 1.3f, m.tray_dy += (ty - ly) * 1080 * 1.3f;
            const float half = ((1920 - (TW + 2 * MARGIN)) / 2.0f), up = 1080 - (TH + 2 * MARGIN);
            m.tray_dx = m.tray_dx < -half ? -half : m.tray_dx > half ? half : m.tray_dx;
            m.tray_dy = m.tray_dy < -up ? -up : m.tray_dy > 0 ? 0 : m.tray_dy;
        }
        was = n > 0, lx = tx, ly = ty;
    }
    // the crosshair: the stick (faster further out), the D-pad a few pixels
    float mag = sqrtf(sx * sx + sy * sy);
    if (mag > 0.18f) {
        float sp = 1300 * powf((fminf(mag, 1) - 0.18f) / 0.82f, 1.8f) * dt;
        m.ex += sx / mag * sp, m.ey += sy / mag * sp;
    }
    if (pressed & PB_LEFT) m.ex -= 12;
    if (pressed & PB_RIGHT) m.ex += 12;
    if (pressed & PB_UP) m.ey -= 12;
    if (pressed & PB_DOWN) m.ey += 12;
    m.ex = m.ex < 0 ? 0 : m.ex > 1919 ? 1919 : m.ex;
    m.ey = m.ey < 0 ? 0 : m.ey > 1079 ? 1079 : m.ey;
    if (m.hold == 100) m.km.stick_x = m.ex, m.km.stick_y = m.ey; // what's held follows it
    else if (m.hold == 101) m.km.stick2_x = m.ex, m.km.stick2_y = m.ey;
    else if (m.hold >= 0 && m.hold < m.km.count) m.km.button[m.hold].x = m.ex, m.km.button[m.hold].y = m.ey;
    if (pressed & (PB_L1 | PB_R1)) {
        m.tray = (m.tray + ((pressed & PB_L1) ? TRAY_N - 1 : 1)) % TRAY_N;
        fx(SND_STEP, 1.0f, 0.12f, 14);
    }
    if (b & (PB_L2 | PB_R2)) { // the joystick's reach (the one under the crosshair, or held)
        const int at = m.hold >= 100 ? m.hold : editor_marker_at(m.ex, m.ey);
        float *r = at == 100 && m.km.stick_r > 0 ? &m.km.stick_r : at == 101 && m.km.stick2_r > 0 ? &m.km.stick2_r : NULL;
        if (r) {
            *r += ((b & PB_R2) ? 1 : -1) * 180 * dt;
            *r = *r < 50 ? 50 : *r > 360 ? 360 : *r;
        }
    }
    if (pressed & PB_CROSS) {
        if (m.hold >= 0) {
            m.hold = -1; // put down
            fx(SND_CONFIRM, 0.8f, 0.3f, 30);
        } else {
            int at = editor_marker_at(m.ex, m.ey);
            if (at >= 0) {
                m.hold = at; // picked up: it follows the crosshair
                fx(SND_CHANGE, 1.0f, 0.25f, 24);
            } else {
                editor_place();
                fx(SND_CONFIRM, 0.8f, 0.3f, 30);
            }
        }
    }
    if (pressed & PB_SQUARE) {
        int at = m.hold >= 0 ? m.hold : editor_marker_at(m.ex, m.ey);
        if (at >= 0) {
            editor_remove(at);
            fx(SND_CLOSE, 1.0f, 0.22f, 26);
        }
    }
    if (pressed & (PB_TRIANGLE | PB_OPTIONS)) {
        km_convert(0); // (saved in the game's screen's points)
        keymap_write(&m.km); // every host plays it from the next button
        fx(SND_CONFIRM, 1.0f, 0.5f, 70);
        return 1;
    }
    if (pressed & PB_CIRCLE) {
        fx(SND_CLOSE, 1.0f, 0.22f, 26);
        return 1;
    }
    return 0;
}

static void free_all(void) {
    free(m.edit_base);
    m.edit_base = NULL;
    free(m.sharp), free(m.frost), free(m.back);
    free(m.glass.p), free(m.words.p), free(m.card.p);
    m.sharp = m.frost = m.back = NULL;
    m.glass.p = m.words.p = m.card.p = NULL;
}

int quick_menu_begin(const uint8_t *px, int w, int h, size_t stride, int bgra, int bottom_up, int screen_w, int screen_h) {
    requested = 0;
    if (m.on || !px || w <= 0 || h <= 0) return 0;
    double t0 = now_s();
    free_all();
    unsigned bv = m.back_ver, cv = m.card_ver; // (versions go on across menus: a cache never mistakes one)
    memset(&m, 0, sizeof(m));
    m.back_ver = bv + 1, m.card_ver = cv + 1;
    m.sw = screen_w, m.sh = screen_h;
    m.s = screen_h / 1080.0f;
    P = &palettes[app_settings.theme ? 1 : 0];
    if (m.s < 0.5f) m.s = 0.5f;
    const int cw = (int)((CW + 2 * MARGIN) * m.s), ch = (int)((CH + 2 * MARGIN) * m.s);
    if (!faces_ready(m.s) || !backdrop_make(px, w, h, stride, bgra, bottom_up) || !layer_new(&m.glass, cw, ch) ||
        !layer_new(&m.words, cw, ch) || !layer_new(&m.card, cw, ch)) {
        droid_log("quick menu: could not open (no memory or no font)");
        free_all();
        return 0;
    }
    draw_glass();
    m.on = 1;
    m.state = ST_OPENING;
    m.held = ~0u; // the buttons down now (Options) count once they're let go and pressed again
    m.last = m.opened_at = now_s();
    m.drawn_sel = -1;
    droid_game_paused = 1;
    ui_sound_open(); // (its own port beside the game's; kept open after: a sound isn't cut at the resume)
    fx(SND_OPEN, 1.0f, 0.35f, 40);
    backdrop_update();
    card_update();
    droid_log("quick menu: open (the frame %dx%d, the screen %dx%d, made in %.1f ms)", w, h, screen_w, screen_h,
              (now_s() - t0) * 1000);
    return 1;
}

// PC tests: DROID_QM_KEYS="d d r x" pressed one by one (u d l r x o: up down left right cross
// circle; q e s t: L1 R1 square triangle), 0.7 s apart from 1 s after the open; a picture of the
// menu before each (qm_<n>.ppm)
static void test_shot(int n);
static uint32_t test_keys(double t) {
    const char *e = getenv("DROID_QM_KEYS");
    if (!e) return 0;
    char keys[64];
    int n = 0;
    for (; *e && n < 63; e++)
        if (*e != ' ') keys[n++] = *e;
    if (m.test_step > n) return 0;
    double at = 1.0 + m.test_step * 0.7; // this key's time
    if (t - m.opened_at < at) return 0;
    static int shot_for = -1, frames_held;
    if (shot_for != m.test_step) { // the picture just before the key
        shot_for = m.test_step;
        test_shot(m.test_step);
        frames_held = 0;
    }
    if (m.test_step == n) return 0; // (the last picture: after the last key)
    if (frames_held++ >= 1) { // pressed for one frame (counted after the picture: it takes a while), then the next one
        m.test_step++;
        return 0;
    }
    switch (keys[m.test_step]) {
    case 'u': return PB_UP;
    case 'd': return PB_DOWN;
    case 'l': return PB_LEFT;
    case 'r': return PB_RIGHT;
    case 'x': return PB_CROSS;
    case 'o': return PB_CIRCLE;
    case 'q': return PB_L1;
    case 'e': return PB_R1;
    case 's': return PB_SQUARE;
    case 't': return PB_TRIANGLE;
    }
    return 0;
}

static void set_stats(int dir) {
    int v = app_settings.stats < 0 ? 0 : app_settings.stats > 2 ? 2 : app_settings.stats;
    app_settings.stats = (v + (dir < 0 ? 2 : 1)) % 3;
    app_settings_save();
    m.kick = dir < 0 ? -1 : 1;
    fx(SND_CHANGE, 1.0f, 0.28f, 26);
}

int quick_menu_frame(void) {
    if (!m.on) return 0;
    // paced to the screen's 60 (a swap that doesn't wait, as the PS5's Vulkan, would spin)
    double t = now_s();
    if (m.next_at > t) {
        double left = m.next_at - t;
        struct timespec d = { 0, (long)((left > 0.002 ? left - 0.001 : left) * 1e9) };
        nanosleep(&d, NULL);
        while ((t = now_s()) < m.next_at - 0.0002) {
        }
    }
    m.next_at = (m.next_at > t - 0.05 ? m.next_at : t) + 1 / 59.94;
    float dt = (float)(t - m.last);
    if (dt > 0.1f) dt = 0.1f;
    m.last = t;

    // the pad
    uint32_t b = 0;
    uint8_t lx = 128, ly = 128;
    droid_menu_pad(&b, &lx, &ly);
    const float sx = (lx - 127.5f) / 127.5f, sy = (ly - 127.5f) / 127.5f; // (the editor: its crosshair)
    if (m.mode == 0) { // the menu: the stick as the D-pad
        if (ly < 50) b |= PB_UP;
        if (ly > 206) b |= PB_DOWN;
        if (lx < 50) b |= PB_LEFT;
        if (lx > 206) b |= PB_RIGHT;
    }
    b |= test_keys(t);
    uint32_t pressed = b & ~m.held;
    uint32_t dirs = b & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT);
    if (dirs && (dirs & m.held) == dirs && t >= m.repeat_at) {
        pressed |= m.mode ? dirs : dirs & (PB_UP | PB_DOWN); // held, they repeat (the editor: all four, fast)
        m.repeat_at = t + (m.mode ? 0.03 : 0.11);
    } else if (dirs & ~m.held) {
        m.repeat_at = t + (m.mode ? 0.25 : 0.38);
    }
    m.held = b;
    if (m.state == ST_OPENING && m.switching == 0 && m.mode == 1) {
        if (editor_input(b, pressed, sx, sy, dt)) m.switching = 2; // saved or not: back to the menu
    } else if (m.state == ST_OPENING && m.switching == 0) {
        if (pressed & (PB_UP | PB_DOWN)) {
            m.sel = (m.sel + ((pressed & PB_UP) ? IT_COUNT - 1 : 1)) % IT_COUNT;
            fx(SND_STEP, 1.0f, 0.14f, 16);
        }
        if (m.sel == IT_STATS && (pressed & (PB_LEFT | PB_RIGHT))) set_stats((pressed & PB_LEFT) ? -1 : 1);
        if (pressed & PB_CROSS) {
            m.press = 1;
            if (m.sel == IT_STATS) {
                set_stats(1);
            } else if (m.sel == IT_CONTROLS) {
                m.switching = 1; // the card goes, the editor comes
                fx(SND_OPEN, 1.0f, 0.3f, 36);
            } else if (m.sel == IT_RESUME) {
                m.state = ST_CLOSING;
                m.action = IT_RESUME;
                fx(SND_CLOSE, 1.0f, 0.22f, 26);
            } else {
                m.state = ST_LEAVING;
                m.action = m.sel;
                fx(SND_CONFIRM, 1.0f, 0.6f, 90);
            }
        } else if (pressed & (PB_CIRCLE | PB_OPTIONS | PB_TRIANGLE)) {
            m.state = ST_CLOSING;
            m.action = IT_RESUME;
            fx(SND_CLOSE, 1.0f, 0.22f, 26);
        }
    }

    // the animations
    if (m.switching) { // to the editor or back: the card fades out, then the other one in
        m.o = clamp01(m.o - dt / 0.15f);
        if (m.o <= 0) {
            m.mode = m.switching == 1;
            if (m.mode) editor_enter();
            if (!card_layers(m.mode)) m.mode = 0, card_layers(0);
            m.switching = 0;
        }
    } else if (m.state == ST_OPENING) {
        m.o = clamp01(m.o + dt / 0.26f);
        if (m.mode == 0) m.blur = m.o;
    } else if (m.state == ST_CLOSING) {
        m.o = clamp01(m.o - dt / 0.20f);
        m.blur = m.o;
    } else { // leaving: the card goes, then the screen goes dark
        m.o = clamp01(m.o - dt / 0.22f);
        m.black = clamp01(m.black + dt / 0.45f);
    }
    m.sel_anim = ease_to(m.sel_anim, (float)m.sel, dt, 17);
    if (fabsf(m.sel_anim - m.sel) < 0.002f) m.sel_anim = (float)m.sel;
    m.kick = ease_to(m.kick, 0, dt, 14);
    if (fabsf(m.kick) < 0.01f) m.kick = 0;
    m.press = m.press > 0.01f ? m.press * expf(-9 * dt) : 0;
    m.ek = clamp01(m.ek + ((m.mode == 1 && m.switching != 2) ? dt : -dt) / 0.3f);
    m.tray_anim = ease_to(m.tray_anim, (float)m.tray, dt, 18);
    if (fabsf(m.tray_anim - m.tray) < 0.002f) m.tray_anim = (float)m.tray;
    double w0 = now_s();
    if (m.mode == 1 || m.ek > 0) {
        editor_compose(); // (each frame: the crosshair moves)
        m.back_blur = -1;  // (the menu's backdrop made again once it's back)
    } else {
        backdrop_update();
    }
    if (m.mode == 1) tray_update();
    else card_update();
    float work = (float)(now_s() - w0);
    if (work > m.slowest_work) m.slowest_work = work;
    if (m.frames++ > 1 && dt > m.slowest_gap) m.slowest_gap = dt;
    if (m.state == ST_CLOSING && m.o <= 0) return 0;
    if (m.state == ST_LEAVING && m.black >= 1 && m.o <= 0) {
        static int dark_frames; // a few frames of the black on the screen, then on
        if (++dark_frames >= 3) {
            dark_frames = 0;
            return 0;
        }
    }
    return 1;
}

const uint8_t *quick_menu_backdrop(int *w, int *h, unsigned *version) {
    if (!m.on) return NULL;
    *w = m.bw, *h = m.bh, *version = m.back_ver;
    return m.back;
}

const uint8_t *quick_menu_card(int *x, int *y, int *w, int *h, float *alpha, unsigned *version) {
    if (!m.on) return NULL;
    float o = smoother(m.o);
    *w = m.card.w, *h = m.card.h;
    *x = (m.sw - m.card.w) / 2;
    if (m.mode == 1) { // the editor's tray: at the bottom, or where the touchpad moved it
        *x += (int)(m.tray_dx * m.s);
        *y = m.sh - m.card.h + (int)((m.tray_dy + (1 - o) * 26) * m.s);
    }
    else *y = (m.sh - m.card.h) / 2 + (int)((1 - o) * 26 * m.s); // it rises into its place as it fades in
    *alpha = o;
    *version = m.card_ver;
    return m.card.p;
}

void quick_menu_card_max(int *w, int *h) { // the biggest card (the menu's, the editor's tray)
    *w = (int)(((TW > CW ? TW : CW) + 2 * MARGIN) * m.s);
    *h = (int)(((TH > CH ? TH : CH) + 2 * MARGIN) * m.s);
}

void quick_menu_card_opaque(uint8_t *dst, size_t dst_stride, int bgra) {
    int cx, cy, cw, ch;
    float a;
    unsigned v;
    const uint8_t *card = quick_menu_card(&cx, &cy, &cw, &ch, &a, &v);
    if (!card) return;
    const int ia = (int)(a * 256), ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    // the backdrop under it, as the stretch samples it (linear, pixel centres)
    const float fx = (float)m.bw / m.sw, fy = (float)m.bh / m.sh;
    static int *u0s, *fus, cap;
    if (cap < cw) {
        free(u0s), free(fus);
        u0s = malloc(sizeof(int) * cw), fus = malloc(sizeof(int) * cw);
        cap = u0s && fus ? cw : 0;
        if (!cap) return;
    }
    for (int i = 0; i < cw; i++) {
        float u = (cx + i + 0.5f) * fx - 0.5f;
        if (u < 0) u = 0;
        int u0 = (int)u;
        if (u0 >= m.bw - 1) u0 = m.bw - 2, u = (float)(m.bw - 1);
        u0s[i] = u0, fus[i] = (int)((u - u0) * 256);
    }
    for (int j = 0; j < ch; j++) {
        float v = (cy + j + 0.5f) * fy - 0.5f;
        if (v < 0) v = 0;
        int v0 = (int)v;
        if (v0 >= m.bh - 1) v0 = m.bh - 2, v = (float)(m.bh - 1);
        int fv = (int)((v - v0) * 256);
        const uint8_t *r0 = m.back + (size_t)v0 * m.bw * 4, *r1 = r0 + (size_t)m.bw * 4;
        const uint8_t *c = card + (size_t)j * cw * 4;
        uint8_t *d = dst + (size_t)j * dst_stride;
        for (int i = 0; i < cw; i++, c += 4, d += 4) {
            const uint8_t *p00 = r0 + u0s[i] * 4, *p10 = p00 + 4, *p01 = r1 + u0s[i] * 4, *p11 = p01 + 4;
            int fu = fus[i], k = 256 - ((c[3] * ia) >> 8);
            for (int q = 0; q < 3; q++) {
                int top = p00[q] * 256 + (p10[q] - p00[q]) * fu, bot = p01[q] * 256 + (p11[q] - p01[q]) * fu;
                int bd = (top * 256 + (bot - top) * fv) >> 16;
                int val = ((c[q] * ia) >> 8) + ((bd * k) >> 8);
                d[q == 0 ? ri : q == 2 ? bi : 1] = (uint8_t)(val > 255 ? 255 : val);
            }
            d[3] = 255;
        }
    }
}

static void test_shot(int n) { // PC tests: the menu as the screen shows it (backdrop + card), a .ppm
    if (!getenv("DROID_QM_KEYS") || getenv("DROID_QM_NOSHOT") || !m.on) return;
    int cx, cy, cw, ch;
    float a;
    unsigned v;
    if (!quick_menu_card(&cx, &cy, &cw, &ch, &a, &v)) return;
    uint8_t *cardpx = malloc((size_t)cw * ch * 4);
    if (!cardpx) return;
    quick_menu_card_opaque(cardpx, (size_t)cw * 4, 0);
    char name[32];
    snprintf(name, sizeof(name), "qm_%d.ppm", n);
    FILE *f = fopen(name, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", m.sw, m.sh);
        for (int y = 0; y < m.sh; y++)
            for (int x = 0; x < m.sw; x++) {
                const uint8_t *p;
                if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) p = cardpx + ((size_t)(y - cy) * cw + (x - cx)) * 4;
                else p = m.back + ((size_t)(y * m.bh / m.sh) * m.bw + (size_t)x * m.bw / m.sw) * 4;
                fwrite(p, 1, 3, f);
            }
        fclose(f);
        droid_log("quick menu: test picture %s", name);
    }
    free(cardpx);
}

// Close Game (and a game that can't start or crashed): the app closes, the reason in the log
static void relaunch_msg(const char *what, const char *msg);
static void relaunch(const char *what) { relaunch_msg(what, NULL); }
void quick_menu_relaunch(const char *what) { relaunch_msg(what, NULL); } // (app_main.c: a game that can't start)
void quick_menu_relaunch_msg(const char *what, const char *msg) { relaunch_msg(what, msg); }
static void relaunch_msg(const char *what, const char *msg) {
    droid_log("quick menu: %s (%s): the app closes (%s)", what, game_id, msg ? msg : "");
    fflush(NULL);
    _exit(0);
}

void quick_menu_end(void) {
    if (!m.on) return;
    int action = m.action;
    m.on = 0;
    double open_s = now_s() - m.opened_at;
    droid_log("quick menu: shut (%s): %d frames in %.2f s (%.1f fps), slowest frame %.1f ms, slowest drawing %.1f ms",
              item_names[action], m.frames, open_s, m.frames / (open_s > 0 ? open_s : 1), m.slowest_gap * 1000,
              m.slowest_work * 1000);
    if (action == IT_CLOSE) relaunch("back");
    free_all();
    resumed = 1;
    droid_game_paused = 0;
}
