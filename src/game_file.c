// game_file.c — the game file (game_file.h): its package, name, version and icon, read from
// its APK (or the APK inside an .xapk / .apks / .apkm bundle).

#include "game_file.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

#include "android_shim.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // some bundles' "icon.png" is a JPEG (Among Us)
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#include "stb_image.h"

#define ICON 256 // icons are kept at this size
#define ICON_RULES 2 // how icons are made (2: adaptive icons' layers, margins cut): older cached ones are made again

// ---- a zip's entries ----------------------------------------------------------------

typedef struct {
    FILE *f;
    uint8_t *cd; // the central directory
    uint32_t cd_size, count;
    long base; // where the zip starts in the file (an APK stored inside an XAPK: its offset)
} zip;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// the zip at [base, base + size) of the file (size 0: to the end)
static int zip_open_at(zip *z, const char *path, long base, long size) {
    memset(z, 0, sizeof(*z));
    z->f = fopen(path, "rb");
    if (!z->f) return -1;
    z->base = base;
    if (!size) {
        fseek(z->f, 0, SEEK_END);
        size = ftell(z->f) - base;
    }
    long tail = size < 66000 ? size : 66000;
    uint8_t *buf = malloc((size_t)tail);
    fseek(z->f, base + size - tail, SEEK_SET);
    if (!buf || fread(buf, 1, (size_t)tail, z->f) != (size_t)tail) goto fail;
    for (long i = tail - 22; i >= 0; i--)
        if (u32(buf + i) == 0x06054b50) { // the end of the central directory
            z->count = u16(buf + i + 10);
            z->cd_size = u32(buf + i + 12);
            uint32_t at = u32(buf + i + 16);
            z->cd = malloc(z->cd_size);
            fseek(z->f, z->base + at, SEEK_SET);
            if (!z->cd || fread(z->cd, 1, z->cd_size, z->f) != z->cd_size) goto fail;
            free(buf);
            return 0;
        }
fail:
    free(buf);
    if (z->f) fclose(z->f);
    free(z->cd);
    memset(z, 0, sizeof(*z));
    return -1;
}

static int zip_open(zip *z, const char *path) { return zip_open_at(z, path, 0, 0); }

static void zip_close(zip *z) {
    if (z->f) fclose(z->f);
    free(z->cd);
    memset(z, 0, sizeof(*z));
}

// each entry: name (not NUL-terminated: len), its central-directory record
typedef int (*zip_visit)(const char *name, int len, const uint8_t *rec, void *ctx);
static void zip_each(zip *z, zip_visit visit, void *ctx) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < z->count && at + 46 <= z->cd_size; i++) {
        const uint8_t *r = z->cd + at;
        if (u32(r) != 0x02014b50) break;
        int nlen = u16(r + 28), xlen = u16(r + 30), clen = u16(r + 32);
        if (visit((const char *)r + 46, nlen, r, ctx)) return;
        at += 46 + nlen + xlen + clen;
    }
}

// an entry's bytes (at most max), NULL if too big / unreadable
static uint8_t *zip_read(zip *z, const uint8_t *rec, size_t max, size_t *out_len) {
    uint32_t csize = u32(rec + 20), usize = u32(rec + 24), local = u32(rec + 42);
    int method = u16(rec + 10);
    if (usize > max || (method != 0 && method != 8)) return NULL;
    uint8_t lh[30];
    fseek(z->f, z->base + local, SEEK_SET);
    if (fread(lh, 1, 30, z->f) != 30 || u32(lh) != 0x04034b50) return NULL;
    fseek(z->f, z->base + local + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET);
    uint8_t *comp = malloc(csize ? csize : 1), *data = malloc(usize + 1);
    if (!comp || !data || fread(comp, 1, csize, z->f) != csize) {
        free(comp);
        free(data);
        return NULL;
    }
    if (method == 0) {
        memcpy(data, comp, usize);
    } else {
        z_stream s;
        memset(&s, 0, sizeof(s));
        s.next_in = comp;
        s.avail_in = csize;
        s.next_out = data;
        s.avail_out = usize;
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK || inflate(&s, Z_FINISH) != Z_STREAM_END) {
            inflateEnd(&s);
            free(comp);
            free(data);
            return NULL;
        }
        inflateEnd(&s);
    }
    free(comp);
    data[usize] = 0;
    if (out_len) *out_len = usize;
    return data;
}

typedef struct {
    const char *want;
    const uint8_t *rec;
} find_ctx;
static int find_visit(const char *name, int len, const uint8_t *rec, void *ctx) {
    find_ctx *c = ctx;
    if ((int)strlen(c->want) == len && !memcmp(name, c->want, (size_t)len)) {
        c->rec = rec;
        return 1;
    }
    return 0;
}
static uint8_t *zip_get(zip *z, const char *name, size_t max, size_t *len) {
    find_ctx c = { name, NULL };
    zip_each(z, find_visit, &c);
    return c.rec ? zip_read(z, c.rec, max, len) : NULL;
}

// ---- names -----------------------------------------------------------------------

// "com.KomoriGameDev.TheEndlessTunnel" -> "The Endless Tunnel"
static void name_from_package(const char *pkg, char *out, size_t n) {
    const char *last = strrchr(pkg, '.');
    last = last ? last + 1 : pkg;
    size_t at = 0;
    for (size_t i = 0; last[i] && at + 2 < n; i++) {
        char c = last[i];
        if (c == '_' || c == '-') c = ' ';
        if (i > 0 && isupper((unsigned char)c) && islower((unsigned char)last[i - 1])) out[at++] = ' ';
        out[at++] = (char)(i == 0 ? toupper((unsigned char)c) : c);
    }
    out[at] = 0;
}

// "Dead-Trigger-2-FPS-Zombie-Game_3.1.6_apkcube.apks" -> "Dead Trigger 2 FPS Zombie Game":
// up to the version (an _ before a digit), dashes, underscores and pluses as spaces
static void name_from_file(const char *file, char *out, size_t n) {
    size_t at = 0;
    for (const char *p = file; *p && at + 1 < n; p++) {
        if (*p == '.' && !strchr(p + 1, '.')) break;              // the extension
        if (*p == '_' && isdigit((unsigned char)p[1]) && at) break; // the version
        out[at++] = *p == '_' || *p == '-' || *p == '+' ? ' ' : *p;
    }
    while (at > 0 && out[at - 1] == ' ') at--;
    out[at] = 0;
}

// "key": "value" in a JSON text (no escapes needed for names)
static int json_string(const char *json, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    size_t at = 0;
    while (*p && *p != '"' && at + 1 < n) {
        if (*p == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2])) { // \uXXXX: dashes, quotes
            char hex[5] = { p[2], p[3], p[4], p[5], 0 };
            unsigned c = (unsigned)strtoul(hex, NULL, 16);
            p += 6;
            out[at++] = c == 0x2013 || c == 0x2014 ? '-' : c == 0x2018 || c == 0x2019 ? '\'' : c < 0x80 ? (char)c : '?';
            continue;
        }
        if (*p == '\\' && p[1]) p++;
        out[at++] = *p++;
    }
    out[at] = 0;
    return at > 0;
}

// A string from the binary XML's string pool
static size_t axml_string(const uint8_t *pool, uint32_t strings, int utf8, uint32_t i, const uint8_t *end, char *out,
                          size_t n) {
    uint32_t off = u32(pool + 28 + i * 4);
    const uint8_t *p = pool + strings + off;
    size_t sl = 0;
    if (p + 4 > end || n == 0) return 0;
    if (utf8) {
        p += (p[0] & 0x80) ? 2 : 1; // its length in characters, then in bytes
        int bytes = p[0] & 0x80 ? ((p[0] & 0x7F) << 8 | p[1]) : p[0];
        p += (p[0] & 0x80) ? 2 : 1;
        sl = (size_t)bytes < n - 1 ? (size_t)bytes : n - 1;
        if (p + sl > end) sl = 0;
        memcpy(out, p, sl);
    } else {
        int chars = u16(p);
        p += 2;
        for (int k = 0; k < chars && sl < n - 1 && p + k * 2 < end; k++) out[sl++] = (char)p[k * 2];
    }
    out[sl] = 0;
    return sl;
}

// An attribute of a binary AndroidManifest.xml's first tag that has it ("package",
// "versionName", "versionCode"): its text (a string), or its number (*num, an int)
int axml_attr(const uint8_t *x, size_t len, const char *name, char *out, size_t n, long *num) {
    if (out && n) out[0] = 0;
    if (len < 8 || u16(x) != 0x0003) return 0;
    const uint8_t *pool = x + 8, *end = x + len;
    if (u16(pool) != 0x0001) return 0;
    uint32_t pool_size = u32(pool + 4), count = u32(pool + 8), flags = u32(pool + 16), strings = u32(pool + 20);
    int utf8 = (flags & 0x100) != 0;
    int idx = -1; // the name's index in the pool, then the start tag whose attribute uses it
    char s[256];
    for (uint32_t i = 0; i < count && i < 4096 && idx < 0; i++) {
        axml_string(pool, strings, utf8, i, end, s, sizeof(s));
        if (!strcmp(s, name)) idx = (int)i;
    }
    if (idx < 0) return 0;
    for (const uint8_t *c = pool + pool_size; c + 8 <= end;) {
        uint32_t csize = u32(c + 4);
        if (csize < 8) break;
        if (u16(c) == 0x0102 && c + 36 <= end) { // a start tag
            uint16_t attr_start = u16(c + 24), attr_size = u16(c + 26), attr_count = u16(c + 28);
            const uint8_t *a = c + 16 + attr_start;
            for (int k = 0; k < attr_count && a + 20 <= end; k++, a += attr_size) {
                if ((int)u32(a + 4) != idx) continue;
                uint32_t raw = u32(a + 8), data = u32(a + 16);
                uint8_t type = a[15];
                if (raw < count && out) return axml_string(pool, strings, utf8, raw, end, out, n) > 0;
                if (type == 0x03 && data < count && out) return axml_string(pool, strings, utf8, data, end, out, n) > 0;
                if (type == 0x01 && num) { // a reference (@mipmap/ic_launcher): its resource id
                    *num = (long)data;
                    if (out) snprintf(out, n, "@%#x", data);
                    return 1;
                }
                if (type >= 0x10 && type <= 0x1f) { // an int
                    if (num) *num = (long)(int32_t)data;
                    if (out) snprintf(out, n, "%d", (int)data);
                    return 1;
                }
                return 0;
            }
        }
        c += csize;
    }
    return 0;
}

static int axml_package(const uint8_t *x, size_t len, char *out, size_t n) {
    return axml_attr(x, len, "package", out, n, NULL);
}

// A <meta-data android:name="key" android:value="..."/> of a binary AndroidManifest.xml (Unreal's
// GameActivity reads ue4.* keys from it: the screen's DPI list, ...): its value as text (an int /
// bool as its number, true = 1) and as a number in *num; 0 if the key isn't there
int axml_meta(const uint8_t *x, size_t len, const char *key, char *out, size_t n, long *num) {
    if (out && n) out[0] = 0;
    if (len < 8 || u16(x) != 0x0003) return 0;
    const uint8_t *pool = x + 8, *end = x + len;
    if (u16(pool) != 0x0001) return 0;
    uint32_t pool_size = u32(pool + 4), count = u32(pool + 8), flags = u32(pool + 16), strings = u32(pool + 20);
    int utf8 = (flags & 0x100) != 0;
    int meta = -1, name = -1, value = -1, resource = -1; // their indexes in the string pool
    char s[512];
    for (uint32_t i = 0; i < count && i < 65536; i++) {
        axml_string(pool, strings, utf8, i, end, s, sizeof(s));
        if (meta < 0 && !strcmp(s, "meta-data")) meta = (int)i;
        else if (name < 0 && !strcmp(s, "name")) name = (int)i;
        else if (value < 0 && !strcmp(s, "value")) value = (int)i;
        else if (resource < 0 && !strcmp(s, "resource")) resource = (int)i;
    }
    if (meta < 0 || name < 0) return 0;
    for (const uint8_t *c = pool + pool_size; c + 8 <= end;) {
        uint32_t csize = u32(c + 4);
        if (csize < 8) break;
        if (u16(c) == 0x0102 && c + 36 <= end && (int)u32(c + 20) == meta) { // <meta-data ...>
            uint16_t attr_start = u16(c + 24), attr_size = u16(c + 26), attr_count = u16(c + 28);
            const uint8_t *a0 = c + 16 + attr_start, *found = NULL, *val = NULL;
            for (int k = 0; k < attr_count && a0 + (k + 1) * attr_size <= end; k++) {
                const uint8_t *a = a0 + k * attr_size;
                int an = (int)u32(a + 4);
                if (an == name) {
                    uint32_t raw = u32(a + 8);
                    char nm[256];
                    if (raw < count && axml_string(pool, strings, utf8, raw, end, nm, sizeof(nm)) && !strcmp(nm, key)) found = a;
                } else if (an == value || an == resource) val = a;
            }
            if (found) {
                if (!val) return 0;
                uint32_t raw = u32(val + 8), data = u32(val + 16);
                uint8_t type = val[15];
                if (raw < count && out) {
                    axml_string(pool, strings, utf8, raw, end, out, n);
                    if (num) *num = strtol(out, NULL, 0);
                    return 1;
                }
                if (type == 0x03 && data < count && out) {
                    axml_string(pool, strings, utf8, data, end, out, n);
                    if (num) *num = strtol(out, NULL, 0);
                    return 1;
                }
                long v = type == 0x12 ? (data ? 1 : 0) : (long)(int32_t)data; // a bool, an int, a reference
                if (num) *num = v;
                if (out) snprintf(out, n, type == 0x12 ? (v ? "true" : "false") : "%ld", v);
                return 1;
            }
        }
        c += csize;
    }
    return 0;
}

// ---- icons ---------------------------------------------------------------------

// A PNG's pixels at ICON x ICON (area-averaged down, or stretched up)
// (transparent margins cut first, square around the picture: a small icon fills its tile too)
static uint8_t *icon_from_png(const uint8_t *png, size_t len) {
    int w, h, ch;
    uint8_t *img = stbi_load_from_memory(png, (int)len, &w, &h, &ch, 4);
    if (!img) return NULL;
    int bx0 = w, by0 = h, bx1 = -1, by1 = -1; // the visible part
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (img[(y * w + x) * 4 + 3] > 16) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
    int ox = 0, oy = 0, sw = w, sh = h;
    if (bx1 >= 0 && (bx1 - bx0 + 1) * 10 < w * 9 && (by1 - by0 + 1) * 10 < h * 9) { // margins of 10% or more
        const int side = bx1 - bx0 + 1 > by1 - by0 + 1 ? bx1 - bx0 + 1 : by1 - by0 + 1;
        int cx = (bx0 + bx1 + 1) / 2 - side / 2, cy = (by0 + by1 + 1) / 2 - side / 2;
        if (cx + side > w) cx = w - side;
        if (cy + side > h) cy = h - side;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (side <= w && side <= h) ox = cx, oy = cy, sw = sh = side;
    }
    uint8_t *out = malloc(ICON * ICON * 4);
    for (int y = 0; out && y < ICON; y++)
        for (int x = 0; x < ICON; x++) {
            int x0 = ox + x * sw / ICON, x1 = ox + (x + 1) * sw / ICON, y0 = oy + y * sh / ICON, y1 = oy + (y + 1) * sh / ICON;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            unsigned sum[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++, cnt++)
                    for (int c = 0; c < 4; c++) sum[c] += img[(yy * w + xx) * 4 + c];
            for (int c = 0; c < 4; c++) out[(y * ICON + x) * 4 + c] = (uint8_t)(cnt ? sum[c] / cnt : 0);
        }
    stbi_image_free(img);
    return out;
}

// a PNG / JPEG icon: RGBA, LIBRARY_ICON square
uint8_t *library_icon_decode(const uint8_t *data, size_t len) { return icon_from_png(data, len); }

// a picture as RGBA, shrunk to fit max_w x max_h (area-averaged), its shape kept
uint8_t *library_image_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *ow, int *oh) {
    int w, h, ch;
    uint8_t *img = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    if (!img) return NULL;
    float k = 1;
    if (w > max_w) k = (float)max_w / w;
    if (h * k > max_h) k = (float)max_h / h;
    const int W = (int)(w * k) > 0 ? (int)(w * k) : 1, H = (int)(h * k) > 0 ? (int)(h * k) : 1;
    uint8_t *out = malloc((size_t)W * H * 4);
    for (int y = 0; out && y < H; y++)
        for (int x = 0; x < W; x++) {
            int x0 = x * w / W, x1 = (x + 1) * w / W, y0 = y * h / H, y1 = (y + 1) * h / H;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            unsigned sum[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++, cnt++)
                    for (int c = 0; c < 4; c++) sum[c] += img[((size_t)yy * w + xx) * 4 + c];
            for (int c = 0; c < 4; c++) out[((size_t)y * W + x) * 4 + c] = (uint8_t)(cnt ? sum[c] / cnt : 0);
        }
    stbi_image_free(img);
    *ow = W, *oh = H;
    return out;
}

// The best launcher icon in an APK: a mipmap/drawable PNG named like one, densest first
typedef struct {
    const uint8_t *rec;
    int score;
} icon_pick;
static int icon_visit(const char *name, int len, const uint8_t *rec, void *ctx) {
    icon_pick *p = ctx;
    char s[256];
    if (len >= (int)sizeof(s) || len < 9 || strncmp(name, "res/", 4) != 0) return 0;
    memcpy(s, name, (size_t)len);
    s[len] = 0;
    if (!strstr(s, ".png") || strstr(s, ".9.png")) return 0;
    const char *base = strrchr(s, '/') + 1;
    int named = strstr(base, "ic_launcher") ? 40 : strstr(base, "app_icon") ? 40 : !strncmp(base, "icon", 4) ? 35
              : strstr(base, "launcher") ? 30 : strstr(base, "icon") ? 20 : 0;
    if (!named || strstr(base, "background") || strstr(base, "monochrome")) return 0;
    int dens = strstr(s, "xxxhdpi") ? 5 : strstr(s, "xxhdpi") ? 4 : strstr(s, "xhdpi") ? 3 : strstr(s, "hdpi") ? 2 : 1;
    int score = named + dens * 2 + (strstr(s, "mipmap") ? 3 : 0) - (strstr(base, "foreground") ? 1 : 0);
    if (score > p->score) {
        p->score = score;
        p->rec = rec;
    }
    return 0;
}

// ---- one game ------------------------------------------------------------------

static int ends_with(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && strcasecmp(s + a - b, end) == 0;
}

static void sanitize(char *s) {
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '_' && *s != '-') *s = '_';
}

// the saved name / engine / icon of a game whose file hasn't changed
static int load_cached(lib_game *g) {
    char p[400];
    snprintf(p, sizeof(p), "%s/game-info.txt", g->dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[300];
    long long bytes = -1;
    int icons = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "bytes=", 6)) bytes = atoll(line + 6);
        else if (!strncmp(line, "name=", 5)) snprintf(g->name, sizeof(g->name), "%s", line + 5);
        else if (!strncmp(line, "id=", 3)) snprintf(g->id, sizeof(g->id), "%s", line + 3);
        else if (!strncmp(line, "icons=", 6)) icons = atoi(line + 6);
    }
    fclose(f);
    if (bytes != g->bytes || icons != ICON_RULES) return 0; // (new icon rules: read again)
    snprintf(p, sizeof(p), "%s/icon.rgba", g->dir);
    f = fopen(p, "rb");
    if (f) {
        g->icon = malloc(ICON * ICON * 4);
        if (g->icon && fread(g->icon, 1, ICON * ICON * 4, f) == ICON * ICON * 4) g->icon_size = ICON;
        else {
            free(g->icon);
            g->icon = NULL;
        }
        fclose(f);
    }
    return g->icon != NULL; // no icon saved: read the game again (the icon search got better)
}

static void save_cached(const lib_game *g) {
    char p[400];
    snprintf(p, sizeof(p), "%s/game-info.txt", g->dir);
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "bytes=%lld\nname=%s\nid=%s\nicons=%d\n", g->bytes, g->name, g->id, ICON_RULES);
        fclose(f);
    }
    if (g->icon) {
        snprintf(p, sizeof(p), "%s/icon.rgba", g->dir);
        f = fopen(p, "wb");
        if (f) {
            fwrite(g->icon, 1, ICON * ICON * 4, f);
            fclose(f);
        }
    }
}

// ---- the icon as Android finds it: the manifest's android:icon (a resource id), its files
// in resources.arsc (one per screen density), the densest PNG / JPEG. Needed when an APK's
// resource names are shrunk (Shadow Fight 3: res/a1.png, nothing named like an icon).
// A string of a string pool chunk (UTF-8 or UTF-16), into out
static int arsc_string(const uint8_t *pool, const uint8_t *end, uint32_t i, char *out, size_t n) {
    out[0] = 0;
    if (pool + 28 > end || u16(pool) != 0x0001) return 0;
    uint32_t count = u32(pool + 8), flags = u32(pool + 16), start = u32(pool + 20);
    uint16_t hsize = u16(pool + 2);
    if (i >= count || pool + hsize + (i + 1) * 4 > end) return 0;
    const uint8_t *str = pool + start + u32(pool + hsize + i * 4);
    if (str + 4 > end) return 0;
    size_t k = 0;
    if (flags & 0x100) { // UTF-8: its UTF-16 length, its byte length, the bytes
        str += (str[0] & 0x80) ? 2 : 1;
        size_t len = str[0];
        if (str[0] & 0x80) len = ((size_t)(str[0] & 0x7f) << 8) | str[1], str++;
        str++;
        for (; k < len && k + 1 < n && str + k < end; k++) out[k] = (char)str[k];
    } else { // UTF-16 (the ASCII of it)
        size_t len = u16(str);
        str += 2;
        if (len & 0x8000) len = ((len & 0x7fff) << 16) | u16(str), str += 2;
        for (; k < len && k + 1 < n && str + k * 2 + 1 < end; k++) out[k] = (char)str[k * 2];
    }
    out[k] = 0;
    return (int)k;
}

// resource id's files: the densest that is a PNG / JPEG, into path
// id's densest image (path), and when there's one its anydpi XML (xml: an <adaptive-icon>) or its
// color (*color, ARGB: a color resource; 0 none). 1: something found
static int arsc_res(const uint8_t *t, size_t len, uint32_t id, char *path, size_t n, char *xml, size_t xn,
                    uint32_t *color) {
    const uint8_t *end = t + len;
    path[0] = 0;
    if (xml && xn) xml[0] = 0;
    if (color) *color = 0;
    if (len < 12 || u16(t) != 0x0002) return 0;
    const uint8_t *strings = NULL;
    int best = -1;
    unsigned want_pkg = id >> 24, want_type = (id >> 16) & 0xff, want_entry = id & 0xffff;
    for (const uint8_t *c = t + u16(t + 2); c + 8 <= end;) {
        uint32_t csize = u32(c + 4);
        if (csize < 8 || c + csize > end) break;
        if (u16(c) == 0x0001 && !strings) strings = c; // the values' strings
        if (u16(c) == 0x0200 && u32(c + 8) == want_pkg) { // our package: its types
            const uint8_t *pend = c + csize;
            for (const uint8_t *k = c + u16(c + 2); k + 8 <= pend;) {
                uint32_t ksize = u32(k + 4);
                if (ksize < 8 || k + ksize > pend) break;
                if (u16(k) == 0x0201 && k[8] == want_type) { // a ResTable_type of that type
                    uint8_t flags = k[9];
                    uint32_t count = u32(k + 12), entries = u32(k + 16);
                    uint16_t hsize = u16(k + 2), density = u16(k + 20 + 14); // its config's density
                    const uint8_t *offs = k + hsize;
                    uint32_t off = 0xffffffff;
                    if (flags & 0x01) { // sparse: (index, offset / 4) pairs
                        for (uint32_t e = 0; e < count && offs + e * 4 + 4 <= pend; e++)
                            if (u16(offs + e * 4) == want_entry) off = (uint32_t)u16(offs + e * 4 + 2) * 4;
                    } else if (flags & 0x02) { // 16-bit offsets, / 4
                        if (want_entry < count && offs + want_entry * 2 + 2 <= pend) {
                            uint16_t o = u16(offs + want_entry * 2);
                            if (o != 0xffff) off = (uint32_t)o * 4;
                        }
                    } else if (want_entry < count && offs + want_entry * 4 + 4 <= pend) {
                        off = u32(offs + want_entry * 4);
                    }
                    const uint8_t *e = k + entries + off;
                    if (off != 0xffffffff && e + 8 <= pend) {
                        uint16_t esize = u16(e), eflags = u16(e + 2);
                        uint8_t vtype = 0;
                        uint32_t vdata = 0;
                        if (eflags & 0x08) { // compact: the value right in the entry
                            vtype = (uint8_t)(eflags >> 8);
                            vdata = u32(e + 4);
                        } else if (!(eflags & 0x01) && e + esize + 8 <= pend) { // simple: a Res_value after it
                            vtype = e[esize + 3];
                            vdata = u32(e + esize + 4);
                        }
                        char file[256];
                        if (vtype == 0x03 && strings && arsc_string(strings, end, vdata, file, sizeof(file))) {
                            const char *dot = strrchr(file, '.');
                            int image = dot && (!strcmp(dot, ".png") || !strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg"));
                            int d = density >= 0xfffe ? 0 : density ? density : 160; // anydpi: not an image
                            if (image && d > best) {
                                best = d;
                                snprintf(path, n, "%s", file);
                            }
                            if (dot && !strcmp(dot, ".xml") && xml && xn) snprintf(xml, xn, "%s", file);
                        } else if (vtype >= 0x1c && vtype <= 0x1f && color) { // a color (#argb, #rgb...)
                            *color = vtype == 0x1d || vtype == 0x1f ? vdata | 0xff000000u : vdata;
                        }
                    }
                }
                k += ksize;
            }
        }
        c += csize;
    }
    return path[0] != 0 || (xml && xml[0]) || (color && *color);
}

static int arsc_icon_path(const uint8_t *t, size_t len, uint32_t id, char *path, size_t n) {
    return arsc_res(t, len, id, path, n, NULL, 0, NULL) && path[0];
}

// A binary XML's <tag attr="@ref"/>: the first such tag's reference (an <adaptive-icon>'s layers)
static int axml_tag_ref(const uint8_t *x, size_t len, const char *tag, const char *attr, uint32_t *ref) {
    if (len < 8 || u16(x) != 0x0003) return 0;
    const uint8_t *pool = x + 8, *end = x + len;
    if (u16(pool) != 0x0001) return 0;
    uint32_t pool_size = u32(pool + 4), count = u32(pool + 8), flags = u32(pool + 16), strings = u32(pool + 20);
    int utf8 = (flags & 0x100) != 0, ti = -1, ai = -1;
    char s[128];
    for (uint32_t i = 0; i < count && i < 4096; i++) {
        axml_string(pool, strings, utf8, i, end, s, sizeof(s));
        if (ti < 0 && !strcmp(s, tag)) ti = (int)i;
        if (ai < 0 && !strcmp(s, attr)) ai = (int)i;
    }
    if (ti < 0 || ai < 0) return 0;
    for (const uint8_t *c = pool + pool_size; c + 8 <= end;) {
        uint32_t csize = u32(c + 4);
        if (csize < 8) break;
        if (u16(c) == 0x0102 && c + 36 <= end && (int)u32(c + 20) == ti) { // <tag ...>
            uint16_t attr_start = u16(c + 24), attr_size = u16(c + 26), attr_count = u16(c + 28);
            const uint8_t *a = c + 16 + attr_start;
            for (int k = 0; k < attr_count && a + 20 <= end; k++, a += attr_size)
                if ((int)u32(a + 4) == ai && a[15] == 0x01) {
                    *ref = u32(a + 16);
                    return 1;
                }
        }
        c += csize;
    }
    return 0;
}

// A layer of an adaptive icon at ICON x ICON: the middle 72 of its 108 units (what a home screen's
// shape shows), from a picture, or a color (the background); NULL if neither
static uint8_t *icon_layer(zip *z, const uint8_t *arsc, size_t alen, uint32_t ref) {
    char path[256];
    uint32_t color = 0;
    if (!arsc_res(arsc, alen, ref, path, sizeof(path), NULL, 0, &color)) return NULL;
    uint8_t *out = malloc(ICON * ICON * 4);
    if (!out) return NULL;
    if (!path[0]) { // a color
        if (!color) {
            free(out);
            return NULL;
        }
        for (int i = 0; i < ICON * ICON; i++) {
            out[i * 4] = (uint8_t)(color >> 16), out[i * 4 + 1] = (uint8_t)(color >> 8);
            out[i * 4 + 2] = (uint8_t)color, out[i * 4 + 3] = (uint8_t)(color >> 24);
        }
        return out;
    }
    size_t plen = 0;
    uint8_t *png = zip_get(z, path, 8 << 20, &plen);
    int w = 0, h = 0, ch;
    uint8_t *img = png ? stbi_load_from_memory(png, (int)plen, &w, &h, &ch, 4) : NULL;
    free(png);
    if (!img) {
        free(out);
        return NULL;
    }
    const double x0 = w / 6.0, y0 = h / 6.0, cw = w * 4 / 6.0, chh = h * 4 / 6.0; // the middle 72 / 108
    for (int y = 0; y < ICON; y++)
        for (int x = 0; x < ICON; x++) {
            const int sx0 = (int)(x0 + x * cw / ICON), sx1 = (int)(x0 + (x + 1) * cw / ICON);
            const int sy0 = (int)(y0 + y * chh / ICON), sy1 = (int)(y0 + (y + 1) * chh / ICON);
            unsigned sum[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int yy = sy0; yy <= sy1 && yy < h; yy++)
                for (int xx = sx0; xx <= sx1 && xx < w; xx++, cnt++)
                    for (int c = 0; c < 4; c++) sum[c] += img[(yy * w + xx) * 4 + c];
            for (int c = 0; c < 4; c++) out[(y * ICON + x) * 4 + c] = (uint8_t)(cnt ? sum[c] / cnt : 0);
        }
    stbi_image_free(img);
    return out;
}

// The manifest's icon as Android 8+ draws it, when it's an <adaptive-icon> with picture layers:
// its foreground over its background (Subway Surfers' old-style icon is that art with a grey
// frame drawn in: small in its tile). NULL: not adaptive, or a layer we can't draw (vectors)
static uint8_t *adaptive_icon(zip *z, const uint8_t *arsc, size_t alen, const char *xml_path) {
    size_t xlen = 0;
    uint8_t *xml = zip_get(z, xml_path, 1 << 20, &xlen);
    uint32_t bg_ref = 0, fg_ref = 0;
    const int ok = xml && axml_tag_ref(xml, xlen, "background", "drawable", &bg_ref) &&
                   axml_tag_ref(xml, xlen, "foreground", "drawable", &fg_ref);
    free(xml);
    if (!ok) return NULL;
    uint8_t *bg = icon_layer(z, arsc, alen, bg_ref), *fg = icon_layer(z, arsc, alen, fg_ref);
    if (!bg || !fg) {
        free(bg);
        free(fg);
        return NULL;
    }
    for (int i = 0; i < ICON * ICON; i++) { // fg over bg
        const unsigned a = fg[i * 4 + 3];
        for (int c = 0; c < 3; c++) bg[i * 4 + c] = (uint8_t)((fg[i * 4 + c] * a + bg[i * 4 + c] * (255 - a)) / 255);
        bg[i * 4 + 3] = (uint8_t)(a + bg[i * 4 + 3] * (255 - a) / 255);
    }
    free(fg);
    return bg;
}

// the manifest's android:icon, found through resources.arsc: its pixels, or NULL
static uint8_t *arsc_icon(zip *z) {
    size_t mlen = 0, alen = 0, plen = 0;
    uint8_t *man = zip_get(z, "AndroidManifest.xml", 4 << 20, &mlen), *icon = NULL;
    long id = 0;
    if (man && axml_attr(man, mlen, "icon", NULL, 0, &id) && (id >> 24) == 0x7f) {
        uint8_t *arsc = zip_get(z, "resources.arsc", 64 << 20, &alen);
        char path[256], xml[256];
        uint32_t color;
        if (arsc && arsc_res(arsc, alen, (uint32_t)id, path, sizeof(path), xml, sizeof(xml), &color)) {
            if (xml[0] && (icon = adaptive_icon(z, arsc, alen, xml)))
                droid_log("library: icon %#lx is %s (adaptive: its layers)", id, xml);
            if (!icon && path[0]) {
                uint8_t *png = zip_get(z, path, 8 << 20, &plen);
                if (png) icon = icon_from_png(png, plen);
                droid_log("library: icon %#lx is %s%s", id, path, icon ? "" : " (not readable)");
                free(png);
            }
        }
        free(arsc);
    }
    free(man);
    return icon;
}

// an APK's launcher icon: the manifest's (through resources.arsc), else its densest
// mipmap/drawable PNG named like one
static uint8_t *apk_icon(zip *z) {
    uint8_t *found = arsc_icon(z);
    if (found) return found;
    icon_pick pick = { NULL, 0 };
    zip_each(z, icon_visit, &pick);
    if (!pick.rec) return NULL;
    size_t len = 0;
    uint8_t *png = zip_read(z, pick.rec, 8 << 20, &len), *icon = NULL;
    if (png) {
        icon = icon_from_png(png, len);
        free(png);
    }
    return icon;
}

static void read_game(lib_game *g) {
    zip z;
    if (zip_open(&z, g->path) != 0) return;
    size_t len = 0;
    if (ends_with(g->path, ".xapk") || ends_with(g->path, ".apks") || ends_with(g->path, ".apkm")) {
        char *json = (char *)zip_get(&z, "manifest.json", 1 << 20, &len);
        if (json) {
            json_string(json, "name", g->name, sizeof(g->name));
            json_string(json, "package_name", g->id, sizeof(g->id));
            free(json);
        } else if ((json = (char *)zip_get(&z, "info.json", 1 << 20, &len))) { // .apkm (APKMirror)
            json_string(json, "app_name", g->name, sizeof(g->name));
            json_string(json, "pname", g->id, sizeof(g->id));
            free(json);
        }
        uint8_t *png = zip_get(&z, "icon.png", 8 << 20, &len);
        if (png) {
            g->icon = icon_from_png(png, len);
            free(png);
        }
        // no usable icon.png (Wuthering Waves': empty) or no manifest (apkcube's .apks): the
        // main APK's own package and icon, read in place (bundles store their APKs uncompressed)
        if (!g->icon || !g->id[0]) {
            char inner[160];
            snprintf(inner, sizeof(inner), "%s.apk", g->id);
            find_ctx c = { inner, NULL };
            if (g->id[0]) zip_each(&z, find_visit, &c);
            if (!c.rec) {
                c.want = "base.apk";
                zip_each(&z, find_visit, &c);
            }
            if (c.rec && u16(c.rec + 10) == 0) {
                uint8_t lh[30];
                long local = (long)u32(c.rec + 42);
                fseek(z.f, local, SEEK_SET);
                if (fread(lh, 1, 30, z.f) == 30 && u32(lh) == 0x04034b50) {
                    zip a;
                    if (zip_open_at(&a, g->path, local + 30 + u16(lh + 26) + u16(lh + 28), (long)u32(c.rec + 24)) == 0) {
                        if (!g->id[0]) {
                            size_t xl = 0;
                            uint8_t *x = zip_get(&a, "AndroidManifest.xml", 4 << 20, &xl);
                            if (x) {
                                axml_package(x, xl, g->id, sizeof(g->id));
                                free(x);
                            }
                        }
                        if (!g->icon) g->icon = apk_icon(&a);
                        zip_close(&a);
                    }
                }
            }
            // compressed inside (apkcube's): the copy unpacked at its first launch
            if (!g->icon || !g->id[0]) {
                char path[400];
                snprintf(path, sizeof(path), "%s/apk/base.apk", g->dir);
                zip a;
                if (zip_open(&a, path) == 0) {
                    if (!g->id[0]) {
                        size_t xl = 0;
                        uint8_t *x = zip_get(&a, "AndroidManifest.xml", 4 << 20, &xl);
                        if (x) {
                            axml_package(x, xl, g->id, sizeof(g->id));
                            free(x);
                        }
                    }
                    if (!g->icon) g->icon = apk_icon(&a);
                    zip_close(&a);
                }
            }
        }
    } else {
        uint8_t *x = zip_get(&z, "AndroidManifest.xml", 4 << 20, &len);
        if (x) {
            axml_package(x, len, g->id, sizeof(g->id));
            free(x);
        }
        g->icon = apk_icon(&z);
    }
    zip_close(&z);
    if (g->icon) g->icon_size = ICON;
}

// a game file's folder key: by file name (the package isn't known before it's read)
static void folder_key(const char *path, char *key, size_t kn) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(key, kn, "%s", base);
    sanitize(key);
    // a very long file name (APKMirror's, 140 characters): its first 48 and a fingerprint of
    // the whole name, so the paths inside stay short (Hitman's folder name was cut off and
    // its libraries' paths with it). Shorter names keep their folders (and saves) as before.
    if (strlen(base) > 64 && kn > 60) {
        uint32_t h = 2166136261u;
        for (const char *c = base; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        snprintf(key + 48, kn - 48, "-%08x", h);
    }
}

void library_dir_for(const char *path, char *dir, size_t n) {
    char key[128];
    folder_key(path, key, sizeof(key));
    snprintf(dir, n, APP0 "/data/games/%s", key);
}

// the version (versionCode) in a game's file: an .xapk's manifest.json, else its (main) APK's manifest
static long long manifest_vc(zip *z) {
    size_t xl = 0;
    uint8_t *x = zip_get(z, "AndroidManifest.xml", 4 << 20, &xl);
    long num = 0;
    char txt[32];
    const long long vc = x && axml_attr(x, xl, "versionCode", txt, sizeof(txt), &num) ? (txt[0] ? strtoll(txt, NULL, 10) : num) : 0;
    free(x);
    return vc;
}
long long library_version_code(const char *path) {
    zip z;
    if (zip_open(&z, path) != 0) return 0;
    long long vc = 0;
    size_t len = 0;
    char *json = (char *)zip_get(&z, "manifest.json", 1 << 20, &len);
    if (json) { // an .xapk: "version_code": 96919
        const char *k = strstr(json, "\"version_code\"");
        if (k && (k = strchr(k, ':'))) {
            while (*++k == ' ' || *k == '"') {
            }
            vc = strtoll(k, NULL, 10);
        }
        free(json);
    }
    if (!vc && ends_with(path, ".apk")) vc = manifest_vc(&z);
    if (!vc) { // a bundle: its base APK (stored: read in place)
        find_ctx c = { "base.apk", NULL };
        zip_each(&z, find_visit, &c);
        if (c.rec && u16(c.rec + 10) == 0) {
            uint8_t lh[30];
            const long local = (long)u32(c.rec + 42);
            fseek(z.f, local, SEEK_SET);
            zip a;
            if (fread(lh, 1, 30, z.f) == 30 && u32(lh) == 0x04034b50 &&
                zip_open_at(&a, path, local + 30 + u16(lh + 26) + u16(lh + 28), (long)u32(c.rec + 24)) == 0) {
                vc = manifest_vc(&a);
                zip_close(&a);
            }
        }
    }
    zip_close(&z);
    return vc;
}

static int add_game(lib_game *games, int n, int max, const char *path, const char *dir_override) {
    if (n >= max) return n;
    struct stat st;
    if (stat(path, &st) != 0) return n;
    lib_game *g = &games[n];
    memset(g, 0, sizeof(*g));
    snprintf(g->path, sizeof(g->path), "%s", path);
    g->bytes = (long long)st.st_size;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char key[128];
    folder_key(path, key, sizeof(key));
    if (dir_override) snprintf(g->dir, sizeof(g->dir), "%s", dir_override);
    else snprintf(g->dir, sizeof(g->dir), APP0 "/data/games/%s", key);
    mkdir(APP0 "/data", 0777);
    mkdir(APP0 "/data/games", 0777);
    mkdir(g->dir, 0777);
    if (!load_cached(g)) {
        g->name[0] = g->id[0] = 0; // the cached ones (if any) are read again
        read_game(g);
        if (!g->name[0] && !g->id[0]) name_from_file(base, g->name, sizeof(g->name));
        if (!g->id[0]) snprintf(g->id, sizeof(g->id), "%s", key);
        if (!g->name[0]) name_from_package(g->id, g->name, sizeof(g->name));
        else if (!strchr(g->name, ' ')) { // "TheEndlessTunnel" -> "The Endless Tunnel"
            char spaced[128];
            name_from_package(g->name, spaced, sizeof(spaced));
            snprintf(g->name, sizeof(g->name), "%s", spaced);
        }
        save_cached(g);
        droid_log("library: %s: \"%s\" (%s), icon %s", base, g->name, g->id, g->icon ? "found" : "none");
    }
    char p[400];
    snprintf(p, sizeof(p), "%s/engine.txt", g->dir); // written once the game ran
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(g->engine, sizeof(g->engine), f)) g->engine[strcspn(g->engine, "\r\n")] = 0;
        fclose(f);
    }
    return n + 1;
}

// the game files (.apk, .xapk, .apks, .apkm) of a folder, added after the n there
static int scan_folder(const char *folder, lib_game *games, int n, int max) {
    DIR *d = opendir(folder);
    if (!d) {
        mkdir(folder, 0777);
        return n;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!ends_with(e->d_name, ".apk") && !ends_with(e->d_name, ".xapk") && !ends_with(e->d_name, ".apks") &&
            !ends_with(e->d_name, ".apkm"))
            continue;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
        n = add_game(games, n, max, path, NULL);
    }
    closedir(d);
    return n;
}

int library_scan_in(const char *folder, lib_game *games, int max) {
    const int n = scan_folder(folder, games, 0, max);
    droid_log("game: %d game file(s) in %s", n, folder);
    return n;
}

void library_free(lib_game *games, int n) {
    for (int i = 0; i < n; i++) {
        free(games[i].icon);
        games[i].icon = NULL;
    }
}
