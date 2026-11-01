// ui_gl.c — the menus' GPU drawing (ui_gl.h). Every shape is a quad in one vertex
// buffer per frame, drawn in a few calls (one a texture change): ps5-opengl costs ~36 us
// a draw call, so 500 separate ones would take half a frame. Each quad's fragment
// shader works out its rounded corners and soft edge itself.

#include "ui_gl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_shim.h"

#ifdef DROID_PC
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <X11/Xlib.h>
static Display *xdpy;
static Window xwin;
#else
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <ps5_opengl_display_modes.h>
#endif

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static EGLContext ctx = EGL_NO_CONTEXT;
static GLuint prog, vao, vbo, white_tex;
static GLint u_screen, u_tex;

// a vertex: position, uv, colour, second colour (gradient), the quad's rect, and
// (radius, feather, mode, -)
typedef struct {
    float x, y, u, v;
    float c[4], c2[4];
    float rect[4];
    float p[4];
    float scr[3]; // where it lands on the screen (x, y) and its w: a 3D turn (ui_transform)
} vert;
#define MAX_VERTS (6 * 4096)
static vert verts[MAX_VERTS];
static int nverts;
static GLuint batch_tex;

enum { MODE_SHAPE = 0, MODE_IMAGE = 1, MODE_MASK = 2, MODE_FLAT = 3, MODE_RING = 4 };
static float ring_width; // the ring a push draws (MODE_RING), its thickness

static const char *vs_body =
    "in vec2 a_pos; in vec2 a_uv; in vec4 a_col; in vec4 a_col2; in vec4 a_rect; in vec4 a_p; in vec3 a_scr;\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_pos; out vec2 v_uv; out vec4 v_col; out vec4 v_col2; flat out vec4 v_rect; flat out vec4 v_p;\n"
    "void main() {\n"
    "  v_pos = a_pos; v_uv = a_uv; v_col = a_col; v_col2 = a_col2; v_rect = a_rect; v_p = a_p;\n"
    // w from the 3D turn: the shape's own coordinates (corners, uv) are interpolated in perspective
    "  gl_Position = vec4((a_scr.x / u_screen.x * 2.0 - 1.0) * a_scr.z, (1.0 - a_scr.y / u_screen.y * 2.0) * a_scr.z, 0.0, a_scr.z);\n"
    "}\n";
static const char *fs_body =
    "in vec2 v_pos; in vec2 v_uv; in vec4 v_col; in vec4 v_col2; flat in vec4 v_rect; flat in vec4 v_p;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_col;\n"
    "float box(vec2 p, vec2 half_size, float r) {\n"
    "  vec2 q = abs(p) - half_size + vec2(r);\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "void main() {\n"
    "  int mode = int(v_p.z + 0.5);\n"
    "  if (mode == 3) { o_col = v_col; return; }\n"
    "  if (mode == 2) { o_col = vec4(v_col.rgb, v_col.a * texture(u_tex, v_uv).r); return; }\n"
    "  vec2 half_size = v_rect.zw * 0.5;\n"
    "  float d = box(v_pos - (v_rect.xy + half_size), half_size, min(v_p.x, min(half_size.x, half_size.y)));\n"
    "  float f = max(v_p.y, 1.0);\n"
    "  if (mode == 4) d = abs(d + v_p.w * 0.5) - v_p.w * 0.5;\n" // a ring: thickness w inside the edge
    "  float a = clamp(0.5 - d / f, 0.0, 1.0);\n"
    // an image; v_col2.x / .y: a shine across it (where along the diagonal, how bright)
    "  if (mode == 1) { vec4 t = texture(u_tex, v_uv);\n"
    "    float d = (v_uv.x + v_uv.y) * 0.5 - v_col2.x;\n"
    "    vec3 c = t.rgb + (vec3(1.0) - t.rgb) * v_col2.y * exp(-d * d / 0.012);\n"
    "    o_col = vec4(c, t.a * a * v_col.a); return; }\n"
    "  float g = clamp((v_pos.y - v_rect.y) / max(v_rect.w, 1.0), 0.0, 1.0);\n"
    "  vec4 c = mix(v_col, v_col2, g);\n"
    "  o_col = vec4(c.rgb, c.a * a);\n"
    "}\n";

static GLuint compile(GLenum type, const char *head, const char *body) {
    const char *src[2] = { head, body };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        droid_log("ui: shader failed: %s", log);
    }
    return s;
}

static int gl_setup(void) {
    const char *ver = (const char *)glGetString(GL_VERSION);
    int es = ver && strstr(ver, "OpenGL ES") != NULL;
    const char *head = es ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, head, vs_body));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, head, fs_body));
    static const char *attrs[] = { "a_pos", "a_uv", "a_col", "a_col2", "a_rect", "a_p", "a_scr" };
    for (int i = 0; i < 7; i++) glBindAttribLocation(prog, (GLuint)i, attrs[i]);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    droid_log("ui: GL %s, program %s", ver ? ver : "?", ok ? "ok" : "link failed");
    if (!ok) return -1;
    u_screen = glGetUniformLocation(prog, "u_screen");
    u_tex = glGetUniformLocation(prog, "u_tex");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const size_t at[7] = { offsetof(vert, x), offsetof(vert, u), offsetof(vert, c), offsetof(vert, c2),
                           offsetof(vert, rect), offsetof(vert, p), offsetof(vert, scr) };
    const int n[7] = { 2, 2, 4, 4, 4, 4, 3 };
    for (int i = 0; i < 7; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, n[i], GL_FLOAT, GL_FALSE, sizeof(vert), (const void *)at[i]);
    }
    const uint8_t white[4] = { 255, 255, 255, 255 };
    white_tex = ui_texture_rgba(white, 1, 1);
    return 0;
}

int ui_open(void) {
#ifdef DROID_PC
    XInitThreads();
    xdpy = XOpenDisplay(NULL);
    if (!xdpy) return -1;
    xwin = XCreateSimpleWindow(xdpy, DefaultRootWindow(xdpy), 0, 0, UI_W, UI_H, 0, 0, 0);
    XStoreName(xdpy, xwin, "Android (PC test runner): launcher");
    XMapWindow(xdpy, xwin);
    XFlush(xdpy);
    dpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
    if (!eglInitialize(dpy, NULL, NULL)) return -2;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLNativeWindowType win = (EGLNativeWindowType)xwin;
#else
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglSetDisplayRefreshPS5(dpy, 60);
    eglGetError();
    if (!eglInitialize(dpy, NULL, NULL)) return -2;
    eglBindAPI(EGL_OPENGL_API);
    const EGLint cfg_attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                                EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
    EGLNativeWindowType win = (EGLNativeWindowType)0;
#endif
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        // (ANGLE on X11: only configs of the window's visual, 24-bit colour without alpha)
        const EGLint no_alpha[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                    EGL_RENDERABLE_TYPE, cfg_attr[9], EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
        if (!eglChooseConfig(dpy, no_alpha, &cfg, 1, &n) || n < 1) return -3;
        droid_log("ui: EGL config without alpha");
    }
    surf = eglCreateWindowSurface(dpy, cfg, win, NULL);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        droid_log("ui: EGL window %p context %p (error %#x)", surf, ctx, eglGetError());
        return -4;
    }
    eglSwapInterval(dpy, 1);
    return gl_setup();
}

void ui_close(void) {
    if (dpy == EGL_NO_DISPLAY) return;
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    eglTerminate(dpy); // ps5-opengl closes its video-out port: the game's driver opens its own
    dpy = EGL_NO_DISPLAY;
    surf = EGL_NO_SURFACE;
    ctx = EGL_NO_CONTEXT;
#ifdef DROID_PC
    if (xdpy) {
        XDestroyWindow(xdpy, xwin);
        XCloseDisplay(xdpy);
        xdpy = NULL;
    }
#endif
    droid_log("ui: closed, the screen is the game's");
}

static GLuint shape_tex(void);

static void flush(void) {
    if (!nverts) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * sizeof(vert)), verts, GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_2D, shape_tex());
    glDrawArrays(GL_TRIANGLES, 0, nverts);
    nverts = 0;
}

void ui_frame_begin(void) {
#ifdef DROID_PC
    while (XPending(xdpy)) { // keep the window alive
        XEvent e;
        XNextEvent(xdpy, &e);
    }
#endif
    glViewport(0, 0, UI_W, UI_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glUniform2f(u_screen, UI_W, UI_H);
    glUniform1i(u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    batch_tex = 0;
    nverts = 0;
}

void ui_shot(const char *name) {
#ifdef DROID_PC
    flush();
    static uint8_t px[UI_W * UI_H * 4];
    glReadPixels(0, 0, UI_W, UI_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(name, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
        for (int y = UI_H - 1; y >= 0; y--)
            for (int x = 0; x < UI_W; x++) fwrite(px + ((size_t)y * UI_W + x) * 4, 1, 3, f);
        fclose(f);
    }
#else
    (void)name;
#endif
}

void ui_frame_end(void) {
    flush();
#ifdef DROID_PC
    static int frames; // PC tests: the frame at 150 / 360 as ui_shot_<n>.ppm
    if (++frames == 150 || frames == 360) {
        char name[64];
        snprintf(name, sizeof(name), "ui_shot_%d.ppm", frames);
        ui_shot(name);
    }
#endif
    eglSwapBuffers(dpy, surf);
}

// ---- the 3D turn: what's drawn after ui_transform turns about (cx, cy) and moves back by z,
// seen through a perspective (the menus' swing and flip). Off: the screen as it is.
#define UI_FOCAL 1600.0f
static int xf_on;
static float xf_cx, xf_cy, xf_z, xf_scale, xf_sinx, xf_cosx, xf_siny, xf_cosy;
void ui_transform(float cx, float cy, float rot_x, float rot_y, float z, float scale) {
    xf_on = 1;
    xf_cx = cx, xf_cy = cy, xf_z = z, xf_scale = scale;
    xf_sinx = sinf(rot_x), xf_cosx = cosf(rot_x), xf_siny = sinf(rot_y), xf_cosy = cosf(rot_y);
}
void ui_transform_reset(void) { xf_on = 0; }
static void project(float x, float y, float out[3]) {
    if (!xf_on) {
        out[0] = x, out[1] = y, out[2] = 1;
        return;
    }
    float px = (x - xf_cx) * xf_scale, py = (y - xf_cy) * xf_scale;
    float x1 = px * xf_cosy, z1 = -px * xf_siny;                                    // about the vertical axis
    float y2 = py * xf_cosx - z1 * xf_sinx, z2 = py * xf_sinx + z1 * xf_cosx + xf_z; // about the horizontal one
    float w = (UI_FOCAL + z2) / UI_FOCAL;
    if (w < 0.05f) w = 0.05f;
    out[0] = xf_cx + x1 / w, out[1] = xf_cy + y2 / w, out[2] = w;
}

// one quad (two triangles) of four corners, with the rect / params every vertex carries
static void push(GLuint tex, const float xy[8], const float uv[8], ui_color c, ui_color c2, float rx, float ry,
                 float rw, float rh, float radius, float feather, int mode) {
    if (tex != batch_tex) {
        flush();
        batch_tex = tex;
    }
    if (nverts + 6 > MAX_VERTS) flush();
    static const int order[6] = { 0, 1, 2, 2, 1, 3 };
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        vert *v = &verts[nverts++];
        v->x = xy[i * 2];
        v->y = xy[i * 2 + 1];
        v->u = uv ? uv[i * 2] : 0;
        v->v = uv ? uv[i * 2 + 1] : 0;
        memcpy(v->c, &c, sizeof(v->c));
        memcpy(v->c2, &c2, sizeof(v->c2));
        v->rect[0] = rx;
        v->rect[1] = ry;
        v->rect[2] = rw;
        v->rect[3] = rh;
        v->p[0] = radius;
        v->p[1] = feather;
        v->p[2] = (float)mode;
        v->p[3] = ring_width;
        project(v->x, v->y, v->scr);
    }
}

// the texture a shape goes with: the batch's own (shapes don't sample it), unless none is set
static GLuint shape_tex(void) { return batch_tex ? batch_tex : white_tex; }

static void rect_corners(float x, float y, float w, float h, float grow, float out[8]) {
    out[0] = x - grow;     out[1] = y - grow;     // top left
    out[2] = x + w + grow; out[3] = y - grow;     // top right
    out[4] = x - grow;     out[5] = y + h + grow; // bottom left
    out[6] = x + w + grow; out[7] = y + h + grow; // bottom right
}

void ui_rect(float x, float y, float w, float h, float radius, float feather, ui_color col) {
    float xy[8];
    rect_corners(x, y, w, h, feather, xy); // room for the soft edge outside the rect
    push(shape_tex(), xy, NULL, col, col, x, y, w, h, radius, feather, MODE_SHAPE);
}

void ui_rect_gradient(float x, float y, float w, float h, float radius, ui_color top, ui_color bottom) {
    float xy[8];
    rect_corners(x, y, w, h, 1, xy);
    push(shape_tex(), xy, NULL, top, bottom, x, y, w, h, radius, 1, MODE_SHAPE);
}

void ui_ring(float cx, float cy, float r, float thickness, float feather, ui_color col) {
    float xy[8];
    rect_corners(cx - r, cy - r, r * 2, r * 2, feather, xy);
    ring_width = thickness;
    push(shape_tex(), xy, NULL, col, col, cx - r, cy - r, r * 2, r * 2, r, feather, MODE_RING);
    ring_width = 0;
}

void ui_quad(const float xy[8], ui_color col) {
    push(shape_tex(), xy, NULL, col, col, 0, 0, 1, 1, 0, 1, MODE_FLAT);
}

// drawing only inside a band of the screen (rows y0..y1): the game grid under the logo
void ui_clip(float y0, float y1) {
    flush(); // what's batched goes first, unclipped
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, (GLint)(UI_H - y1), UI_W, (GLsizei)(y1 - y0)); // (GL's y is from the bottom)
}
void ui_clip_off(void) {
    flush();
    glDisable(GL_SCISSOR_TEST);
}

unsigned ui_texture_rgba(const uint8_t *rgba, int w, int h) {
    flush(); // what's batched is drawn with its own texture first
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    batch_tex = 0; // bound again at the next draw
    return t;
}

unsigned ui_texture_mask(const uint8_t *mask, int w, int h) {
    flush();
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, mask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    batch_tex = 0;
    return t;
}

void ui_texture_free(unsigned tex) {
    flush();
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

void ui_image(unsigned tex, float x, float y, float w, float h, float radius, float alpha) {
    ui_image_shine(tex, x, y, w, h, radius, alpha, 0, 0);
}

void ui_image_shine(unsigned tex, float x, float y, float w, float h, float radius, float alpha, float at,
                    float strength) {
    float xy[8], uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    rect_corners(x, y, w, h, 0, xy);
    ui_color c = { 1, 1, 1, alpha }, shine = { at, strength, 0, 0 };
    push(tex, xy, uv, c, shine, x, y, w, h, radius, 1, MODE_IMAGE);
}

void ui_mask(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, ui_color col) {
    float xy[8], uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    rect_corners(x, y, w, h, 0, xy);
    push(tex, xy, uv, col, col, x, y, w, h, 0, 1, MODE_MASK);
}
