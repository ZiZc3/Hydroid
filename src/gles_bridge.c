// gles_bridge.c — a game's OpenGL ES 2/3 and EGL on ps5-opengl (Mesa,
// desktop OpenGL core, GPL-3.0-or-later). Most gl* names go straight to
// ps5-opengl; only where desktop core OpenGL differs from ES is there a step
// in between: ES shaders get a #version, ES contexts become core 3.3 with a
// vertex array bound, old ES texture formats become swizzled ones.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "game_settings.h"
#include "mapper.h"
#include "android_shim.h"
#include "overlay_art.h"
#include "overlay_stats.h"
#include "quick_menu.h"
#include "dpad_grid.h"
#include <ps5_opengl_display_modes.h>

// ps5-opengl's thread-local GL context needs no initialiser call (as its
// native-app/runtime_shims.c has it).
void ps5_opengl_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void ps5_opengl_glapi_tls_context_init(void) {}

#ifndef EGL_OPENGL_ES2_BIT
#define EGL_OPENGL_ES2_BIT 0x0004
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_ALPHA_ES 0x1906

static unsigned long frames;
static int srgb_surface; // the game asked for an sRGB window

// ---- EGL ---------------------------------------------------------------------

static EGLDisplay b_eglGetDisplay(EGLNativeDisplayType native) {
    (void)native;
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

// ---- the screen: 120 Hz when the TV takes it (else ps5-opengl stays at 60), and its
// vblank for the game's Choreographer (java_layer.c) -------------------------------

int sceVideoOutGetVblankStatus(int handle, void *status);
int sceVideoOutWaitVblank(int handle);

extern int (*droid_wait_vblank)(void); // java_layer.c: set once the screen is known
extern int droid_vblank_hz;

static int vblank_handle = -1;

// ps5-opengl opens the video-out port itself and keeps the handle: the link wraps
// sceVideoOutOpen (build-title.sh --wrap) so we see it as it is opened.
int __real_sceVideoOutOpen(int user, int type, int index, const void *param);
static volatile int opened_video_handle = -1;
int __wrap_sceVideoOutOpen(int user, int type, int index, const void *param) {
    int h = __real_sceVideoOutOpen(user, type, index, param);
    droid_log("gl: video out opened: handle %d", h);
    if (h >= 0) opened_video_handle = h;
    return h;
}

// The flip rate (test switch, tools.txt "FlipRate rate=N"): RADV's display (Vulkan games, ANGLE)
// sets 0 - a flip at every refresh, and the PS5's output is 120 Hz on a 120 Hz TV, so a FIFO
// present is 120 fps and pace_frame (vulkan_bridge.c) holds a 60 with clock sleeps. A rate that
// holds each frame for 2 refreshes lets the console pace it, as the PC's vsync does.
// FlipMeter (tools.txt "FlipMeter"): what the screen really shows, from the video-out port's own
// counters - RADV's flip argument (one more per frame) and the vblank count. Every 5 s: frames
// shown for 1, 2, 3, 4+ refreshes, frames never shown (the argument jumped), and the frames shown
// in each second (a dip to 30 for a second is there, which our own present times don't see).
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t *status);
static int64_t swap_now_ns(void); // below
static int flip_meter_handle = -1;
static void *flip_meter_thread(void *arg) {
    (void)arg;
    struct { uint64_t count, process_time, tsc, reserved; uint8_t flags, pad[7]; } vb;
    uint64_t st[16];
    uint64_t last_arg = 0, last_vb = 0;
    int hist[5] = { 0 }, skipped = 0, per_sec[8] = { 0 }, sec_at = 0, frames_sec = 0;
    int64_t window = 0, second = 0;
    for (;;) {
        struct timespec d = { 0, 1000000 };
        nanosleep(&d, NULL);
        memset(st, 0, sizeof(st));
        if (sceVideoOutGetFlipStatus(flip_meter_handle, st) != 0) continue;
        memset(&vb, 0, sizeof(vb));
        sceVideoOutGetVblankStatus(flip_meter_handle, &vb);
        uint64_t arg = st[3];
        int64_t now = swap_now_ns();
        if (!window) window = second = now;
        if (arg != last_arg) {
            if (last_arg) {
                uint64_t refreshes = vb.count - last_vb; // the last frame was up for these
                hist[refreshes >= 4 ? 4 : refreshes]++;
                if (arg > last_arg + 1) skipped += (int)(arg - last_arg - 1);
                frames_sec++;
            }
            last_arg = arg;
            last_vb = vb.count;
        }
        if (now - second >= 1000000000LL) {
            if (sec_at < 8) per_sec[sec_at++] = frames_sec;
            frames_sec = 0;
            second = now;
        }
        if (now - window >= 5000000000LL) {
            char secs[64] = "";
            int at = 0;
            for (int i = 0; i < sec_at; i++) at += snprintf(secs + at, sizeof(secs) - at, " %d", per_sec[i]);
            droid_log("FlipMeter: shown for 1 refresh %d, 2: %d, 3: %d, 4+: %d; never shown %d; frames each second:%s",
                      hist[1], hist[2], hist[3], hist[4], skipped, secs);
            memset(hist, 0, sizeof(hist));
            skipped = sec_at = 0;
            window = now;
        }
    }
    return NULL;
}

int __real_sceVideoOutSetFlipRate(int handle, int rate);
int droid_flip_rate = -1; // -1: not asked (the game's / pace_frame's pacing, as before)
int __wrap_sceVideoOutSetFlipRate(int handle, int rate) {
    int droid_tool(const char *name);
    const char *droid_tool_args(const char *name);
    if (droid_flip_rate < 0 && droid_tool("FlipRate")) {
        const char *a = droid_tool_args("FlipRate");
        const char *r = a ? strstr(a, "rate=") : NULL;
        droid_flip_rate = r ? atoi(r + 5) : 1;
    }
    if (flip_meter_handle < 0 && droid_tool("FlipMeter")) { // the game's port
        flip_meter_handle = handle;
        pthread_t th;
        pthread_create(&th, NULL, flip_meter_thread, NULL);
        droid_log("FlipMeter: on (video out %d)", handle);
    }
    int want = droid_flip_rate >= 0 ? droid_flip_rate : rate;
    int res = __real_sceVideoOutSetFlipRate(handle, want);
    droid_log("gl: video out %d: flip rate %d asked, %d set -> %#x", handle, rate, want, res);
    return res;
}

// ps5-opengl's video-out port: the handle whose vblank status answers
static int find_vblank_handle(void) {
    struct { uint64_t count, process_time, tsc, reserved; uint8_t flags, pad[7]; } st;
    for (int h = 0; h < 64; h++) {
        memset(&st, 0, sizeof(st));
        if (sceVideoOutGetVblankStatus(h, &st) == 0 && st.count > 0) return h;
    }
    return -1;
}

// What the TV really showed: at each vblank, has a new frame gone up (the flip count)?
// How many vblanks each frame stayed: smooth 30 fps at 60 Hz is all 2s.
// sceVideoOutGetFlipStatus isn't in our link (its import was 0: a crash): looked up in
// the loaded libSceVideoOut, as Ruffle Flash PS5's ps5_early.c finds modules.
int sceKernelGetModuleList(int *handles, size_t count, size_t *out) __attribute__((weak));
int sceKernelGetModuleInfo(int handle, void *info) __attribute__((weak));
int sceKernelDlsym(int handle, const char *name, void **fn) __attribute__((weak));
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, const void *opt,
                             int *res) __attribute__((weak));
static int (*get_flip_status)(int handle, void *status);
static int find_video_module(void) {
    static int handles[512];
    size_t count = 0;
    if (!sceKernelGetModuleList || !sceKernelGetModuleInfo || sceKernelGetModuleList(handles, 512, &count) != 0)
        return -1; // (optional imports: 0 when the system has none)
    static const uint64_t sizes[] = { 0x160, 0x1a8, 0x1b0, 0x158, 0x200 };
    for (size_t i = 0; i < count; i++)
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            struct { uint64_t size; char name[256]; uint8_t rest[0x400]; } info;
            memset(&info, 0, sizeof(info));
            info.size = sizes[k];
            if (sceKernelGetModuleInfo(handles[i], &info) != 0) continue;
            if (strstr(info.name, "libSceVideoOut")) return handles[i];
            break;
        }
    return -1;
}

static void flip_watch(void) {
    static int looked;
    if (!looked) {
        looked = 1;
        // looked up, never linked: linked, its import came out 0 on the console and the first
        // call crashed every game (rip 0). The module from the list, else from the loader
        int m = find_video_module();
        if (m < 0 && sceKernelLoadStartModule)
            m = sceKernelLoadStartModule("libSceVideoOut.sprx", 0, NULL, 0, NULL, NULL);
        void *fn = NULL;
        if (m >= 0 && sceKernelDlsym) sceKernelDlsym(m, "sceVideoOutGetFlipStatus", &fn);
        get_flip_status = (int (*)(int, void *))fn;
        droid_log("screen: flip status %s (module %#x)", fn ? "found: frames as the screen shows them" : "not found: no flip report", m);
    }
    if (!get_flip_status) return;
    struct {
        uint64_t count, process_time, tsc;
        int64_t flip_arg;
        uint64_t submit_tsc, reserved0;
        int32_t gpu_pending, pending, current_buffer;
        uint32_t reserved1;
    } st;
    static uint64_t last_count, vblanks;
    static int since, shown[6], seen;
    static int64_t last_tick;
    static int64_t tick_min = INT64_MAX, tick_max;
    memset(&st, 0, sizeof(st));
    if (get_flip_status(vblank_handle, &st) != 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (last_tick) {
        int64_t d = now - last_tick;
        if (d < tick_min) tick_min = d;
        if (d > tick_max) tick_max = d;
    }
    last_tick = now;
    since++;
    if (st.count != last_count) {
        if (seen++) shown[since > 5 ? 5 : since]++;
        since = 0;
        last_count = st.count;
    }
    if (++vblanks % 300 == 0) {
        droid_log("screen: last 300 refreshes, frames shown for 1: %d, 2: %d, 3: %d, 4: %d, 5+: %d refreshes; "
                  "vblank waits %.1f-%.1f ms apart", shown[1], shown[2], shown[3], shown[4], shown[5],
                  tick_min / 1e6, tick_max / 1e6);
        memset(shown, 0, sizeof(shown));
        tick_min = INT64_MAX;
        tick_max = 0;
    }
}

static int wait_vblank(void) {
    if (sceVideoOutWaitVblank(vblank_handle) != 0) return -1;
    flip_watch();
    return 0;
}

// Vulkan games: the same, once RADV has the port (no EGL display; 60 Hz)
void droid_screen_ready(void) {
    vblank_handle = opened_video_handle >= 0 ? opened_video_handle : find_vblank_handle();
    droid_log("gl: screen (Vulkan): video-out handle %d%s", vblank_handle,
              vblank_handle >= 0 ? " (the Choreographer ticks on its vblank)" : "");
    droid_vblank_hz = 60;
    if (vblank_handle >= 0) droid_wait_vblank = wait_vblank;
}

static void screen_ready(EGLDisplay dpy) {
    EGLint w = 0, h = 0, hz = 0;
    if (!eglGetDisplayModePS5(dpy, &w, &h, &hz)) hz = 60;
    vblank_handle = opened_video_handle >= 0 ? opened_video_handle : find_vblank_handle();
    droid_log("gl: screen %dx%d at %d Hz, video-out handle %d%s", w, h, hz, vblank_handle,
              vblank_handle >= 0 ? " (the Choreographer ticks on its vblank)" : "");
    droid_vblank_hz = hz > 0 ? hz : 60;
    if (vblank_handle >= 0) droid_wait_vblank = wait_vblank;
}

static EGLBoolean b_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    // only while EGL is down. 60 Hz: a 30 fps frame is 2 vblanks of 16.7 ms, its ~9 ms of
    // work far from the edge (at 120 Hz it sits on the 8.3 ms one: frames up 3, 4 or 5
    // vblanks, judder). The game's settings may ask for 120 (60 if the TV can't).
    int want = app_fps_cap_hz() == 120 ? 120 : app_settings.fps_cap ? 60 : settings.refresh_hz ? settings.refresh_hz : 60; // the cap's
    EGLBoolean hfr = eglSetDisplayRefreshPS5(dpy, want);
    eglGetError(); // refused while EGL runs: that error isn't the game's (it saw EGL_BAD_ACCESS)
    static int told;
    if (!told++) droid_log("gl: asked for %d Hz: %s", want, hfr ? "ok" : "refused now");
    EGLBoolean ok = eglInitialize(dpy, major, minor);
    if (ok) ok = eglBindAPI(EGL_OPENGL_API);
    droid_log("gl: eglInitialize -> %d (ps5-opengl, desktop GL for the game's GLES)", ok);
    return ok;
}

// The game asks for an ES-renderable config: ask ps5-opengl for an OpenGL one.
static EGLBoolean b_eglChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs,
                                    EGLint size, EGLint *count) {
    EGLint list[64];
    int n = 0;
    for (int i = 0; attribs && attribs[i] != EGL_NONE && n < 60; i += 2) {
        EGLint key = attribs[i], value = attribs[i + 1];
        if (key == EGL_RENDERABLE_TYPE) value = EGL_OPENGL_BIT;
        if (key == EGL_SURFACE_TYPE) value = EGL_WINDOW_BIT;
        list[n++] = key;
        list[n++] = value;
    }
    list[n] = EGL_NONE;
    EGLBoolean ok = eglChooseConfig(dpy, list, configs, size, count);
    if (!ok || (count && *count == 0)) {
        static const EGLint fallback[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                           EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                           EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE };
        ok = eglChooseConfig(dpy, fallback, configs, size, count);
        droid_log("gl: eglChooseConfig: the game's attributes matched nothing, fallback -> %d", ok);
    }
    return ok;
}

static EGLBoolean b_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint attr, EGLint *v) {
    EGLBoolean ok = eglGetConfigAttrib(dpy, cfg, attr, v);
    if (ok && v && (attr == EGL_RENDERABLE_TYPE || attr == EGL_CONFORMANT))
        *v |= EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
    return ok;
}

static EGLSurface b_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
                                           const EGLint *attribs) {
    (void)win;
    // The game's settings: only the colour space goes on (sRGB: the game writes linear
    // colour and the screen turns it into normal brightness; without it, too dark).
    EGLint pass[3] = { EGL_NONE, EGL_NONE, EGL_NONE };
    char seen[256] = "";
    int at = 0;
    for (int i = 0; attribs && attribs[i] != EGL_NONE && i < 32; i += 2) {
        at += snprintf(seen + at, sizeof(seen) - at, " %#x=%#x", attribs[i], attribs[i + 1]);
        if (attribs[i] == 0x309D) { // EGL_GL_COLORSPACE_KHR
            pass[0] = 0x309D;
            pass[1] = attribs[i + 1];
            if (attribs[i + 1] == 0x3089) srgb_surface = 1; // EGL_GL_COLORSPACE_SRGB_KHR
        }
    }
    EGLSurface s = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, pass[0] != EGL_NONE ? pass : NULL);
    if (!s && pass[0] != EGL_NONE) {
        droid_log("gl: window surface with colour space %#x refused (%#x); without", pass[1], eglGetError());
        s = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    }
    droid_log("gl: window surface %p (error %#x), game's settings:%s", s, s ? 0 : eglGetError(),
              seen[0] ? seen : " none");
    return s;
}

static EGLBoolean b_eglQuerySurface(EGLDisplay dpy, EGLSurface s, EGLint attr, EGLint *v) {
    EGLBoolean ok = eglQuerySurface(dpy, s, attr, v);
    static int logged;
    if (logged++ < 24) droid_log("gl: game asks surface %#x -> %d, value %#x", attr, ok, ok && v ? *v : 0);
    return ok;
}

static EGLContext b_eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
                                     const EGLint *attribs) {
    int es = 1;
    for (int i = 0; attribs && attribs[i] != EGL_NONE; i += 2)
        if (attribs[i] == EGL_CONTEXT_CLIENT_VERSION) es = attribs[i + 1];
    static const EGLint core[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                                   EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                   EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
    EGLContext c = eglCreateContext(dpy, cfg, share, core);
    droid_log("gl: game asked for OpenGL ES %d -> core 3.3 context %p (error %#x)", es, c,
              c ? 0 : eglGetError());
    return c;
}

// What the PS5's GL can take of a phone game's textures (ASTC / ETC2 are phone formats).
static void gl_report_formats(void) {
    static const char *want[] = { "GL_KHR_texture_compression_astc_ldr", "GL_KHR_texture_compression_astc_hdr",
                                  "GL_ARB_ES3_compatibility", "GL_OES_compressed_ETC1_RGB8_texture",
                                  "GL_EXT_texture_compression_s3tc", "GL_ARB_texture_compression_bptc",
                                  "GL_EXT_texture_sRGB_decode", "GL_ARB_ES3_2_compatibility" };
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
        int has = 0;
        for (GLint i = 0; i < n && !has; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            has = e && strcmp(e, want[w]) == 0;
        }
        droid_log("gl: %s %s", want[w], has ? "yes" : "NO");
    }
    GLint nf = 0, fmts[256];
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &nf);
    if (nf > 256) nf = 256;
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, fmts);
    char line[1200];
    int at = snprintf(line, sizeof(line), "gl: %d compressed formats:", nf);
    for (GLint i = 0; i < nf && at < (int)sizeof(line) - 8; i++) at += snprintf(line + at, sizeof(line) - at, " %#x", fmts[i]);
    droid_log("%s", line);
    glGetError();
}

// GL errors right after the calls a phone game's ES may use differently: the first ones logged.
static int gl_errors_logged;
static void gl_check(const char *call, long a, long b, long c, long d) {
    // only the first 300 frames (the game's debug log: always): glGetError makes the CPU wait
    // for the driver - after every draw and state call that was ~0.2 ms a draw (Shadow Fight 3)
    if (frames > 300 && !settings.debug_log) return;
    GLenum e = glGetError();
    if (e != GL_NO_ERROR && gl_errors_logged < 80) {
        gl_errors_logged++;
        droid_log("gl: %s(%#lx, %#lx, %#lx, %#lx) -> error %#x (frame %lu)", call, a, b, c, d, e, frames);
    }
}

static void c_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void *p) {
    static int seen;
    if (seen++ < 12) droid_log("gl: compressed texture %#x %dx%d level %d (%d bytes)", f, w, h, l, n);
    glCompressedTexImage2D(t, l, f, w, h, b, n, p);
    gl_check("glCompressedTexImage2D", t, f, w, h);
}
// Read a just-uploaded level back as RGBA8: how much of it isn't black (is the
// PS5's ASTC real?). The first few big ones only.
static void texture_probe(GLenum t, GLint l, GLsizei w, GLsizei h, GLenum f, const void *src, GLsizei n) {
    static int probes;
    if (t != GL_TEXTURE_2D || w * h < 128 * 128 || probes >= 4) return;
    probes++;
    GLint pack = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    memset(px, 0, (size_t)w * h * 4);
    glGetTexImage(GL_TEXTURE_2D, l, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum e = glGetError();
    long lit = 0, sum = 0;
    for (long i = 0; i < (long)w * h; i++) {
        unsigned v = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        if (v > 12) lit++;
        sum += v;
    }
    long nz = 0;
    for (GLsizei i = 0; i < n; i++) nz += ((const unsigned char *)src)[i] != 0;
    droid_log("gl: texture probe %#x %dx%d level %d: %ld%% pixels not black, mean %ld, first px %02x%02x%02x%02x "
              "(read error %#x; source %ld%% non-zero bytes)", f, w, h, l, lit * 100 / ((long)w * h),
              sum / ((long)w * h * 3), px[0], px[1], px[2], px[3], e, n ? nz * 100 / n : 0);
    free(px);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
}

static void c_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei n, const void *p) {
    static int seen;
    if (seen++ < 12) droid_log("gl: compressed sub-texture %#x %dx%d level %d (%d bytes)", f, w, h, l, n);
    glCompressedTexSubImage2D(t, l, x, y, w, h, f, n, p);
    gl_check("glCompressedTexSubImage2D", t, f, w, h);
    GLint unpack = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    if (!unpack && x == 0 && y == 0) texture_probe(t, l, w, h, f, p, n);
}
static void c_glTexStorage2D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h) {
    glTexStorage2D(t, l, f, w, h);
    gl_check("glTexStorage2D", t, f, w, h);
}
static void c_glRenderbufferStorageMultisample(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h) {
    glRenderbufferStorageMultisample(t, s, f, w, h);
    gl_check("glRenderbufferStorageMultisample", s, f, w, h);
}
static void c_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) {
    glRenderbufferStorage(t, f, w, h);
    gl_check("glRenderbufferStorage", t, f, w, h);
}
static void c_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) {
    glFramebufferTexture2D(t, a, tt, tex, l);
    gl_check("glFramebufferTexture2D", a, tt, tex, l);
}
static GLenum c_glCheckFramebufferStatus(GLenum t) {
    GLenum r = glCheckFramebufferStatus(t);
    static int logged;
    if (r != GL_FRAMEBUFFER_COMPLETE && logged++ < 20) droid_log("gl: framebuffer not complete: %#x", r);
    return r;
}
static void c_glEnable(GLenum cap) { glEnable(cap); gl_check("glEnable", cap, 0, 0, 0); }
static void c_glDisable(GLenum cap) { glDisable(cap); gl_check("glDisable", cap, 0, 0, 0); }
static void c_glTexParameteri(GLenum t, GLenum n, GLint v) { glTexParameteri(t, n, v); gl_check("glTexParameteri", t, n, v, 0); }
static void c_glDrawBuffers(GLsizei n, const GLenum *b) { glDrawBuffers(n, b); gl_check("glDrawBuffers", n, n > 0 ? b[0] : 0, 0, 0); }
static void c_glInvalidateFramebuffer(GLenum t, GLsizei n, const GLenum *a) {
    glInvalidateFramebuffer(t, n, a);
    gl_check("glInvalidateFramebuffer", t, n, n > 0 ? a[0] : 0, 0);
}
// ps5-opengl refused GL_DRAW_FRAMEBUFFER binds for Shadow Fight 2 (INVALID_ENUM): its
// frames went nowhere and the screen stayed black. Refused: bound as GL_FRAMEBUFFER (both).
// The first binds are logged with their own error (earlier calls' errors cleared first).
static int draw_fb_refused = -1; // GL_DRAW/READ_FRAMEBUFFER refused by the driver: known after the first
static void c_glBindFramebuffer(GLenum t, GLuint fb) {
    // once known, no error checks (each glGetError waits for the driver)
    if (draw_fb_refused >= 0 && frames > 300 && !settings.debug_log) {
        glBindFramebuffer(draw_fb_refused && (t == GL_DRAW_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) ? GL_FRAMEBUFFER : t, fb);
        return;
    }
    while (glGetError() != GL_NO_ERROR) {}
    glBindFramebuffer(t, fb);
    GLenum e = glGetError();
    int fallback = 0;
    if (e == GL_INVALID_ENUM && (t == GL_DRAW_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER)) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        fallback = 1;
        e = glGetError();
    }
    if (t == GL_DRAW_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) draw_fb_refused = fallback;
    static int logged;
    if (logged < 40 && (logged < 12 || fallback || e)) {
        logged++;
        droid_log("gl: bind framebuffer %#x <- %u%s (error %#x, frame %lu)", t, fb,
                  fallback ? ": refused, bound as GL_FRAMEBUFFER" : "", e, frames);
    }
}
static void c_glHint(GLenum t, GLenum m) { glHint(t, m); gl_check("glHint", t, m, 0, 0); }
static void c_glPixelStorei(GLenum n, GLint v) { glPixelStorei(n, v); gl_check("glPixelStorei", n, v, 0, 0); }
// ---- ES client-side arrays: core GL has none ----------------------------------------
// ES draws from vertex / index arrays in the game's own memory when no buffer is bound (GTA SA's
// cutscenes); core GL refuses those draws (GL_INVALID_OPERATION) and Mesa then read an index
// offset as an address (crash). They go through stream buffers here; draws from buffers as before.
#define CLIENT_ATTRS 16
static int gl_stream_mode(void); // below: tools.txt "GLStream mode=N"
typedef struct {
    int on, client;
    GLint size;
    GLenum type;
    GLboolean norm;
    GLsizei stride;
    const void *ptr;
} client_attr;
static client_attr cattr[CLIENT_ATTRS];
static GLuint array_bound;    // the game's GL_ARRAY_BUFFER
static int client_attr_count; // attributes with a client pointer
static int client_draws_logged;

static void c_glBindBuffer(GLenum t, GLuint b) {
    if (t == GL_ARRAY_BUFFER) array_bound = b;
    glBindBuffer(t, b);
}
static void c_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    if (i < CLIENT_ATTRS && gl_stream_mode() != 3) {
        int client = array_bound == 0 && p != NULL;
        client_attr_count += client - cattr[i].client;
        cattr[i] = (client_attr){ cattr[i].on, client, size, type, norm, stride, p };
        if (client) return; // set at the draw
    }
    glVertexAttribPointer(i, size, type, norm, stride, p);
}
static void c_glEnableVertexAttribArray(GLuint i) {
    if (i < CLIENT_ATTRS) cattr[i].on = 1;
    glEnableVertexAttribArray(i);
}
static void c_glDisableVertexAttribArray(GLuint i) {
    if (i < CLIENT_ATTRS) cattr[i].on = 0;
    glDisableVertexAttribArray(i);
}
static unsigned gl_type_size(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case 0x8D61 /* HALF_FLOAT_OES */: return 2;
    default: return 4;
    }
}
// the client attributes copied into the stream buffer (vertices 0..count-1), pointed at there
// Stream rings: one big buffer each, filled at rising offsets through unsynchronized maps and
// given back to the driver (orphaned) only when full. A new small buffer per draw made the
// console's driver wait for the GPU at every draw (Geometry Dash: 1.2 ms a draw, 20-30 fps).
#define RING_SIZE (8u << 20)
typedef struct { GLuint buf; size_t off; uint8_t *persist; } stream_ring;
static stream_ring vring, iring;
// tools.txt "GLStream mode=N" (tests on the console): 0 ring + unsynchronized maps, 1 a new buffer
// each draw, 2 a persistent mapped ring (glBufferStorage), 3 the game's pointers straight to GL
static int stream_mode = -1;
static int gl_stream_mode(void) {
    if (stream_mode < 0) {
        const char *a = droid_tool("GLStream") ? droid_tool_args("GLStream") : NULL;
        const char *m = a ? strstr(a, "mode=") : NULL;
        stream_mode = m ? atoi(m + 5) : 0;
        if (stream_mode < 0 || stream_mode > 3) stream_mode = 0;
        droid_log("gl: draws from the game's memory: mode %d", stream_mode);
    }
    return stream_mode;
}
typedef void (*buffer_storage_fn)(GLenum, GLsizeiptr, const void *, GLbitfield);
// room for len bytes, bound at target: its mapping (NULL: write with glBufferSubData at *at)
static void *ring_map(stream_ring *r, GLenum target, size_t len, size_t *at) {
    int mode = gl_stream_mode();
    if (!r->buf) {
        glGenBuffers(1, &r->buf);
        glBindBuffer(target, r->buf);
        buffer_storage_fn storage = mode == 2 ? (buffer_storage_fn)eglGetProcAddress("glBufferStorage") : NULL;
        if (storage) {
            storage(target, RING_SIZE, NULL, GL_MAP_WRITE_BIT | 0x40 /* PERSISTENT */ | 0x80 /* COHERENT */);
            r->persist = glMapBufferRange(target, 0, RING_SIZE, GL_MAP_WRITE_BIT | 0x40 | 0x80);
            droid_log("gl: persistent stream ring %s", r->persist ? "mapped" : "NOT mapped");
        }
        if (!r->persist) glBufferData(target, RING_SIZE, NULL, GL_STREAM_DRAW);
    } else {
        glBindBuffer(target, r->buf);
    }
    if (r->persist && len <= RING_SIZE) {
        if (r->off + len > RING_SIZE) {
            glFinish(); // (the GPU done with the ring before it's written again: once per 8 MB)
            r->off = 0;
        }
        *at = r->off;
        r->off = (r->off + len + 63) & ~(size_t)63;
        return r->persist + *at;
    }
    if (mode == 1 || len > RING_SIZE) { // a buffer of its own for this draw
        glBufferData(target, (GLsizeiptr)len, NULL, GL_STREAM_DRAW);
        r->off = RING_SIZE; // (a ring after it: sized back)
        *at = 0;
        return glMapBufferRange(target, 0, (GLsizeiptr)len, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    }
    if (r->off + len > RING_SIZE) {
        glBufferData(target, RING_SIZE, NULL, GL_STREAM_DRAW);
        r->off = 0;
    }
    *at = r->off;
    r->off = (r->off + len + 63) & ~(size_t)63;
    return glMapBufferRange(target, (GLintptr)*at, (GLsizeiptr)len,
                            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
}
static void ring_unmap(stream_ring *r, GLenum target) {
    if (!r->persist) glUnmapBuffer(target);
}

static int client_upload(GLsizei count) {
    if (!client_attr_count || count <= 0) return 0;
    size_t total = 0, at[CLIENT_ATTRS], len[CLIENT_ATTRS];
    for (int i = 0; i < CLIENT_ATTRS; i++) {
        len[i] = 0;
        if (!cattr[i].on || !cattr[i].client) continue;
        size_t elem = (size_t)cattr[i].size * gl_type_size(cattr[i].type);
        size_t stride = cattr[i].stride ? (size_t)cattr[i].stride : elem;
        at[i] = total;
        len[i] = (size_t)(count - 1) * stride + elem;
        total += (len[i] + 15) & ~(size_t)15;
    }
    if (!total) return 0;
    size_t base = 0;
    uint8_t *dst = ring_map(&vring, GL_ARRAY_BUFFER, total, &base);
    for (int i = 0; i < CLIENT_ATTRS; i++) {
        if (!len[i]) continue;
        if (dst) memcpy(dst + at[i], cattr[i].ptr, len[i]);
        else glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(base + at[i]), (GLsizeiptr)len[i], cattr[i].ptr);
        size_t elem = (size_t)cattr[i].size * gl_type_size(cattr[i].type);
        glVertexAttribPointer((GLuint)i, cattr[i].size, cattr[i].type, cattr[i].norm,
                              cattr[i].stride ? cattr[i].stride : (GLsizei)elem, (const void *)(uintptr_t)(base + at[i]));
    }
    if (dst) ring_unmap(&vring, GL_ARRAY_BUFFER);
    { // each attribute layout of these draws once (a slow draw: the driver converting a format?)
        static uint32_t seen[32];
        static int nseen;
        for (int i = 0; i < CLIENT_ATTRS; i++) {
            if (!len[i]) continue;
            uint32_t key = (uint32_t)i << 28 | (uint32_t)cattr[i].size << 24 | (uint32_t)(cattr[i].type & 0xFFF) << 12 |
                           (uint32_t)cattr[i].norm << 11 | (uint32_t)(cattr[i].stride & 0x7FF);
            int k = 0;
            while (k < nseen && seen[k] != key) k++;
            if (k == nseen && nseen < 32) {
                seen[nseen++] = key;
                droid_log("gl: game-memory attribute %d: %d x %#x%s, stride %d", i, cattr[i].size, cattr[i].type,
                          cattr[i].norm ? " normalized" : "", (int)cattr[i].stride);
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, array_bound);
    if (client_draws_logged < 3) {
        client_draws_logged++;
        droid_log("gl: draw from the game's memory (ES client arrays): %d vertices, %zu bytes through a stream buffer",
                  (int)count, total);
    }
    return 1;
}
// draw costs, every 5 s in the log: from buffers / from the game's memory (and its upload)
static struct { int64_t at, buf_ns, cl_ns, up_ns; int buf_n, cl_n; } dstat;
static int64_t draw_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void draw_stat(int client, int64_t t0, int64_t t1, int64_t t2) {
    if (client) { dstat.cl_n++; dstat.up_ns += t1 - t0; dstat.cl_ns += t2 - t1; }
    else { dstat.buf_n++; dstat.buf_ns += t2 - t1; }
    if (!dstat.at) dstat.at = t2;
    if (t2 - dstat.at >= 5000000000LL) {
        droid_log("gl: draws in 5 s: %d from buffers (%.1f ms), %d from the game's memory (%.1f ms + upload %.1f ms)",
                  dstat.buf_n, dstat.buf_ns / 1e6, dstat.cl_n, dstat.cl_ns / 1e6, dstat.up_ns / 1e6);
        memset(&dstat, 0, sizeof(dstat));
        dstat.at = t2;
    }
}
static void c_glDrawArrays(GLenum m, GLint first, GLsizei n) {
    int64_t t0 = draw_now_ns();
    int client = client_attr_count && client_upload(first + n);
    int64_t t1 = draw_now_ns();
    glDrawArrays(m, first, n);
    draw_stat(client, t0, t1, draw_now_ns());
}
static void c_glDrawElements(GLenum m, GLsizei n, GLenum t, const void *i) {
    int64_t t0 = draw_now_ns();
    // (a game's own memory is far past any buffer offset: only those draws ask for the binding)
    GLint ibo = 1;
    if (client_attr_count || (uintptr_t)i >= 0x100000000ull) glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ibo);
    int client_idx = ibo == 0 && i != NULL && n > 0 && gl_stream_mode() != 3;
    if (client_attr_count) {
        // the vertices used: up to the largest index (read where the indices are)
        unsigned isz = gl_type_size(t), max = 0;
        const void *src = i;
        void *copy = NULL;
        if (!client_idx && n > 0) { // indices in a buffer: read them back
            copy = malloc((size_t)n * isz);
            if (copy) glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)(uintptr_t)i, (GLsizeiptr)n * isz, copy);
            src = copy;
        }
        for (GLsizei k = 0; src && k < n; k++) {
            unsigned v = isz == 1 ? ((const uint8_t *)src)[k] : isz == 2 ? ((const uint16_t *)src)[k] : ((const uint32_t *)src)[k];
            if (v > max) max = v;
        }
        free(copy);
        client_upload((GLsizei)max + 1);
    }
    int64_t t1 = draw_now_ns();
    if (client_idx) {
        size_t bytes = (size_t)n * gl_type_size(t), at = 0;
        void *dst = ring_map(&iring, GL_ELEMENT_ARRAY_BUFFER, bytes, &at);
        if (dst) {
            memcpy(dst, i, bytes);
            ring_unmap(&iring, GL_ELEMENT_ARRAY_BUFFER);
        } else {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)at, (GLsizeiptr)bytes, i);
        }
        glDrawElements(m, n, t, (const void *)(uintptr_t)at);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        glDrawElements(m, n, t, i);
    }
    draw_stat(client_idx || client_attr_count, t0, t1, draw_now_ns());
    gl_check("glDrawElements", m, n, t, (long)(uintptr_t)i);
}

// Core OpenGL draws nothing without a vertex array object; ES has a default one.
static EGLContext vao_contexts[8];

static EGLBoolean b_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                   EGLContext ctx) {
    EGLBoolean ok = eglMakeCurrent(dpy, draw, read, ctx);
    if (!ok || ctx == EGL_NO_CONTEXT) return ok;
    // ES always converts to sRGB when drawing to an sRGB target; desktop GL only with this on.
    glEnable(GL_FRAMEBUFFER_SRGB);
    for (int i = 0; i < 8; i++) {
        if (vao_contexts[i] == ctx) return ok;
        if (vao_contexts[i] == NULL) {
            vao_contexts[i] = ctx;
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            droid_log("gl: context %p current, vertex array %u bound; %s", ctx, vao,
                      (const char *)glGetString(GL_RENDERER));
            if (i == 0) gl_report_formats();
            return ok;
        }
    }
    return ok;
}

// ---- FPS counter: drawn at every swap (all games), colored by speed ------------------
// (gl_overlay.inc, built twice: on ps5-opengl here, on ANGLE further down)
#define OVL_PX 5      // one font pixel = 5x5 screen pixels
#define OVL_X 24
#define OVL_Y 24      // from the top
#define OVL_SCREEN_H 1080
extern float droid_cursor_x, droid_cursor_y;
extern volatile int droid_cursor_shown;
#define CUR_W 40
#define CUR_H 48
#define CUR_O 4 // the tip's place in the texture
int droid_shot_due(void); // overlay_art.c: ShotBox
void droid_shot_write(const uint8_t *px, int w, int h, size_t stride, int bgra, int bottom_up, int sec);
#define OVL_NAME(x) x
#define OVL_S 1
#define OVL_SURF_W 1920
#define OVL_SURF_H 1080
#define OVL_DESKTOP 1
#define OVL_GLSL_VS "#version 330 core\n"
#define OVL_GLSL_FS "#version 330 core\n"
#include "gl_overlay.inc"
#undef OVL_NAME
#undef OVL_S
#undef OVL_SURF_W
#undef OVL_SURF_H
#undef OVL_DESKTOP
#undef OVL_GLSL_VS
#undef OVL_GLSL_FS

// The finished picture (before the swap): how much of it isn't black.
static void frame_probe(void) {
    GLint read_fb = 0, pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    int lit = 0, total = 0;
    unsigned char px[4], center[4] = {0};
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            glReadPixels(60 + x * 112, 30 + y * 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (px[0] + px[1] + px[2] > 12) lit++;
            if (x == 8 && y == 8) memcpy(center, px, 4);
            total++;
        }
    droid_log("gl: frame %lu picture: %d%% of sampled pixels not black, center %02x%02x%02x (error %#x)", frames,
              lit * 100 / total, center[0], center[1], center[2], glGetError());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
}

// Swap interval: always 1 - each frame waits for a screen refresh (ps5-opengl's
// swap with 0 returns at once: frames reach the screen at random points, judder).
static EGLBoolean b_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    static int logged;
    if (logged++ < 6) droid_log("gl: game asks swap interval %d -> 1", interval);
    return eglSwapInterval(dpy, 1);
}

void java_vsync(int64_t t); // java_layer.c: the Choreographer keeps to the refresh

static int64_t swap_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// Even frames on screen: each swap goes on the vblank grid of the game's rhythm (30 fps
// at 120 Hz: every 4th vblank), right after a vblank - else a frame flips whenever the
// GPU is done and stays up 3 or 5 vblanks (judder at a steady 30 fps).
static uint64_t vblank_count(void) {
    struct { uint64_t count, process_time, tsc, reserved; uint8_t flags, pad[7]; } st;
    memset(&st, 0, sizeof(st));
    return sceVideoOutGetVblankStatus(vblank_handle, &st) == 0 ? st.count : 0;
}

static void pace_swap(void) {
    static uint64_t last_count;
    static double last_ns, rhythm_ms;
    if (vblank_handle < 0 || droid_vblank_hz <= 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec * 1e9 + ts.tv_nsec;
    if (last_ns) { // the game's own frame interval, smoothed
        double ms = (now - last_ns) / 1e6;
        if (ms > 0 && ms < 200) rhythm_ms = rhythm_ms ? rhythm_ms * 0.9 + ms * 0.1 : ms;
    }
    double period = 1000.0 / droid_vblank_hz;
    int every = rhythm_ms ? (int)(rhythm_ms / period + 0.5) : 1;
    if (every < 1) every = 1;
    if (every > 8) every = 8;
    uint64_t c = vblank_count();
    if (c && last_count && c < last_count + (uint64_t)every && c + 16 > last_count) {
        while (c < last_count + (uint64_t)every) {
            if (sceVideoOutWaitVblank(vblank_handle) != 0) break;
            c = vblank_count();
        }
    }
    last_count = c;
    last_ns = now; // the game's rhythm: request to request, our wait not in it
    static unsigned long logged;
    if (logged++ % 900 == 899)
        droid_log("gl: swaps every %d vblank(s) at %d Hz (game frame %.1f ms)", every, droid_vblank_hz, rhythm_ms);
}

// ---- EGL_ANDROID_presentation_time, for Unity's Swappy (Android Frame Pacing) ----------
// Swappy names the time each frame should be on screen; the swap holds the frame until
// the vblank just before it (a phone's compositor does the same). Without it Swappy
// can't pace and frames flip whenever they're done: some stay 3 vblanks, some 5.
static volatile int64_t present_at_ns; // 0: none asked
static EGLBoolean b_eglPresentationTimeANDROID(EGLDisplay dpy, EGLSurface s, int64_t when_ns) {
    (void)dpy; (void)s;
    static int logged;
    if (!logged++) droid_log("gl: Swappy paces frames (eglPresentationTimeANDROID)");
    present_at_ns = when_ns;
    return EGL_TRUE;
}
// frame timestamps: not had (Swappy then goes by its own clock)
static EGLBoolean b_eglGetFrameTimestampSupportedANDROID(EGLDisplay d, EGLSurface s, EGLint t) {
    (void)d; (void)s; (void)t;
    return EGL_FALSE;
}
static EGLBoolean b_eglGetNextFrameIdANDROID(EGLDisplay d, EGLSurface s, uint64_t *id) {
    (void)d; (void)s;
    if (id) *id = frames + 1;
    return EGL_TRUE;
}
static EGLBoolean b_eglGetFrameTimestampsANDROID(EGLDisplay d, EGLSurface s, uint64_t id, EGLint n,
                                                 const EGLint *names, int64_t *values) {
    (void)d; (void)s; (void)id; (void)names;
    for (EGLint i = 0; i < n; i++) values[i] = -1; // EGL_TIMESTAMP_PENDING_ANDROID
    return EGL_FALSE;
}

static const char *b_eglQueryString(EGLDisplay dpy, EGLint name) {
    const char *s = eglQueryString(dpy, name);
    if (name != EGL_EXTENSIONS) return s;
    static char ext[4096];
    snprintf(ext, sizeof(ext), "%s EGL_ANDROID_presentation_time EGL_KHR_gl_colorspace", s ? s : "");
    static int told;
    if (!told++) droid_log("egl: extensions the game sees: %s", ext);
    return ext;
}

// The frame's wait for its time: vblanks until the next one is close to it.
static void present_wait(void) {
    int64_t at = present_at_ns;
    present_at_ns = 0;
    if (!at || vblank_handle < 0) return;
    int64_t period = 1000000000LL / (droid_vblank_hz > 0 ? droid_vblank_hz : 60);
    for (int i = 0; i < 16; i++) { // never more than 16 vblanks
        int64_t now = swap_now_ns();
        if (now >= at - period - period / 2) break; // the flip, at the next vblank, is on time
        if (sceVideoOutWaitVblank(vblank_handle) != 0) break;
    }
}

int video_player_swap(void *dpy, void *surf, unsigned (*swap)(void *, void *)); // video_player.c
static EGLDisplay qm_dpy;
static EGLSurface qm_surf;
static void qm_present(void *arg) { (void)arg; eglSwapBuffers(qm_dpy, qm_surf); } // the Quick menu's frames
static EGLBoolean b_eglSwapBuffers(EGLDisplay dpy, EGLSurface s) {
    if (video_player_swap(dpy, s, (unsigned (*)(void *, void *))eglSwapBuffers)) return EGL_TRUE; // a movie played
    if (frames == 0) eglSwapInterval(dpy, app_fps_cap_hz() == 30 ? 2 : 1); // (a cap of 30: every other refresh)
    if (frames == 60 || frames == 150 || frames == 250 || frames == 300 || frames == 330) frame_probe();
    { // a new GL context: the overlay's objects made again in it (gl_overlay.inc ovl_new_context)
        static EGLContext ctx_of_overlay;
        EGLContext ctx = eglGetCurrentContext();
        if (ctx != ctx_of_overlay) {
            if (ctx_of_overlay) droid_log("gl: a new GL context: the cursor and the stats panel made again in it");
            ctx_of_overlay = ctx;
            ovl_new_context();
        }
        if (quick_menu_wanted()) { // Options held: the Quick menu on this frame (quick_menu.c)
            qm_dpy = dpy, qm_surf = s;
            quick_menu_run(ctx, qm_present, NULL);
        }
    }
    draw_fps_overlay();
    present_wait(); // Swappy's time for this frame (pace_swap guessed it: off)
    int64_t t0 = swap_now_ns();
    EGLBoolean ok = eglSwapBuffers(dpy, s);
    int64_t t1 = swap_now_ns();
    // a swap that waited (over 1.5 ms) returned just after a vblank
    if (t1 - t0 > 1500000) java_vsync(t1);
    static int64_t waited_ns, window_swaps, waits;
    waited_ns += t1 - t0;
    window_swaps++;
    if (t1 - t0 > 1500000) waits++;
    if (window_swaps == 300) {
        droid_log("gl: last 300 swaps: %.1f ms each on average, %lld waited for a refresh",
                  waited_ns / 300.0 / 1e6, (long long)waits);
        waited_ns = window_swaps = waits = 0;
    }
    frames++;
    if (frames == 1) {
        screen_ready(dpy);
        droid_startup_frame("OpenGL ES, ps5-opengl");
    }
    if (frames == 1 || frames % 600 == 0)
        droid_log("gl: frame %lu (swap %d, gl error %#x)", frames, ok, glGetError());
    return ok;
}

void *egl_resolve(const char *name);

void *texspy_wrap(const char *name, void *real); // texspy.c
void *video_gl_wrap(const char *name, void *real); // video_tex.c
void *phone_gl_wrap(const char *name, void *real, void *(*resolver)(const char *)); // phone_gl.c
static void *b_eglGetProcAddress(const char *name) {
    return phone_gl_wrap(name, video_gl_wrap(name, texspy_wrap(name, egl_resolve(name))), egl_resolve);
}

// ---- OpenGL ES -> desktop core ------------------------------------------------

static void b_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings,
                             const GLint *lengths) {
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *text = malloc(total + 32);
    if (!text) return;
    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(text + at, strings[i], n);
        at += n;
    }
    text[at] = 0;
    // GLSL ES 1.00 when the game wrote no #version (ES 2's default).
    const char *src = text;
    char *with_version = NULL;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "#version", 8) != 0) {
        with_version = malloc(at + 16);
        if (with_version) {
            memcpy(with_version, "#version 100\n", 13);
            memcpy(with_version + 13, text, at + 1);
            src = with_version;
        }
    }
    glShaderSource(shader, 1, &src, NULL);
    free(with_version);
    free(text);
}

static void b_glCompileShader(GLuint shader) {
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        droid_log("gl: shader %u failed to compile: %s", shader, log);
    }
}

static void b_glLineWidth(GLfloat width) {
    (void)width;
    glLineWidth(1.0f); // core contexts refuse wide lines
}

static int hide_ext(const char *e);
static void ext_list(void);
static int ext_index[1024], ext_count = -1;

static const GLubyte *b_glGetString(GLenum name) {
    switch (name) {
    case GL_VERSION: return (const GLubyte *)"OpenGL ES 3.0 (ps5-opengl)";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 3.00";
    case GL_EXTENSIONS: { // ES has the list as one string; core GL only by index
        static char *all;
        if (!all) {
            ext_list();
            size_t cap = 65536, at = 0;
            all = calloc(1, cap);
            for (int i = 0; all && i < ext_count; i++) {
                const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)ext_index[i]);
                if (e && at + strlen(e) + 2 < cap) at += (size_t)snprintf(all + at, cap - at, "%s ", e);
            }
        }
        return (const GLubyte *)all;
    }
    default: return glGetString(name);
    }
}

// The version numbers too (desktop 4.6 underneath): Unity writes "#version <major><minor>0 es"
// from these, and there is no GLSL ES 4.60.
// The extensions the game sees, ASTC hidden only by the game's settings (then Unity
// unpacks ASTC itself at load; it didn't change the 1.4 s swaps while a game loads).
static int hide_ext(const char *e) {
    static int keep_astc = -1;
    if (keep_astc < 0) {
        keep_astc = !settings.hide_astc;
        droid_log("gl: ASTC %s", keep_astc ? "shown" : "hidden from the game (its settings)");
    }
    return !keep_astc && e && strstr(e, "texture_compression_astc") != NULL;
}
static void ext_list(void) {
    if (ext_count >= 0) return;
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    ext_count = 0;
    for (GLint i = 0; i < n && ext_count < 1024; i++)
        if (!hide_ext((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i))) ext_index[ext_count++] = i;
}
static const GLubyte *b_glGetStringi(GLenum name, GLuint i) {
    if (name != GL_EXTENSIONS) return glGetStringi(name, i);
    ext_list();
    return i < (GLuint)ext_count ? glGetStringi(GL_EXTENSIONS, (GLuint)ext_index[i]) : NULL;
}

static void b_glGetIntegerv(GLenum name, GLint *v) {
    switch (name) {
    case GL_MAJOR_VERSION: *v = 3; return;
    case GL_MINOR_VERSION: *v = 0; return;
    case GL_NUM_EXTENSIONS: ext_list(); *v = ext_count; return;
    case GL_NUM_PROGRAM_BINARY_FORMATS: // Unreal: its program binary cache off (0-byte binaries)
        if (droid_quirks & QUIRK_UNREAL) { *v = 0; return; }
        glGetIntegerv(name, v);
        return;
    default: glGetIntegerv(name, v);
    }
}

// ES's LUMINANCE/ALPHA formats are gone from core: one or two channels,
// swizzled back to what the shader expects.
static void es_format(GLenum format, GLenum *core, const GLint **swizzle) {
    static const GLint lum[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
    static const GLint lum_alpha[4] = { GL_RED, GL_RED, GL_RED, GL_GREEN };
    static const GLint alpha[4] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
    *swizzle = NULL;
    *core = format;
    if (format == GL_LUMINANCE) { *core = GL_RED; *swizzle = lum; }
    else if (format == GL_LUMINANCE_ALPHA) { *core = GL_RG; *swizzle = lum_alpha; }
    else if (format == GL_ALPHA_ES) { *core = GL_RED; *swizzle = alpha; }
}

static void b_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                           GLint border, GLenum format, GLenum type, const void *pixels) {
    GLenum core;
    const GLint *swizzle;
    es_format(format, &core, &swizzle);
    if (swizzle) {
        glTexImage2D(target, level, core == GL_RG ? GL_RG8 : GL_R8, w, h, border, core, type,
                     pixels);
        glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        return;
    }
    glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
    gl_check("glTexImage2D", internal, w, h, format);
}

static void b_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                              GLenum format, GLenum type, const void *pixels) {
    GLenum core;
    const GLint *swizzle;
    es_format(format, &core, &swizzle);
    glTexSubImage2D(target, level, x, y, w, h, core, type, pixels);
}

static void b_glClearDepthf(GLfloat d) { glClearDepth(d); }
static void b_glDepthRangef(GLfloat n, GLfloat f) { glDepthRange(n, f); }
static void b_glReleaseShaderCompiler(void) {}

// ---- the table ---------------------------------------------------------------

// ---- glBlitFramebuffer to the screen, as a textured quad --------------------------
// Godot (and others) draw into their own framebuffer and blit it to the window
// every frame, often scaled; ps5-opengl's blit took ~116 ms a frame there. Drawn
// with a shader instead (same result), the game's GL state put back afterwards.

static GLuint blit_prog, blit_vao, blit_samplers[2];
static GLint blit_u_src, blit_u_dst;

static GLuint blit_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        droid_log("gl: blit shader: %s", log);
    }
    return s;
}

static int blit_setup(void) {
    if (blit_prog) return 1;
    GLuint vs = blit_shader(GL_VERTEX_SHADER,
        "#version 330 core\n"
        "uniform vec4 src; uniform vec4 dst; out vec2 uv;\n"
        "void main() {\n"
        "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        "  uv = mix(src.xy, src.zw, c);\n"
        "  gl_Position = vec4(mix(dst.xy, dst.zw, c), 0.0, 1.0);\n"
        "}\n");
    GLuint fs = blit_shader(GL_FRAGMENT_SHADER,
        "#version 330 core\n"
        "uniform sampler2D t; in vec2 uv; out vec4 o;\n"
        "void main() { o = texture(t, uv); }\n");
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        droid_log("gl: blit program did not link; using the real blit");
        glDeleteProgram(p);
        return 0;
    }
    blit_prog = p;
    blit_u_src = glGetUniformLocation(p, "src");
    blit_u_dst = glGetUniformLocation(p, "dst");
    glGenVertexArrays(1, &blit_vao);
    glGenSamplers(2, blit_samplers);
    for (int i = 0; i < 2; i++) {
        GLint f = i ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(blit_samplers[i], GL_TEXTURE_MIN_FILTER, f);
        glSamplerParameteri(blit_samplers[i], GL_TEXTURE_MAG_FILTER, f);
        glSamplerParameteri(blit_samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(blit_samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return 1;
}

static void b_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                                GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    static int logged;
    GLint draw_fb = 0, read_fb = 0, type = 0, tex = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    if (read_fb) {
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
    }
    int quad = draw_fb == 0 && read_fb && mask == GL_COLOR_BUFFER_BIT && type == GL_TEXTURE &&
               tex && blit_setup();
    if (!logged++)
        droid_log("gl: blit fb %d -> %d src %d,%d-%d,%d dst %d,%d-%d,%d mask %#x %s, attachment "
                  "%#x tex %d: %s", read_fb, draw_fb, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask,
                  filter == GL_LINEAR ? "linear" : "nearest", type, tex,
                  quad ? "drawn as a quad" : "real blit");
    if (!quad) {
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
        return;
    }

    // the game's state
    GLint prog, vao, active, tex2d, sampler, viewport[4], color_mask_i[4];
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST),
              stencil = glIsEnabled(GL_STENCIL_TEST), srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB),
              color_mask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    (void)color_mask_i;

    // texture size: source rectangle in texture coordinates
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    GLint tw = 1, th = 1;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    GLint fw = viewport[2], fh = viewport[3];
    fw = 1920; // the window
    fh = 1080;
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, fw, fh);
    glUseProgram(blit_prog);
    glBindVertexArray(blit_vao);
    glBindSampler(0, blit_samplers[filter == GL_LINEAR]);
    glUniform4f(blit_u_src, (float)sx0 / tw, (float)sy0 / th, (float)sx1 / tw, (float)sy1 / th);
    glUniform4f(blit_u_dst, 2.0f * dx0 / fw - 1, 2.0f * dy0 / fh - 1, 2.0f * dx1 / fw - 1,
                2.0f * dy1 / fh - 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // put it back
    glBindSampler(0, (GLuint)sampler);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
}

#define B(name) { #name, (void *)b_##name }
static const struct { const char *name; void *fn; } table[] = {
    B(eglGetDisplay), B(eglInitialize), B(eglChooseConfig), B(eglGetConfigAttrib),
    B(eglCreateWindowSurface), B(eglCreateContext), B(eglMakeCurrent), B(eglSwapBuffers),
    B(eglGetProcAddress),
    B(eglQuerySurface), { "eglGetError", (void *)eglGetError },
    { "eglDestroyContext", (void *)eglDestroyContext },
    { "eglDestroySurface", (void *)eglDestroySurface }, { "eglTerminate", (void *)eglTerminate },
    B(eglSwapInterval), { "eglBindAPI", (void *)eglBindAPI },
    { "eglGetCurrentContext", (void *)eglGetCurrentContext },
    { "eglGetCurrentSurface", (void *)eglGetCurrentSurface },
    { "eglGetCurrentDisplay", (void *)eglGetCurrentDisplay },
    B(eglQueryString), { "eglQueryContext", (void *)eglQueryContext },
    B(eglPresentationTimeANDROID), B(eglGetFrameTimestampSupportedANDROID), B(eglGetNextFrameIdANDROID),
    B(eglGetFrameTimestampsANDROID),
    { "eglReleaseThread", (void *)eglReleaseThread }, { "eglWaitGL", (void *)eglWaitGL },
    { "eglGetConfigs", (void *)eglGetConfigs },
    B(glShaderSource), B(glCompileShader), B(glLineWidth), B(glGetString), B(glGetIntegerv), B(glGetStringi),
    B(glTexImage2D),
    B(glBlitFramebuffer), { "eglCreatePbufferSurface", (void *)eglCreatePbufferSurface },
    { "eglSurfaceAttrib", (void *)eglSurfaceAttrib },
    { "eglGetCurrentSurface", (void *)eglGetCurrentSurface },
    B(glTexSubImage2D), B(glClearDepthf), B(glDepthRangef), B(glReleaseShaderCompiler),
#define C(name) { #name, (void *)c_##name }
    C(glCompressedTexImage2D), C(glCompressedTexSubImage2D), C(glTexStorage2D),
    C(glRenderbufferStorageMultisample), C(glRenderbufferStorage), C(glFramebufferTexture2D),
    C(glCheckFramebufferStatus), C(glEnable), C(glDisable), C(glTexParameteri), C(glDrawBuffers),
    C(glInvalidateFramebuffer), C(glBindFramebuffer), C(glHint), C(glPixelStorei), C(glDrawElements),
    C(glDrawArrays), C(glBindBuffer), C(glVertexAttribPointer), C(glEnableVertexAttribArray),
    C(glDisableVertexAttribArray),
};

// ---- ANGLE (renderer setting "OpenGL ES on ANGLE"): the game's GLES and EGL on ANGLE, which
// draws with Vulkan (RADV) and shows its frames through VK_KHR_display (its "simple" display).
// No ES-to-desktop steps here: ANGLE is OpenGL ES. Ours only: the display and window ANGLE's
// simple display wants, and the swap's timing for the Choreographer (as the GL path's).
void *angle_resolve(const char *name); // angle_gles.c
int angle_available(void);
int droid_angle_auto; // app_main.c: an engine that's on ANGLE when the setting is Auto (cocos2d-x)
static int angle_on(void) {
    static int on = -1;
    if (on < 0) {
        int want = settings.renderer == RENDERER_ANGLE || (settings.renderer == RENDERER_AUTO && droid_angle_auto);
        on = want && angle_available();
        if (want)
            droid_log("gl: renderer ANGLE%s %s", settings.renderer == RENDERER_AUTO ? " (Auto, for this engine)" : "",
                      on ? "(OpenGL ES on Vulkan)" : "asked, but this build has none: ps5-opengl");
    }
    return on;
}
typedef EGLDisplay (*a_get_platform_display_fn)(EGLenum, void *, const EGLAttrib *);
typedef EGLBoolean (*a_initialize_fn)(EGLDisplay, EGLint *, EGLint *);
typedef EGLSurface (*a_create_window_fn)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
typedef EGLBoolean (*a_swap_fn)(EGLDisplay, EGLSurface);
typedef EGLint (*a_get_error_fn)(void);
typedef const char *(*a_query_string_fn)(EGLDisplay, EGLint);
typedef const GLubyte *(*a_get_string_fn)(GLenum);
static EGLint a_error(void) {
    static a_get_error_fn f;
    if (!f) f = (a_get_error_fn)angle_resolve("eglGetError");
    return f ? f() : 0;
}
// ANGLE's own reason for an EGL error (EGL_KHR_debug), in our log
static void a_debug(EGLenum err, const char *cmd, EGLint type, void *thread, void *obj, const char *msg) {
    (void)type; (void)thread; (void)obj;
    static int told;
    if (told++ < 60) droid_log("gl: ANGLE %s: error %#x: %s", cmd ? cmd : "?", err, msg ? msg : "");
}
static EGLDisplay a_eglGetDisplay(EGLNativeDisplayType native) {
    (void)native;
    typedef EGLint (*debug_control_fn)(void *, const EGLAttrib *);
    debug_control_fn dc = (debug_control_fn)angle_resolve("eglDebugMessageControlKHR");
    if (dc) {
        const EGLAttrib on[] = { 0x33BA /* ERROR */, EGL_TRUE, 0x33BB /* WARN */, EGL_TRUE, EGL_NONE };
        dc((void *)a_debug, on);
    }
    a_get_platform_display_fn gpd = (a_get_platform_display_fn)angle_resolve("eglGetPlatformDisplay");
    const EGLAttrib attrs[] = { 0x3203 /* PLATFORM_ANGLE_TYPE */, 0x3450 /* VULKAN */,
                                0x348F /* NATIVE_PLATFORM_TYPE */, 0x34A4 /* VULKAN_DISPLAY_MODE_SIMPLE */, EGL_NONE };
    EGLDisplay d = gpd ? gpd(0x3202 /* EGL_PLATFORM_ANGLE_ANGLE */, EGL_DEFAULT_DISPLAY, attrs) : EGL_NO_DISPLAY;
    droid_log("gl: ANGLE display %p (error %#x), the game asked for %p", d, d ? 0 : a_error(), (void *)native);
    return d;
}
static EGLBoolean a_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    a_initialize_fn init = (a_initialize_fn)angle_resolve("eglInitialize");
    EGLBoolean ok = init ? init(dpy, major, minor) : EGL_FALSE;
    a_query_string_fn qs = (a_query_string_fn)angle_resolve("eglQueryString");
    droid_log("gl: ANGLE eglInitialize -> %d (error %#x): %s", ok, ok ? 0 : a_error(),
              ok && qs ? qs(dpy, EGL_VERSION) : "");
    return ok;
}
// The game's config, from those that can be a window ("Bad match" at the window surface
// otherwise); none: a plain RGBA8, depth 24 / stencil 8 one. What was picked goes in the log.
typedef EGLBoolean (*a_choose_fn)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean (*a_config_attrib_fn)(EGLDisplay, EGLConfig, EGLint, EGLint *);
// The first config eglChooseConfig gave the game: its window and context get it when the game
// passes none (King's engine looked the list over, liked none of ANGLE's and passed 0:
// eglCreateWindowSurface "Invalid Config", Candy Crush's black screen)
static EGLConfig a_last_config;
// what the game asks of a config (once per attribute): what it looked for, in the log
static EGLBoolean a_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint attr, EGLint *v) {
    static a_config_attrib_fn f;
    if (!f) f = (a_config_attrib_fn)angle_resolve("eglGetConfigAttrib");
    EGLBoolean ok = f ? f(dpy, cfg, attr, v) : EGL_FALSE;
    static EGLint seen[48];
    static int n_seen;
    int known = 0;
    for (int i = 0; i < n_seen; i++) known |= seen[i] == attr;
    if (!known && n_seen < 48) {
        seen[n_seen++] = attr;
        droid_log("gl: ANGLE: the game asks a config's %#x -> %d (%s)", attr, v ? *v : 0, ok ? "ok" : "refused");
    }
    return ok;
}
typedef EGLContext (*a_create_context_fn)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLContext a_eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attribs) {
    static a_create_context_fn f;
    if (!f) f = (a_create_context_fn)angle_resolve("eglCreateContext");
    if (!cfg && a_last_config) {
        static int told;
        if (told++ < 2) droid_log("gl: ANGLE: the game's context without a config: the chosen one %p", a_last_config);
        cfg = a_last_config;
    }
    return f ? f(dpy, cfg, share, attribs) : EGL_NO_CONTEXT;
}
static EGLBoolean a_eglChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size,
                                    EGLint *count) {
    a_choose_fn choose = (a_choose_fn)angle_resolve("eglChooseConfig");
    a_config_attrib_fn get = (a_config_attrib_fn)angle_resolve("eglGetConfigAttrib");
    EGLint list[64];
    int n = 0, has_surface = 0;
    for (int i = 0; attribs && attribs[i] != EGL_NONE && n < 58; i += 2) {
        EGLint key = attribs[i], value = attribs[i + 1];
        if (key == EGL_SURFACE_TYPE) {
            value |= EGL_WINDOW_BIT;
            has_surface = 1;
        }
        list[n++] = key;
        list[n++] = value;
    }
    if (!has_surface) {
        list[n++] = EGL_SURFACE_TYPE;
        list[n++] = EGL_WINDOW_BIT;
    }
    list[n] = EGL_NONE;
    EGLint got = 0;
    EGLBoolean ok = choose(dpy, list, configs, size, &got);
    if (!ok || got == 0) {
        static const EGLint plain[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, 0x40 /* ES3 */,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
        ok = choose(dpy, plain, configs, size, &got);
        droid_log("gl: ANGLE: the game's config matched nothing; a plain RGBA8 d24s8 one -> %d (%d)", ok, got);
    }
    if (count) *count = got;
    if (ok && got > 0 && configs) a_last_config = configs[0];
    static int told_cfg;
    if (told_cfg++ < 8)
        droid_log("gl: ANGLE eglChooseConfig(display %p, %s, room %d) -> %d found, first %p", dpy,
                  configs ? "configs" : "count only", size, got, configs && got > 0 ? configs[0] : NULL);
    if (ok && got > 0 && configs && get) {
        EGLint r = 0, g = 0, b = 0, a = 0, d = 0, st = 0, sf = 0, rt = 0;
        get(dpy, configs[0], EGL_RED_SIZE, &r);
        get(dpy, configs[0], EGL_GREEN_SIZE, &g);
        get(dpy, configs[0], EGL_BLUE_SIZE, &b);
        get(dpy, configs[0], EGL_ALPHA_SIZE, &a);
        get(dpy, configs[0], EGL_DEPTH_SIZE, &d);
        get(dpy, configs[0], EGL_STENCIL_SIZE, &st);
        get(dpy, configs[0], EGL_SURFACE_TYPE, &sf);
        get(dpy, configs[0], EGL_RENDERABLE_TYPE, &rt);
        static int told;
        if (told++ < 4)
            droid_log("gl: ANGLE config: %d%d%d%d depth %d stencil %d, surfaces %#x, renderable %#x (%d found)", r, g,
                      b, a, d, st, sf, rt, got);
    }
    return ok;
}

// ANGLE's simple display: the "window" is the size of the picture (angle::vk::SimpleDisplayWindow)
static struct { uint16_t width, height; } angle_window = { 1920, 1080 };
// The PS5's display takes images of the mode's size only (4K on a 4K TV); the game draws its
// 1920x1080 window: on the screen (framebuffer 0) its viewport and scissor are scaled to the
// images, and the game is told its window's size. Its own framebuffers: as they are.
static int angle_surface_w = 1920, angle_surface_h = 1080, angle_on_screen = 1;
static int angle_used; // the game's window is ANGLE's
// The screen's real size for the phone view (phone_gl.c): ANGLE's images are the display's
// (3840x2160 on a 4K TV) while it tells the game 1920x1080; 0: as eglQuerySurface says
int gl_real_screen(int *w, int *h) {
    if (!angle_used) return 0;
    *w = angle_surface_w, *h = angle_surface_h;
    return 1;
}
typedef void (*a_viewport_fn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*a_bind_fb_fn)(GLenum, GLuint);
typedef EGLBoolean (*a_query_surface_fn2)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static void a_scaled(GLint *x, GLint *y, GLsizei *w, GLsizei *h) {
    if (!angle_on_screen || (angle_surface_w == angle_window.width && angle_surface_h == angle_window.height)) return;
    *x = (GLint)((int64_t)*x * angle_surface_w / angle_window.width);
    *y = (GLint)((int64_t)*y * angle_surface_h / angle_window.height);
    *w = (GLsizei)((int64_t)*w * angle_surface_w / angle_window.width);
    *h = (GLsizei)((int64_t)*h * angle_surface_h / angle_window.height);
}
// The game's own viewport and scissor (1920x1080 terms), kept: GL's viewport is one value for every
// framebuffer, and an engine sets it with its own framebuffer bound, then draws to the screen
// (Unity: a quarter of the screen). On every switch between its framebuffers and the screen they
// are set again, scaled or not; read back, the game gets its own values.
static GLint a_vp[4] = { 0, 0, 1920, 1080 }, a_sc[4] = { 0, 0, 1920, 1080 };
static a_viewport_fn a_real_viewport, a_real_scissor;
static void a_apply(void) {
    if (!a_real_viewport) a_real_viewport = (a_viewport_fn)angle_resolve("glViewport");
    if (!a_real_scissor) a_real_scissor = (a_viewport_fn)angle_resolve("glScissor");
    GLint x = a_vp[0], y = a_vp[1];
    GLsizei w = a_vp[2], h = a_vp[3];
    a_scaled(&x, &y, &w, &h);
    a_real_viewport(x, y, w, h);
    x = a_sc[0], y = a_sc[1], w = a_sc[2], h = a_sc[3];
    a_scaled(&x, &y, &w, &h);
    a_real_scissor(x, y, w, h);
}
static void a_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!a_real_viewport) a_real_viewport = (a_viewport_fn)angle_resolve("glViewport");
    a_vp[0] = x, a_vp[1] = y, a_vp[2] = w, a_vp[3] = h;
    a_scaled(&x, &y, &w, &h);
    a_real_viewport(x, y, w, h);
}
static void a_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!a_real_scissor) a_real_scissor = (a_viewport_fn)angle_resolve("glScissor");
    a_sc[0] = x, a_sc[1] = y, a_sc[2] = w, a_sc[3] = h;
    a_scaled(&x, &y, &w, &h);
    a_real_scissor(x, y, w, h);
}
static void a_glBindFramebuffer(GLenum target, GLuint fb) {
    static a_bind_fb_fn f;
    if (!f) f = (a_bind_fb_fn)angle_resolve("glBindFramebuffer");
    f(target, fb);
    if (target == 0x8CA8 /* GL_READ_FRAMEBUFFER */) return;
    int on = fb == 0;
    if (on != angle_on_screen) {
        angle_on_screen = on;
        a_apply();
    }
}
// A blit to the screen, its destination in the game's 1920x1080 terms: scaled to the screen's
// images, as its viewport is (the Infinity Blade III port presents its frame by a blit sized to the
// window: on the 4K images it filled a quarter, bottom left)
typedef void (*a_blit_fn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void a_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,
                                GLint dy1, GLbitfield mask, GLenum filter) {
    static a_blit_fn f;
    if (!f) f = (a_blit_fn)angle_resolve("glBlitFramebuffer");
    static int scale_blits = -1; // (per game: every other ANGLE game's blits as before)
    if (scale_blits < 0) scale_blits = droid_game_in("com.ib3port.,com.ib2port.,com.ib1port.") != 0;
    if (scale_blits && angle_on_screen && (angle_surface_w != angle_window.width || angle_surface_h != angle_window.height) &&
        angle_window.width > 0 && angle_window.height > 0) {
        dx0 = (GLint)((int64_t)dx0 * angle_surface_w / angle_window.width);
        dx1 = (GLint)((int64_t)dx1 * angle_surface_w / angle_window.width);
        dy0 = (GLint)((int64_t)dy0 * angle_surface_h / angle_window.height);
        dy1 = (GLint)((int64_t)dy1 * angle_surface_h / angle_window.height);
        if (filter == 0x2600 /* GL_NEAREST */ && (mask & ~0x4000u) == 0) filter = 0x2601; // (scaled: smooth)
        static int told;
        if (told++ < 3) droid_log("gl: ANGLE: a blit to the screen, scaled to %d,%d-%d,%d", dx0, dy0, dx1, dy1);
    }
    f(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
// Swap interval: always 1, as on ps5-opengl (b_eglSwapInterval) and Vulkan's FIFO - each frame
// waits for a refresh. Unity asks 0: ANGLE then showed frames as fast as they came (Vector at
// 120 fps, its game running fast: it moves things per frame)
typedef EGLBoolean (*a_swap_interval_fn)(EGLDisplay, EGLint);
static EGLBoolean a_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    static a_swap_interval_fn f;
    if (!f) f = (a_swap_interval_fn)angle_resolve("eglSwapInterval");
    static int told;
    if (told++ < 4) droid_log("gl: ANGLE: the game asks swap interval %d -> 1", interval);
    return f ? f(dpy, 1) : EGL_FALSE;
}
typedef void (*a_get_int_fn)(GLenum, GLint *);
static void a_glGetIntegerv(GLenum name, GLint *v) {
    static a_get_int_fn f;
    if (!f) f = (a_get_int_fn)angle_resolve("glGetIntegerv");
    if (v && name == GL_VIEWPORT) { memcpy(v, a_vp, sizeof(a_vp)); return; }
    if (v && name == GL_SCISSOR_BOX) { memcpy(v, a_sc, sizeof(a_sc)); return; }
    f(name, v);
}
static EGLBoolean a_eglQuerySurface(EGLDisplay dpy, EGLSurface s, EGLint attr, EGLint *v) {
    static a_query_surface_fn2 f;
    if (!f) f = (a_query_surface_fn2)angle_resolve("eglQuerySurface");
    EGLBoolean ok = f(dpy, s, attr, v);
    if (ok && v && attr == EGL_WIDTH) *v = angle_window.width;
    if (ok && v && attr == EGL_HEIGHT) *v = angle_window.height;
    return ok;
}
static EGLSurface a_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
                                           const EGLint *attribs) {
    (void)win;
    a_create_window_fn create = (a_create_window_fn)angle_resolve("eglCreateWindowSurface");
    if (!create) return EGL_NO_SURFACE;
    static int told_win;
    if (told_win++ < 6) {
        EGLint first = attribs ? attribs[0] : 0;
        droid_log("gl: ANGLE eglCreateWindowSurface(display %p, config %p, window %p, settings %s %#x)", dpy, cfg,
                  (void *)win, attribs ? "yes, first" : "none", first);
    }
    if (!cfg && a_last_config) {
        static int told_none;
        if (told_none++ < 2) droid_log("gl: ANGLE: the game's window without a config: the chosen one %p", a_last_config);
        cfg = a_last_config;
    }
    EGLSurface s = create(dpy, cfg, (EGLNativeWindowType)&angle_window, attribs);
    if (!s && attribs) { // (a setting of the game's ANGLE can't take: without them)
        droid_log("gl: ANGLE window surface with the game's settings refused (%#x); without", a_error());
        s = create(dpy, cfg, (EGLNativeWindowType)&angle_window, NULL);
    }
    EGLint w = 0, h = 0;
    typedef EGLBoolean (*a_query_surface_fn)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    a_query_surface_fn qsf = (a_query_surface_fn)angle_resolve("eglQuerySurface");
    if (s && qsf) {
        qsf(dpy, s, EGL_WIDTH, &w);
        qsf(dpy, s, EGL_HEIGHT, &h);
    }
    droid_log("gl: ANGLE window surface %p, %dx%d (error %#x)", s, w, h, s ? 0 : a_error());
    if (w > 0 && h > 0) {
        angle_surface_w = w;
        angle_surface_h = h;
        angle_used = 1;
        if (w != angle_window.width || h != angle_window.height)
            droid_log("gl: ANGLE: the game draws %dx%d, the screen's images are %dx%d: scaled", angle_window.width,
                      angle_window.height, w, h);
    }
    return s;
}
// The overlay (gl_overlay.inc) again, on ANGLE: its GL calls are ANGLE's (not ps5-opengl's, which
// draw nowhere in ANGLE mode), its shaders GLSL ES, its 1920x1080 scaled to the screen's images.
static __typeof__(&glActiveTexture) ao_glActiveTexture;
static __typeof__(&glAttachShader) ao_glAttachShader;
static __typeof__(&glBindBuffer) ao_glBindBuffer;
static __typeof__(&glBindFramebuffer) ao_glBindFramebuffer;
static __typeof__(&glBindSampler) ao_glBindSampler;
static __typeof__(&glBindTexture) ao_glBindTexture;
static __typeof__(&glBindVertexArray) ao_glBindVertexArray;
static __typeof__(&glBlendEquation) ao_glBlendEquation;
static __typeof__(&glBlendFunc) ao_glBlendFunc;
static __typeof__(&glBlendFuncSeparate) ao_glBlendFuncSeparate;
static __typeof__(&glClear) ao_glClear;
static __typeof__(&glClearColor) ao_glClearColor;
static __typeof__(&glColorMask) ao_glColorMask;
static __typeof__(&glCompileShader) ao_glCompileShader;
static __typeof__(&glCreateProgram) ao_glCreateProgram;
static __typeof__(&glCreateShader) ao_glCreateShader;
static __typeof__(&glDisable) ao_glDisable;
static __typeof__(&glDrawArrays) ao_glDrawArrays;
static __typeof__(&glEnable) ao_glEnable;
static __typeof__(&glGenTextures) ao_glGenTextures;
static __typeof__(&glGenVertexArrays) ao_glGenVertexArrays;
static __typeof__(&glGetBooleanv) ao_glGetBooleanv;
static __typeof__(&glGetFloatv) ao_glGetFloatv;
static __typeof__(&glGetIntegerv) ao_glGetIntegerv;
static __typeof__(&glGetProgramiv) ao_glGetProgramiv;
static __typeof__(&glGetUniformLocation) ao_glGetUniformLocation;
static __typeof__(&glIsEnabled) ao_glIsEnabled;
static __typeof__(&glLinkProgram) ao_glLinkProgram;
static __typeof__(&glReadPixels) ao_glReadPixels;
static __typeof__(&glScissor) ao_glScissor;
static __typeof__(&glShaderSource) ao_glShaderSource;
static __typeof__(&glTexImage2D) ao_glTexImage2D;
static __typeof__(&glTexParameteri) ao_glTexParameteri;
static __typeof__(&glUniform4f) ao_glUniform4f;
static __typeof__(&glUseProgram) ao_glUseProgram;
static __typeof__(&glViewport) ao_glViewport;
static __typeof__(&glTexSubImage2D) ao_glTexSubImage2D;
static __typeof__(&glPixelStorei) ao_glPixelStorei;
static __typeof__(&glUniform1f) ao_glUniform1f;
static __typeof__(&glDeleteTextures) ao_glDeleteTextures;
static int angle_overlay_load(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
    ok = 1;
    if (!(ao_glActiveTexture = (__typeof__(&glActiveTexture))angle_resolve("glActiveTexture"))) ok = 0;
    if (!(ao_glAttachShader = (__typeof__(&glAttachShader))angle_resolve("glAttachShader"))) ok = 0;
    if (!(ao_glBindBuffer = (__typeof__(&glBindBuffer))angle_resolve("glBindBuffer"))) ok = 0;
    if (!(ao_glBindFramebuffer = (__typeof__(&glBindFramebuffer))angle_resolve("glBindFramebuffer"))) ok = 0;
    if (!(ao_glBindSampler = (__typeof__(&glBindSampler))angle_resolve("glBindSampler"))) ok = 0;
    if (!(ao_glBindTexture = (__typeof__(&glBindTexture))angle_resolve("glBindTexture"))) ok = 0;
    if (!(ao_glBindVertexArray = (__typeof__(&glBindVertexArray))angle_resolve("glBindVertexArray"))) ok = 0;
    if (!(ao_glBlendEquation = (__typeof__(&glBlendEquation))angle_resolve("glBlendEquation"))) ok = 0;
    if (!(ao_glBlendFunc = (__typeof__(&glBlendFunc))angle_resolve("glBlendFunc"))) ok = 0;
    if (!(ao_glBlendFuncSeparate = (__typeof__(&glBlendFuncSeparate))angle_resolve("glBlendFuncSeparate"))) ok = 0;
    if (!(ao_glClear = (__typeof__(&glClear))angle_resolve("glClear"))) ok = 0;
    if (!(ao_glClearColor = (__typeof__(&glClearColor))angle_resolve("glClearColor"))) ok = 0;
    if (!(ao_glColorMask = (__typeof__(&glColorMask))angle_resolve("glColorMask"))) ok = 0;
    if (!(ao_glCompileShader = (__typeof__(&glCompileShader))angle_resolve("glCompileShader"))) ok = 0;
    if (!(ao_glCreateProgram = (__typeof__(&glCreateProgram))angle_resolve("glCreateProgram"))) ok = 0;
    if (!(ao_glCreateShader = (__typeof__(&glCreateShader))angle_resolve("glCreateShader"))) ok = 0;
    if (!(ao_glDisable = (__typeof__(&glDisable))angle_resolve("glDisable"))) ok = 0;
    if (!(ao_glDrawArrays = (__typeof__(&glDrawArrays))angle_resolve("glDrawArrays"))) ok = 0;
    if (!(ao_glEnable = (__typeof__(&glEnable))angle_resolve("glEnable"))) ok = 0;
    if (!(ao_glGenTextures = (__typeof__(&glGenTextures))angle_resolve("glGenTextures"))) ok = 0;
    if (!(ao_glGenVertexArrays = (__typeof__(&glGenVertexArrays))angle_resolve("glGenVertexArrays"))) ok = 0;
    if (!(ao_glGetBooleanv = (__typeof__(&glGetBooleanv))angle_resolve("glGetBooleanv"))) ok = 0;
    if (!(ao_glGetFloatv = (__typeof__(&glGetFloatv))angle_resolve("glGetFloatv"))) ok = 0;
    if (!(ao_glGetIntegerv = (__typeof__(&glGetIntegerv))angle_resolve("glGetIntegerv"))) ok = 0;
    if (!(ao_glGetProgramiv = (__typeof__(&glGetProgramiv))angle_resolve("glGetProgramiv"))) ok = 0;
    if (!(ao_glGetUniformLocation = (__typeof__(&glGetUniformLocation))angle_resolve("glGetUniformLocation"))) ok = 0;
    if (!(ao_glIsEnabled = (__typeof__(&glIsEnabled))angle_resolve("glIsEnabled"))) ok = 0;
    if (!(ao_glLinkProgram = (__typeof__(&glLinkProgram))angle_resolve("glLinkProgram"))) ok = 0;
    if (!(ao_glReadPixels = (__typeof__(&glReadPixels))angle_resolve("glReadPixels"))) ok = 0;
    if (!(ao_glScissor = (__typeof__(&glScissor))angle_resolve("glScissor"))) ok = 0;
    if (!(ao_glShaderSource = (__typeof__(&glShaderSource))angle_resolve("glShaderSource"))) ok = 0;
    if (!(ao_glTexImage2D = (__typeof__(&glTexImage2D))angle_resolve("glTexImage2D"))) ok = 0;
    if (!(ao_glTexParameteri = (__typeof__(&glTexParameteri))angle_resolve("glTexParameteri"))) ok = 0;
    if (!(ao_glUniform4f = (__typeof__(&glUniform4f))angle_resolve("glUniform4f"))) ok = 0;
    if (!(ao_glUseProgram = (__typeof__(&glUseProgram))angle_resolve("glUseProgram"))) ok = 0;
    if (!(ao_glViewport = (__typeof__(&glViewport))angle_resolve("glViewport"))) ok = 0;
    if (!(ao_glTexSubImage2D = (__typeof__(&glTexSubImage2D))angle_resolve("glTexSubImage2D"))) ok = 0;
    if (!(ao_glPixelStorei = (__typeof__(&glPixelStorei))angle_resolve("glPixelStorei"))) ok = 0;
    if (!(ao_glUniform1f = (__typeof__(&glUniform1f))angle_resolve("glUniform1f"))) ok = 0;
    if (!(ao_glDeleteTextures = (__typeof__(&glDeleteTextures))angle_resolve("glDeleteTextures"))) ok = 0;
    droid_log("gl: ANGLE overlay (fps counter, cursor, mapper): %s", ok ? "ready" : "a GL function missing");
    return ok;
}
#define glActiveTexture ao_glActiveTexture
#define glAttachShader ao_glAttachShader
#define glBindBuffer ao_glBindBuffer
#define glBindFramebuffer ao_glBindFramebuffer
#define glBindSampler ao_glBindSampler
#define glBindTexture ao_glBindTexture
#define glBindVertexArray ao_glBindVertexArray
#define glBlendEquation ao_glBlendEquation
#define glBlendFunc ao_glBlendFunc
#define glBlendFuncSeparate ao_glBlendFuncSeparate
#define glClear ao_glClear
#define glClearColor ao_glClearColor
#define glColorMask ao_glColorMask
#define glCompileShader ao_glCompileShader
#define glCreateProgram ao_glCreateProgram
#define glCreateShader ao_glCreateShader
#define glDisable ao_glDisable
#define glDrawArrays ao_glDrawArrays
#define glEnable ao_glEnable
#define glGenTextures ao_glGenTextures
#define glGenVertexArrays ao_glGenVertexArrays
#define glGetBooleanv ao_glGetBooleanv
#define glGetFloatv ao_glGetFloatv
#define glGetIntegerv ao_glGetIntegerv
#define glGetProgramiv ao_glGetProgramiv
#define glGetUniformLocation ao_glGetUniformLocation
#define glIsEnabled ao_glIsEnabled
#define glLinkProgram ao_glLinkProgram
#define glReadPixels ao_glReadPixels
#define glScissor ao_glScissor
#define glShaderSource ao_glShaderSource
#define glTexImage2D ao_glTexImage2D
#define glTexParameteri ao_glTexParameteri
#define glUniform4f ao_glUniform4f
#define glUseProgram ao_glUseProgram
#define glViewport ao_glViewport
#define glTexSubImage2D ao_glTexSubImage2D
#define glPixelStorei ao_glPixelStorei
#define glUniform1f ao_glUniform1f
#define glDeleteTextures ao_glDeleteTextures
#define OVL_NAME(x) angle_##x
#define OVL_S (angle_surface_w >= 1920 ? angle_surface_w / 1920 : 1)
#define OVL_SURF_W angle_surface_w
#define OVL_SURF_H angle_surface_h
#define OVL_DESKTOP 0
#define OVL_GLSL_VS "#version 300 es\n"
#define OVL_GLSL_FS "#version 300 es\nprecision mediump float;\n"
#include "gl_overlay.inc"
#undef OVL_NAME
#undef OVL_S
#undef OVL_SURF_W
#undef OVL_SURF_H
#undef OVL_DESKTOP
#undef OVL_GLSL_VS
#undef OVL_GLSL_FS
#undef glActiveTexture
#undef glAttachShader
#undef glBindBuffer
#undef glBindFramebuffer
#undef glBindSampler
#undef glBindTexture
#undef glBindVertexArray
#undef glBlendEquation
#undef glBlendFunc
#undef glBlendFuncSeparate
#undef glClear
#undef glClearColor
#undef glColorMask
#undef glCompileShader
#undef glCreateProgram
#undef glCreateShader
#undef glDisable
#undef glDrawArrays
#undef glEnable
#undef glGenTextures
#undef glGenVertexArrays
#undef glGetBooleanv
#undef glGetFloatv
#undef glGetIntegerv
#undef glGetProgramiv
#undef glGetUniformLocation
#undef glIsEnabled
#undef glLinkProgram
#undef glReadPixels
#undef glScissor
#undef glShaderSource
#undef glTexImage2D
#undef glTexParameteri
#undef glUniform4f
#undef glUseProgram
#undef glViewport
#undef glTexSubImage2D
#undef glPixelStorei
#undef glUniform1f
#undef glDeleteTextures

// GL's own messages from ANGLE (KHR_debug): its errors and high-severity notes in our log - what
// it refused of the game's (a texture format it can't take: black textures)
static void a_gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei len, const GLchar *msg,
                       const void *user) {
    (void)source; (void)id; (void)len; (void)user;
    static int told;
    if (told++ < 80) droid_log("gl: ANGLE GL %s %#x: %s", type == 0x824C ? "error" : "note", severity, msg ? msg : "");
}
static void a_gl_debug_on(void) {
    typedef void (*debug_cb_fn)(void *, const void *);
    typedef void (*enable_fn)(GLenum);
    typedef void (*control_fn)(GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean);
    debug_cb_fn cb = (debug_cb_fn)angle_resolve("glDebugMessageCallbackKHR");
    enable_fn en = (enable_fn)angle_resolve("glEnable");
    control_fn ctl = (control_fn)angle_resolve("glDebugMessageControlKHR");
    if (!cb || !en) return;
    en(0x92E0); // GL_DEBUG_OUTPUT
    if (ctl) { // errors and high severity only (not every performance hint)
        ctl(0x1100 /* DONT_CARE */, 0x1100, 0x1100, 0, NULL, GL_FALSE);
        ctl(0x1100, 0x824C /* TYPE_ERROR */, 0x1100, 0, NULL, GL_TRUE);
        ctl(0x1100, 0x1100, 0x9146 /* SEVERITY_HIGH */, 0, NULL, GL_TRUE);
    }
    cb((void *)a_gl_debug, NULL);
    droid_log("gl: ANGLE GL errors go to the log");
}
static a_swap_fn a_qm_swap;
static void *a_qm_dpy, *a_qm_surf;
static void a_qm_present(void *arg) { (void)arg; a_qm_swap(a_qm_dpy, a_qm_surf); } // the Quick menu's frames
static EGLBoolean a_eglSwapBuffers(EGLDisplay dpy, EGLSurface s) {
    static a_swap_fn swap;
    if (frames == 0) a_gl_debug_on();
    if (!swap) swap = (a_swap_fn)angle_resolve("eglSwapBuffers");
    if (video_player_swap(dpy, s, (unsigned (*)(void *, void *))swap)) return EGL_TRUE; // a movie played
    if (angle_overlay_load()) {
        // the cursor's texture / program / vertex array belong to the context they were made in:
        // a game that makes a new context (Unity, after its loading) had none, the cursor gone
        // for good. Made again in the new one.
        typedef void *(*get_ctx_fn)(void);
        static get_ctx_fn get_ctx;
        static void *ctx_of_cursor;
        if (!get_ctx) get_ctx = (get_ctx_fn)angle_resolve("eglGetCurrentContext");
        void *ctx = get_ctx ? get_ctx() : NULL;
        if (ctx != ctx_of_cursor) {
            if (ctx_of_cursor) droid_log("gl: ANGLE: a new GL context: the cursor and the stats panel made again in it");
            ctx_of_cursor = ctx;
            angle_ovl_new_context();
        }
        if (quick_menu_wanted()) { // Options held: the Quick menu on this frame (quick_menu.c)
            a_qm_swap = swap, a_qm_dpy = dpy, a_qm_surf = s;
            angle_quick_menu_run(ctx, a_qm_present, NULL);
        }
        angle_draw_fps_overlay(); // ours on the finished frame
    }
    // the Vulkan path's pacing (vulkan_bridge.c): the PS5's display doesn't hold a FIFO present
    // back for the refresh, so the FPS cap / an even 60 is a wait before the present (Vector ran
    // at 120 here without it, its game too fast)
    void droid_pace_before_present(void), droid_pace_after_present(void);
    droid_pace_before_present();
    int64_t t0 = swap_now_ns();
    EGLBoolean ok = swap(dpy, s);
    int64_t t1 = swap_now_ns();
    droid_pace_after_present();
    if (t1 - t0 > 1500000) java_vsync(t1); // a swap that waited returned just after a vblank
    frames++;
    if (frames == 1) {
        droid_startup_frame("OpenGL ES, ANGLE");
        droid_screen_ready(); // the Choreographer on the video-out port's vblank (as Vulkan games)
        a_get_string_fn gs = (a_get_string_fn)angle_resolve("glGetString");
        if (gs) droid_log("gl: ANGLE: %s / %s", (const char *)gs(GL_RENDERER), (const char *)gs(GL_VERSION));
    }
    if (frames == 1 || frames % 600 == 0) droid_log("gl: ANGLE frame %lu (swap %d)", frames, ok);
    return ok;
}
static void *angle_egl_resolve(const char *name);
static void *a_eglGetProcAddress(const char *name) {
    return phone_gl_wrap(name, video_gl_wrap(name, texspy_wrap(name, angle_egl_resolve(name))), angle_egl_resolve);
}
// EGL 1.5's display call (the game's platform: Android's): our one ANGLE display, as eglGetDisplay's.
// ANGLE's own gave the game another display than the one its configs came from ("Invalid Config")
static EGLDisplay a_eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attrs) {
    (void)attrs;
    static EGLDisplay one;
    droid_log("gl: ANGLE: the game's eglGetPlatformDisplay(%#x, %p): our display", platform, native);
    if (!one) one = a_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return one;
}
static void *angle_egl_resolve(const char *name) {
    static const struct { const char *name; void *fn; } ours[] = {
        { "eglGetDisplay", (void *)a_eglGetDisplay }, { "eglInitialize", (void *)a_eglInitialize },
        { "eglChooseConfig", (void *)a_eglChooseConfig },
        { "eglQuerySurface", (void *)a_eglQuerySurface }, { "glViewport", (void *)a_glViewport },
        { "glScissor", (void *)a_glScissor }, { "glBindFramebuffer", (void *)a_glBindFramebuffer },
        { "glBlitFramebuffer", (void *)a_glBlitFramebuffer },
        { "glGetIntegerv", (void *)a_glGetIntegerv },
        { "eglSwapInterval", (void *)a_eglSwapInterval },
        { "glBindFramebufferOES", (void *)a_glBindFramebuffer },
        { "eglCreateWindowSurface", (void *)a_eglCreateWindowSurface },
        { "eglSwapBuffers", (void *)a_eglSwapBuffers }, { "eglGetProcAddress", (void *)a_eglGetProcAddress },
        { "eglGetPlatformDisplay", (void *)a_eglGetPlatformDisplay },
        { "eglGetConfigAttrib", (void *)a_eglGetConfigAttrib }, { "eglCreateContext", (void *)a_eglCreateContext },
        { "eglGetPlatformDisplayEXT", (void *)a_eglGetPlatformDisplay },
    };
    for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
        if (!strcmp(ours[i].name, name)) return ours[i].fn;
    return angle_resolve(name);
}

void *egl_resolve(const char *name) {
    if (angle_on()) return angle_egl_resolve(name);
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    if (!strncmp(name, "egl", 3)) {
        // EGL extensions the game fetches (Unreal: eglCreateSyncKHR & co., which it then calls
        // unchecked when the extension is listed): the console's EGL, or its EGL 1.5 core name
        void *fn = (void *)eglGetProcAddress(name);
        size_t n = strlen(name);
        if (!fn && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
            char core[128];
            memcpy(core, name, n - 3);
            core[n - 3] = 0;
            fn = (void *)eglGetProcAddress(core);
        }
        return fn;
    }
    if (strncmp(name, "gl", 2) != 0) return NULL;
    // Everything else: ps5-opengl's own function, also for ES's OES/EXT
    // spellings of core functions (glGenVertexArraysOES -> glGenVertexArrays).
    void *fn = (void *)eglGetProcAddress(name);
    if (fn) return fn;
    size_t n = strlen(name);
    if (n > 3 && n < 128 && (strcmp(name + n - 3, "OES") == 0 || strcmp(name + n - 3, "EXT") == 0)) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        return (void *)eglGetProcAddress(core);
    }
    return NULL;
}
