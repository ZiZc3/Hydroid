// texspy.c — TexSpy (tools.txt "TexSpy"), the texture tool: every texture a game uploads, logged
// as "texspy:" lines - call, texture id, target, level, format, size, bytes - with a look at its
// data (uncompressed: how much isn't black / is see-through; compressed: how much isn't zero), so a
// texture that comes out black can be told from one that went in black. "TexSpyDump" as well:
// textures of 256x256 and up (uncompressed RGBA / RGB) saved as <game>/texspy/<id>_<w>x<h>_l<level>.ppm.
// Hooked where the game's GL names are resolved (android_shim.c, eglGetProcAddress), any GL path.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "game_settings.h"

void droid_log(const char *fmt, ...);
void *egl_resolve(const char *name);
const char *game_path(char *out, size_t n, const char *rel); // game_settings.c

typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
#define T_TEXTURE_BINDING_2D 0x8069
#define T_TEXTURE_BINDING_CUBE 0x8514
#define T_RGBA 0x1908
#define T_RGB 0x1907
#define T_UNSIGNED_BYTE 0x1401

typedef void (*tex_image_fn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*tex_sub_fn)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*ctex_image_fn)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (*ctex_sub_fn)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
typedef void (*tex_storage_fn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*get_int_fn)(GLenum, GLint *);

static tex_image_fn real_tex_image;
static tex_sub_fn real_tex_sub;
static ctex_image_fn real_ctex_image;
static ctex_sub_fn real_ctex_sub;
static tex_storage_fn real_tex_storage;
static get_int_fn get_int;

static int lines;
#define MAX_LINES 1500

static GLint bound(GLenum target) {
    if (!get_int) get_int = (get_int_fn)egl_resolve("glGetIntegerv");
    GLint id = -1;
    if (get_int) get_int(target >= 0x8515 && target <= 0x851A ? T_TEXTURE_BINDING_CUBE : T_TEXTURE_BINDING_2D, &id);
    return id;
}

// uncompressed RGBA8 / RGB8: % not black, % fully see-through
static void look_pixels(const unsigned char *p, GLsizei w, GLsizei h, GLenum format, GLenum type, char *out, size_t n) {
    if (!p) { snprintf(out, n, "no data"); return; }
    if (type != T_UNSIGNED_BYTE || (format != T_RGBA && format != T_RGB)) { snprintf(out, n, "(format %#x type %#x)", format, type); return; }
    const int c = format == T_RGBA ? 4 : 3;
    const long count = (long)w * h;
    long lit = 0, clear = 0;
    for (long i = 0; i < count; i++) {
        const unsigned char *q = p + i * c;
        if (q[0] + q[1] + q[2] > 12) lit++;
        if (c == 4 && q[3] == 0) clear++;
    }
    snprintf(out, n, "%ld%% not black, %ld%% see-through%s", count ? lit * 100 / count : 0,
             count ? clear * 100 / count : 0, count && lit * 100 / count < 1 ? "  << ALL BLACK" : "");
}

static void look_bytes(const unsigned char *p, GLsizei n, char *out, size_t on) {
    if (!p || n <= 0) { snprintf(out, on, "no data"); return; }
    long nz = 0;
    for (GLsizei i = 0; i < n; i++) nz += p[i] != 0;
    snprintf(out, on, "%ld%% non-zero bytes%s", nz * 100 / n, nz * 100 / n < 2 ? "  << ALL ZERO" : "");
}

static void dump(GLint id, GLint level, GLsizei w, GLsizei h, GLenum format, GLenum type, const unsigned char *p) {
    static int on = -1, dumped;
    if (on < 0) on = droid_tool("TexSpyDump") ? 1 : 0;
    if (!on || !p || w < 256 || h < 256 || type != T_UNSIGNED_BYTE || dumped >= 300) return;
    if (format != T_RGBA && format != T_RGB) return;
    char dir[600], path[700];
    game_path(dir, sizeof(dir), "texspy");
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%d_%dx%d_l%d.ppm", dir, id, w, h, level);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    dumped++;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const int c = format == T_RGBA ? 4 : 3;
    for (GLsizei y = h - 1; y >= 0; y--) // GL's rows go up
        for (GLsizei x = 0; x < w; x++) fwrite(p + ((long)y * w + x) * c, 1, 3, f);
    fclose(f);
}

static void ts_tex_image(GLenum t, GLint l, GLint internal, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p) {
    real_tex_image(t, l, internal, w, h, b, f, ty, p);
    if (lines >= MAX_LINES) return;
    char look[96];
    look_pixels(p, w, h, f, ty, look, sizeof(look));
    const GLint id = bound(t);
    lines++;
    droid_log("texspy: TexImage2D tex %d target %#x level %d internal %#x %dx%d: %s", id, t, l, internal, w, h, look);
    dump(id, l, w, h, f, ty, p);
}
static void ts_tex_sub(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p) {
    real_tex_sub(t, l, x, y, w, h, f, ty, p);
    if (lines >= MAX_LINES) return;
    char look[96];
    look_pixels(p, w, h, f, ty, look, sizeof(look));
    const GLint id = bound(t);
    lines++;
    droid_log("texspy: TexSubImage2D tex %d target %#x level %d at %d,%d %dx%d: %s", id, t, l, x, y, w, h, look);
    if (x == 0 && y == 0) dump(id, l, w, h, f, ty, p);
}
static void ts_ctex_image(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void *p) {
    real_ctex_image(t, l, f, w, h, b, n, p);
    if (lines >= MAX_LINES) return;
    char look[64];
    look_bytes(p, n, look, sizeof(look));
    lines++;
    droid_log("texspy: CompressedTexImage2D tex %d target %#x level %d format %#x %dx%d %d bytes: %s", bound(t), t, l, f, w,
              h, n, look);
}
static void ts_ctex_sub(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei n, const void *p) {
    real_ctex_sub(t, l, x, y, w, h, f, n, p);
    if (lines >= MAX_LINES) return;
    char look[64];
    look_bytes(p, n, look, sizeof(look));
    lines++;
    droid_log("texspy: CompressedTexSubImage2D tex %d target %#x level %d at %d,%d format %#x %dx%d %d bytes: %s", bound(t),
              t, l, x, y, f, w, h, n, look);
}
static void ts_tex_storage(GLenum t, GLsizei levels, GLenum f, GLsizei w, GLsizei h) {
    real_tex_storage(t, levels, f, w, h);
    if (lines >= MAX_LINES) return;
    lines++;
    droid_log("texspy: TexStorage2D tex %d target %#x %d level(s) format %#x %dx%d", bound(t), t, levels, f, w, h);
}

// the game's GL name and what it would get: ours instead while TexSpy is on
void *texspy_wrap(const char *name, void *real) {
    static int on = -1;
    if (on < 0) on = droid_tool("TexSpy") ? 1 : 0;
    if (!on || !real || !name) return real;
    if (!strcmp(name, "glTexImage2D")) { real_tex_image = (tex_image_fn)real; return (void *)ts_tex_image; }
    if (!strcmp(name, "glTexSubImage2D")) { real_tex_sub = (tex_sub_fn)real; return (void *)ts_tex_sub; }
    if (!strcmp(name, "glCompressedTexImage2D")) { real_ctex_image = (ctex_image_fn)real; return (void *)ts_ctex_image; }
    if (!strcmp(name, "glCompressedTexSubImage2D")) { real_ctex_sub = (ctex_sub_fn)real; return (void *)ts_ctex_sub; }
    if (!strcmp(name, "glTexStorage2D") || !strcmp(name, "glTexStorage2DEXT")) {
        real_tex_storage = (tex_storage_fn)real;
        return (void *)ts_tex_storage;
    }
    return real;
}
