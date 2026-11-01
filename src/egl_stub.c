// egl_stub.c â€” step 4a: EGL and OpenGL ES calls answered without a GPU, so
// a game can boot and run its loop before ps5-opengl is linked in. Frames are
// counted; each swap shows the game's clear colour on the screen.

#include <stdint.h>
#include <string.h>

#include "android_shim.h"
#include "display.h"

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_SUCCESS 0x3000
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_COLOR_BUFFER_BIT 0x4000

static uint32_t clear_color = 0xFF000000;
static GLuint next_id = 1;
static unsigned long draws, frames, shaders;
static int back;

// ---- EGL -----------------------------------------------------------------

static void *eglGetDisplay(void *native) { (void)native; return (void *)1; }

static EGLBoolean eglInitialize(void *dpy, EGLint *major, EGLint *minor) {
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 4;
    droid_log("egl: initialize (step 4a stand-in, no GPU yet)");
    return 1;
}

static EGLBoolean eglChooseConfig(void *dpy, const EGLint *attribs, void **configs, EGLint size,
                                  EGLint *count) {
    (void)dpy; (void)attribs;
    if (configs && size > 0) configs[0] = (void *)1;
    if (count) *count = 1;
    return 1;
}

// What an ES 3 phone's config says (Unity checks RENDERABLE_TYPE for ES 3).
static EGLBoolean eglGetConfigAttrib(void *dpy, void *cfg, EGLint attr, EGLint *value) {
    (void)dpy; (void)cfg;
    if (!value) return 1;
    switch (attr) {
    case 0x3040: case 0x3042: *value = 0x4D; break; // RENDERABLE_TYPE, CONFORMANT: ES|ES2|ES3|GL
    case 0x3033: *value = 0x5; break;               // SURFACE_TYPE: PBUFFER|WINDOW
    case 0x3025: *value = 24; break;                // DEPTH_SIZE
    case 0x3026: *value = 8; break;                 // STENCIL_SIZE
    case 0x3031: case 0x3032: *value = 0; break;    // SAMPLES, SAMPLE_BUFFERS
    case 0x3027: *value = 0x3038; break;            // CONFIG_CAVEAT: EGL_NONE
    case 0x3028: *value = 1; break;                 // CONFIG_ID
    default: *value = 8; break;                     // RGBA sizes and the rest
    }
    return 1;
}

static void *eglCreateWindowSurface(void *dpy, void *cfg, void *win, const EGLint *attribs) {
    (void)dpy; (void)cfg; (void)attribs;
    droid_log("egl: window surface for window %p", win);
    return (void *)2;
}

static void *eglCreateContext(void *dpy, void *cfg, void *share, const EGLint *attribs) {
    (void)dpy; (void)cfg; (void)share;
    int version = 1;
    for (int i = 0; attribs && attribs[i] != 0x3038 /* EGL_NONE */; i += 2)
        if (attribs[i] == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */) version = attribs[i + 1];
    droid_log("egl: OpenGL ES %d context", version);
    return (void *)3;
}

static EGLBoolean eglMakeCurrent(void *d, void *dr, void *rd, void *c) {
    (void)d; (void)dr; (void)rd; (void)c;
    return 1;
}

static EGLBoolean eglQuerySurface(void *dpy, void *s, EGLint attr, EGLint *value) {
    (void)dpy; (void)s;
    if (!value) return 0;
    if (attr == EGL_WIDTH) *value = DISPLAY_WIDTH;
    else if (attr == EGL_HEIGHT) *value = DISPLAY_HEIGHT;
    else *value = 0;
    return 1;
}

static EGLBoolean eglSwapBuffers(void *dpy, void *s) {
    (void)dpy; (void)s;
    display_fill(back, clear_color);
    display_flip(back, (int64_t)frames);
    back ^= 1;
    frames++;
    if (frames == 1 || frames % 600 == 0)
        droid_log("egl: frame %lu (%lu draws so far, %lu shaders)", frames, draws, shaders);
    return 1;
}

static EGLint eglGetError(void) { return EGL_SUCCESS; }
static EGLBoolean egl_true(void) { return 1; }
static void *eglGetCurrentContext(void) { return (void *)3; }
static const char *eglQueryString(void *dpy, EGLint name) { (void)dpy; (void)name; return ""; }
static void *eglGetProcAddress(const char *name) { return droid_resolve(name); }
static void *eglCreatePbufferSurface(void *dpy, void *cfg, const EGLint *attribs) {
    (void)dpy; (void)cfg; (void)attribs;
    return (void *)4;
}
static EGLBoolean eglSurfaceAttrib(void *dpy, void *s, EGLint attr, EGLint value) {
    (void)dpy; (void)s; (void)attr; (void)value;
    return 1;
}
static void *eglGetCurrentSurface(EGLint which) { (void)which; return (void *)2; }
static void *eglGetCurrentDisplay(void) { return (void *)1; }

// ---- OpenGL ES 2 -----------------------------------------------------------

static void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    // The PS5 framebuffer is A8B8G8R8.
    uint32_t R = (uint32_t)(r * 255.0f), G = (uint32_t)(g * 255.0f), B = (uint32_t)(b * 255.0f);
    uint32_t A = (uint32_t)(a * 255.0f);
    clear_color = (A << 24) | (B << 16) | (G << 8) | R;
}

static void glClear(GLbitfield mask) { (void)mask; }
static void glDrawArrays(GLenum m, GLint f, GLsizei c) { (void)m; (void)f; (void)c; draws++; }
static void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    (void)m; (void)c; (void)t; (void)i;
    draws++;
}

static GLuint glCreateShader(GLenum type) { (void)type; shaders++; return next_id++; }
static GLuint glCreateProgram(void) { return next_id++; }

static void gen_ids(GLsizei n, GLuint *ids) {
    for (GLsizei i = 0; i < n; i++) ids[i] = next_id++;
}

static void glGenBuffers(GLsizei n, GLuint *ids) { gen_ids(n, ids); }
static void glGenTextures(GLsizei n, GLuint *ids) { gen_ids(n, ids); }

static void glGetShaderiv(GLuint s, GLenum pname, GLint *v) {
    (void)s;
    *v = pname == GL_COMPILE_STATUS ? 1 : 0;
}

static void glGetProgramiv(GLuint p, GLenum pname, GLint *v) {
    (void)p;
    *v = pname == GL_LINK_STATUS ? 1 : 0;
}

static void gl_get_log(GLuint o, GLsizei max, GLsizei *len, char *log) {
    (void)o;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}

static GLint gl_location(GLuint p, const char *name) { (void)p; (void)name; return 0; }
static GLenum glGetError(void) { return 0; }
static void gl_nop(void) {}

// An OpenGL ES 3.2 device, as far as queries go.
static const unsigned char *glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return (const unsigned char *)"PS5 (PC test runner)";         // VENDOR
    case 0x1F01: return (const unsigned char *)"Stub GL";                       // RENDERER
    case 0x1F02: return (const unsigned char *)"OpenGL ES 3.2 stub";            // VERSION
    case 0x8B8C: return (const unsigned char *)"OpenGL ES GLSL ES 3.20";        // SHADING_LANGUAGE_VERSION
    case 0x1F03: return (const unsigned char *)"";                              // EXTENSIONS
    }
    return (const unsigned char *)"";
}
static const unsigned char *glGetStringi(GLenum name, GLuint i) { (void)name; (void)i; return (const unsigned char *)""; }
static void glGetIntegerv(GLenum p, GLint *v) {
    if (!v) return;
    switch (p) {
    case 0x821B: *v = 3; break;                     // MAJOR_VERSION
    case 0x821C: *v = 2; break;                     // MINOR_VERSION
    case 0x821D: *v = 0; break;                     // NUM_EXTENSIONS
    case 0x0D33: case 0x851C: case 0x84E8: *v = 16384; break; // MAX_TEXTURE_SIZE, CUBE, RENDERBUFFER
    case 0x0BA2: v[0] = 0; v[1] = 0; v[2] = 1920; v[3] = 1080; break; // VIEWPORT
    case 0x0D3A: v[0] = 16384; v[1] = 16384; break; // MAX_VIEWPORT_DIMS
    default: *v = 16; break;                        // the many MAX_* limits
    }
}
static void glGetFloatv(GLenum p, GLfloat *v) { (void)p; if (v) *v = 1.0f; }
static void glGetBooleanv(GLenum p, GLboolean *v) { (void)p; if (v) *v = 0; }

// ---- the table -----------------------------------------------------------

#define E(name) { #name, (void *)name }
#define N(name) { #name, (void *)gl_nop }
static const struct { const char *name; void *fn; } table[] = {
    E(eglGetDisplay), E(eglInitialize), E(eglChooseConfig), E(eglGetConfigAttrib),
    E(eglCreateWindowSurface), E(eglCreateContext), E(eglMakeCurrent), E(eglQuerySurface),
    E(eglSwapBuffers), E(eglGetError), E(eglGetCurrentContext), E(eglQueryString),
    E(eglGetProcAddress),
    E(eglCreatePbufferSurface), E(eglSurfaceAttrib), E(eglGetCurrentSurface), E(eglGetCurrentDisplay),
    { "eglDestroyContext", (void *)egl_true }, { "eglDestroySurface", (void *)egl_true },
    { "eglTerminate", (void *)egl_true }, { "eglSwapInterval", (void *)egl_true },
    { "eglBindAPI", (void *)egl_true }, { "eglReleaseThread", (void *)egl_true },
    E(glClearColor), E(glClear), E(glDrawArrays), E(glDrawElements), E(glCreateShader),
    E(glCreateProgram), E(glGenBuffers), E(glGenTextures), E(glGetShaderiv),
    E(glGetProgramiv), E(glGetError), E(glGetString), E(glGetStringi), E(glGetIntegerv),
    E(glGetFloatv), E(glGetBooleanv),
    { "glGetShaderInfoLog", (void *)gl_get_log }, { "glGetProgramInfoLog", (void *)gl_get_log },
    { "glGetAttribLocation", (void *)gl_location }, { "glGetUniformLocation", (void *)gl_location },
    N(glActiveTexture), N(glAttachShader), N(glBindBuffer), N(glBindTexture), N(glBufferData),
    N(glCompileShader), N(glDeleteBuffers), N(glDeleteProgram), N(glDeleteShader),
    N(glDisable), N(glEnable), N(glEnableVertexAttribArray), N(glIsEnabled), N(glLineWidth),
    N(glLinkProgram), N(glPixelStorei), N(glShaderSource), N(glTexImage2D),
    N(glTexParameterf), N(glTexParameteri), N(glUniform1i), N(glUniform4f),
    N(glUniformMatrix4fv), N(glUseProgram), N(glVertexAttribPointer), N(glViewport),
    N(glDisableVertexAttribArray), N(glBlendFunc), N(glDepthFunc), N(glDepthMask),
    N(glCullFace), N(glScissor), N(glUniform1f), N(glUniform2f), N(glUniform3f),
};

void *egl_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}
