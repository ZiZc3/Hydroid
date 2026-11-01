// phone_gl.c — the phone view for OpenGL ES games: the game is told its screen is a phone's
// 1080x1920 (view_decide), so its "screen" (framebuffer 0) becomes a framebuffer of ours at
// that size, and each eglSwapBuffers copies it into the middle of the real one at its full
// height, black at the sides (glBlitFramebuffer, linear). Put between the game and every GL
// path where its GL names are resolved (android_shim.c, the eglGetProcAddress of each), as
// TexSpy is; off (the game's own functions) when the game has the TV's screen.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game_settings.h"

void droid_log(const char *fmt, ...);
int gl_real_screen(int *w, int *h) __attribute__((weak)); // gles_bridge.c (the PS5; not on the PC)

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define FRAMEBUFFER 0x8D40
#define READ_FB 0x8CA8
#define DRAW_FB 0x8CA9
#define FB_BINDING 0x8CA6 // = DRAW_FRAMEBUFFER_BINDING
#define READ_FB_BINDING 0x8CAA
#define RENDERBUFFER 0x8D41
#define RGBA8 0x8058
#define DEPTH24_STENCIL8 0x88F0
#define COLOR_ATTACHMENT0 0x8CE0
#define DEPTH_STENCIL_ATTACHMENT 0x821A
#define DEPTH_ATTACHMENT 0x8D00
#define STENCIL_ATTACHMENT 0x8D20
#define G_COLOR 0x1800 // the default framebuffer's attachment names
#define G_DEPTH 0x1801
#define G_STENCIL 0x1802
#define G_BACK 0x0405
#define SCISSOR_TEST 0x0C11
#define COLOR_CLEAR_VALUE 0x0C22
#define COLOR_WRITEMASK 0x0C23
#define COLOR_BUFFER_BIT 0x4000
#define LINEAR 0x2601
#define FB_COMPLETE 0x8CD5
#define TEXTURE_2D 0x0DE1
#define TEX_MIN_FILTER 0x2801
#define TEX_MAG_FILTER 0x2800
#define RGBA 0x1908
#define UNSIGNED_BYTE 0x1401
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056

// the real functions: the game's own (what its names resolve to without us)
static void (*r_bind_fb)(GLenum, GLuint);
static void (*r_get_int)(GLenum, GLint *);
static void (*r_invalidate)(GLenum, GLsizei, const GLenum *);
static void (*r_discard)(GLenum, GLsizei, const GLenum *);
static void (*r_draw_buffers)(GLsizei, const GLenum *);
static void (*r_read_buffer)(GLenum);
static void (*r_attach_param)(GLenum, GLenum, GLenum, GLint *);
static EGLBoolean (*r_swap)(EGLDisplay, EGLSurface);
static EGLBoolean (*r_make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*r_query_surface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLSurface (*r_create_window)(EGLDisplay, EGLConfig, void *, const EGLint *);
static void *(*resolve)(const char *); // the GL path's own resolver, for the ones we call

// what we call for our framebuffer and the copy
static void (*gen_fb)(GLsizei, GLuint *), (*gen_rb)(GLsizei, GLuint *), (*bind_rb)(GLenum, GLuint);
static void (*rb_storage)(GLenum, GLenum, GLsizei, GLsizei), (*fb_rb)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*fb_status)(GLenum);
static void (*blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*clear)(GLbitfield), (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*get_float)(GLenum, GLfloat *), (*get_bool)(GLenum, GLboolean *);
static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void (*enable)(GLenum), (*disable)(GLenum);
static void (*gen_tex)(GLsizei, GLuint *), (*bind_tex)(GLenum, GLuint), (*tex_param)(GLenum, GLenum, GLint);
static void (*tex_image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*fb_tex)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLboolean (*is_enabled)(GLenum);
static EGLContext (*cur_context)(void);

// the game's window surfaces, and a framebuffer of ours for each context drawing to one
#define MAX_WIN 8
static EGLSurface windows[MAX_WIN];
static struct { EGLContext ctx; GLuint fb; } fbs[8];
static __thread GLuint cur_fb;         // ours on this thread's context (0: none, drawing elsewhere)
static __thread EGLSurface cur_draw;

static int is_window(EGLSurface s) {
    for (int i = 0; i < MAX_WIN; i++)
        if (s && windows[i] == s) return 1;
    return 0;
}

static int load(void) {
    static int done, ok;
    if (done) return ok;
    done = 1;
#define R(v, n) (*(void **)&v = resolve ? resolve(n) : NULL)
    R(gen_fb, "glGenFramebuffers"), R(gen_rb, "glGenRenderbuffers"), R(bind_rb, "glBindRenderbuffer");
    R(rb_storage, "glRenderbufferStorage"), R(fb_rb, "glFramebufferRenderbuffer"), R(fb_status, "glCheckFramebufferStatus");
    R(blit, "glBlitFramebuffer"), R(clear, "glClear"), R(clear_color, "glClearColor"), R(get_float, "glGetFloatv");
    R(get_bool, "glGetBooleanv"), R(color_mask, "glColorMask"), R(enable, "glEnable"), R(disable, "glDisable");
    R(is_enabled, "glIsEnabled"), R(cur_context, "eglGetCurrentContext");
    R(gen_tex, "glGenTextures"), R(bind_tex, "glBindTexture"), R(tex_param, "glTexParameteri");
    R(tex_image, "glTexImage2D"), R(fb_tex, "glFramebufferTexture2D");
    if (!r_bind_fb) R(r_bind_fb, "glBindFramebuffer");
    if (!r_get_int) R(r_get_int, "glGetIntegerv");
    if (!r_query_surface) R(r_query_surface, "eglQuerySurface");
#undef R
    ok = gen_fb && gen_rb && bind_rb && rb_storage && fb_rb && fb_status && blit && clear && clear_color && get_float &&
         get_bool && color_mask && enable && disable && is_enabled && cur_context && r_bind_fb && r_get_int && gen_tex &&
         bind_tex && tex_param && tex_image && fb_tex;
    droid_log("phone view: OpenGL ES %s", ok ? "on (the game draws at 1080x1920, shown in the middle)"
                                              : "can't start (no glBlitFramebuffer etc.): the picture is the game's own");
    return ok;
}

// this context's framebuffer (made on its first use at the window). Its colour is a texture:
// the PS5's GL bridge draws a texture's blit to the screen as a quad in the window's 1920x1080
// (a renderbuffer's went to the 4K framebuffer as it is: the picture small, bottom left)
static GLuint context_fb(EGLContext ctx) {
    for (int i = 0; i < 8; i++)
        if (fbs[i].ctx == ctx) return fbs[i].fb;
    GLuint fb = 0, rb[2] = { 0, 0 }, tex = 0;
    gen_fb(1, &fb);
    gen_rb(2, rb);
    gen_tex(1, &tex);
    bind_tex(TEXTURE_2D, tex);
    tex_image(TEXTURE_2D, 0, RGBA8, droid_game_w, droid_game_h, 0, RGBA, UNSIGNED_BYTE, NULL);
    tex_param(TEXTURE_2D, TEX_MIN_FILTER, LINEAR);
    tex_param(TEXTURE_2D, TEX_MAG_FILTER, LINEAR);
    bind_tex(TEXTURE_2D, 0);
    bind_rb(RENDERBUFFER, rb[1]);
    rb_storage(RENDERBUFFER, DEPTH24_STENCIL8, droid_game_w, droid_game_h);
    bind_rb(RENDERBUFFER, 0);
    r_bind_fb(FRAMEBUFFER, fb);
    fb_tex(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex, 0);
    fb_rb(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, rb[1]);
    fb_rb(FRAMEBUFFER, STENCIL_ATTACHMENT, RENDERBUFFER, rb[1]);
    const GLenum st = fb_status(FRAMEBUFFER);
    droid_log("phone view: context %p gets framebuffer %u (%dx%d)%s", ctx, fb, droid_game_w, droid_game_h,
              st == FB_COMPLETE ? "" : " - NOT complete");
    for (int i = 0; i < 8; i++)
        if (!fbs[i].ctx) {
            fbs[i].ctx = ctx, fbs[i].fb = fb;
            break;
        }
    return fb;
}

static EGLSurface w_create_window(EGLDisplay d, EGLConfig c, void *win, const EGLint *attrs) {
    EGLSurface s = r_create_window(d, c, win, attrs);
    for (int i = 0; s && i < MAX_WIN; i++)
        if (!windows[i]) {
            windows[i] = s;
            break;
        }
    return s;
}

static EGLBoolean w_make_current(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    EGLBoolean ok = r_make_current(d, draw, read, ctx);
    cur_fb = 0, cur_draw = draw;
    if (ok && ctx && is_window(draw) && load()) {
        cur_fb = context_fb(ctx);
        r_bind_fb(FRAMEBUFFER, cur_fb); // the game's "screen" from its first draw
    }
    return ok;
}

static EGLBoolean w_query_surface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *v) {
    EGLBoolean ok = r_query_surface(d, s, attr, v);
    if (ok && v && is_window(s)) {
        if (attr == EGL_WIDTH) *v = droid_game_w;
        if (attr == EGL_HEIGHT) *v = droid_game_h;
    }
    return ok;
}

static void w_bind_fb(GLenum target, GLuint fb) { r_bind_fb(target, fb == 0 && cur_fb ? cur_fb : fb); }

static void w_get_int(GLenum pname, GLint *v) {
    r_get_int(pname, v);
    if (cur_fb && v && (pname == FB_BINDING || pname == READ_FB_BINDING) && (GLuint)*v == cur_fb) *v = 0;
}

// the default framebuffer's names for its attachments, ours' names when ours is bound
static GLenum att(GLenum target, GLenum a) {
    if (!cur_fb) return a;
    GLint b = 0;
    r_get_int(target == READ_FB ? READ_FB_BINDING : FB_BINDING, &b);
    if ((GLuint)b != cur_fb) return a;
    if (a == G_COLOR || a == G_BACK) return COLOR_ATTACHMENT0;
    if (a == G_DEPTH) return DEPTH_ATTACHMENT;
    if (a == G_STENCIL) return STENCIL_ATTACHMENT;
    return a;
}
static void fix_list(GLenum target, GLsizei n, const GLenum *in, GLenum *out) {
    for (GLsizei i = 0; i < n && i < 8; i++) out[i] = att(target, in[i]);
}
static void w_invalidate(GLenum target, GLsizei n, const GLenum *a) {
    GLenum f[8];
    if (n > 8 || !a) return r_invalidate(target, n, a);
    fix_list(target, n, a, f);
    r_invalidate(target, n, f);
}
static void w_discard(GLenum target, GLsizei n, const GLenum *a) {
    GLenum f[8];
    if (n > 8 || !a) return r_discard(target, n, a);
    fix_list(target, n, a, f);
    r_discard(target, n, f);
}
static void w_draw_buffers(GLsizei n, const GLenum *a) {
    GLenum f[8];
    if (n > 8 || !a) return r_draw_buffers(n, a);
    fix_list(DRAW_FB, n, a, f);
    r_draw_buffers(n, f);
}
static void w_read_buffer(GLenum a) { r_read_buffer(att(READ_FB, a)); }
static void w_attach_param(GLenum target, GLenum a, GLenum pname, GLint *v) { r_attach_param(target, att(target, a), pname, v); }

// the frame: our framebuffer into the middle of the window's, the sides black
static EGLBoolean w_swap(EGLDisplay d, EGLSurface s) {
    if (!cur_fb || s != cur_draw || !is_window(s)) return r_swap(d, s);
    EGLint sw = 1920, sh = 1080;
    if (gl_real_screen && gl_real_screen(&sw, &sh)) {
    } else if (r_query_surface) {
        r_query_surface(d, s, EGL_WIDTH, &sw);
        r_query_surface(d, s, EGL_HEIGHT, &sh);
    }
    int x, y, w, h;
    view_rect(sw, sh, &x, &y, &w, &h);
    GLint draw_b = 0, read_b = 0;
    r_get_int(FB_BINDING, &draw_b);
    r_get_int(READ_FB_BINDING, &read_b);
    const GLboolean scissor = is_enabled(SCISSOR_TEST);
    GLfloat cc[4];
    GLboolean mask[4];
    get_float(COLOR_CLEAR_VALUE, cc);
    get_bool(COLOR_WRITEMASK, mask);
    if (scissor) disable(SCISSOR_TEST);
    color_mask(1, 1, 1, 1);
    r_bind_fb(DRAW_FB, 0);
    r_bind_fb(READ_FB, cur_fb);
    clear_color(0, 0, 0, 1);
    clear(COLOR_BUFFER_BIT);
    blit(0, 0, droid_game_w, droid_game_h, x, y, x + w, y + h, COLOR_BUFFER_BIT, LINEAR);
    EGLBoolean ok = r_swap(d, s);
    r_bind_fb(DRAW_FB, (GLuint)draw_b);
    r_bind_fb(READ_FB, (GLuint)read_b);
    clear_color(cc[0], cc[1], cc[2], cc[3]);
    color_mask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) enable(SCISSOR_TEST);
    return ok;
}

int phone_gl_wants(const char *name) {
    static const char *const ours[] = { "glBindFramebuffer", "glGetIntegerv", "glInvalidateFramebuffer",
                                        "glDiscardFramebufferEXT", "glDrawBuffers", "glReadBuffer",
                                        "glGetFramebufferAttachmentParameteriv", "eglSwapBuffers", "eglMakeCurrent",
                                        "eglQuerySurface", "eglCreateWindowSurface", NULL };
    if (!phone_view() || !name) return 0;
    for (int i = 0; ours[i]; i++)
        if (!strcmp(name, ours[i])) return 1;
    return 0;
}

// the game's name and what it resolved to: ours instead in the phone view
void *phone_gl_wrap(const char *name, void *real, void *(*resolver)(const char *)) {
    if (!real || !phone_gl_wants(name)) return real;
    if (!resolve) resolve = resolver;
#define W(n, r, w) if (!strcmp(name, n)) { *(void **)&r = real; return (void *)w; }
    W("glBindFramebuffer", r_bind_fb, w_bind_fb);
    W("glGetIntegerv", r_get_int, w_get_int);
    W("glInvalidateFramebuffer", r_invalidate, w_invalidate);
    W("glDiscardFramebufferEXT", r_discard, w_discard);
    W("glDrawBuffers", r_draw_buffers, w_draw_buffers);
    W("glReadBuffer", r_read_buffer, w_read_buffer);
    W("glGetFramebufferAttachmentParameteriv", r_attach_param, w_attach_param);
    W("eglSwapBuffers", r_swap, w_swap);
    W("eglMakeCurrent", r_make_current, w_make_current);
    W("eglQuerySurface", r_query_surface, w_query_surface);
    W("eglCreateWindowSurface", r_create_window, w_create_window);
#undef W
    return real;
}
