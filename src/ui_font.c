// ui_font.c — the menus' text (ui_font.h), with stb_truetype (public domain).

#include "ui_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_shim.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define ATLAS_MAX 2048 // the big styles' atlas; the others 1024
#define FIRST 32
#define COUNT 224 // 32..255: ASCII and Latin-1

static const struct { const char *file; float px; } style_def[UI_TEXT_STYLES] = {
    { "Outfit-Regular.ttf", 22 }, { "Outfit-SemiBold.ttf", 26 }, { "Outfit-Regular.ttf", 30 },
    { "Outfit-SemiBold.ttf", 34 }, { "Outfit-Bold.ttf", 48 },     { "Outfit-Bold.ttf", 72 },
};

static struct {
    unsigned tex;
    int atlas; // its size
    stbtt_packedchar chars[COUNT];
    float ascent, descent, cap; // cap: the capitals' height
} styles[UI_TEXT_STYLES];

static unsigned char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)len);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (n) *n = (size_t)len;
    return buf;
}

static char font_dir_kept[256]; // for a new size (ui_font_set_scale)
static float font_scale = 1;

int ui_font_init(const char *font_dir) {
    snprintf(font_dir_kept, sizeof(font_dir_kept), "%s", font_dir);
    unsigned char *atlas = malloc(ATLAS_MAX * ATLAS_MAX);
    if (!atlas) return -1;
    for (int s = 0; s < UI_TEXT_STYLES; s++) {
        const float px = style_def[s].px * font_scale;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", font_dir, style_def[s].file);
        unsigned char *ttf = read_file(path, NULL);
        if (!ttf) {
            droid_log("ui: font %s missing", path);
            free(atlas);
            return -2;
        }
        int size = px >= 40 ? 2048 : 1024;
        styles[s].atlas = size;
        stbtt_pack_context pc;
        memset(atlas, 0, (size_t)size * size);
        stbtt_PackBegin(&pc, atlas, size, size, 0, 1, NULL);
        stbtt_PackSetOversampling(&pc, 2, 1); // sharper horizontal placement
        stbtt_PackFontRange(&pc, ttf, 0, px, FIRST, COUNT, styles[s].chars);
        stbtt_PackEnd(&pc);
        stbtt_fontinfo info;
        stbtt_InitFont(&info, ttf, 0);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        float scale = stbtt_ScaleForPixelHeight(&info, px);
        styles[s].ascent = asc * scale;
        styles[s].descent = -desc * scale;
        int hx0, hy0, hx1, hy1;
        styles[s].cap = stbtt_GetCodepointBox(&info, 'H', &hx0, &hy0, &hx1, &hy1) ? hy1 * scale : styles[s].ascent * 0.7f;
        styles[s].tex = ui_texture_mask(atlas, size, size);
        free(ttf);
    }
    free(atlas);
    return 0;
}

void ui_font_set_scale(float scale) {
    if (scale == font_scale) return;
    font_scale = scale;
    if (!font_dir_kept[0]) return;
    for (int s = 0; s < UI_TEXT_STYLES; s++) // (only here: on a new window the old ids aren't its own)
        if (styles[s].tex) ui_texture_free(styles[s].tex);
    ui_font_init(font_dir_kept);
}

// the next character of UTF-8 text (Latin-1 range; others: '?')
static int next_char(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0x80) {
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        int v = c & (0x3F >> extra);
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++) v = (v << 6) | (*p++ & 0x3F);
        c = v;
    }
    *s = (const char *)p;
    return c >= FIRST && c < FIRST + COUNT ? c : '?';
}

float ui_text_width(ui_text_style style, const char *s) {
    float w = 0;
    while (*s) w += styles[style].chars[next_char(&s) - FIRST].xadvance;
    return w;
}

float ui_text_height(ui_text_style style) { return styles[style].ascent + styles[style].descent; }
float ui_text_cap(ui_text_style style) { return styles[style].cap; }
float ui_text_cap_mid(ui_text_style style) { return styles[style].ascent - styles[style].cap / 2; }

void ui_text(ui_text_style style, float x, float y, const char *s, ui_color col, int align) {
    if (!styles[style].tex) return;
    if (align != UI_ALIGN_LEFT) {
        float w = ui_text_width(style, s);
        x -= align == UI_ALIGN_CENTER ? w / 2 : w;
    }
    float base = y + styles[style].ascent;
    const float a = (float)styles[style].atlas;
    while (*s) {
        const stbtt_packedchar *g = &styles[style].chars[next_char(&s) - FIRST];
        if (g->x1 > g->x0)
            ui_mask(styles[style].tex, x + g->xoff, base + g->yoff, g->xoff2 - g->xoff, g->yoff2 - g->yoff,
                    g->x0 / a, g->y0 / a, g->x1 / a, g->y1 / a, col);
        x += g->xadvance;
    }
}

void ui_text_fit(ui_text_style style, float x, float y, float max_w, const char *s, ui_color col, int align) {
    if (ui_text_width(style, s) <= max_w) {
        ui_text(style, x, y, s, col, align);
        return;
    }
    char buf[256];
    size_t n = strlen(s);
    if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
    memcpy(buf, s, n);
    buf[n] = 0;
    while (n > 0) {
        do n--; while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80); // whole characters
        memcpy(buf + n, "...", 4);
        if (ui_text_width(style, buf) <= max_w) break;
    }
    ui_text(style, x, y, buf, col, align);
}
