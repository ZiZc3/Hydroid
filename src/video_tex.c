// video_tex.c — Android's SurfaceTexture for the game's GL (see video_tex.h). Unity's VideoPlayer
// makes one from a texture of its own, gives the decoder a Surface of it, and on each
// onFrameAvailable calls updateTexImage on its GL thread, then draws the texture as an external
// (GL_TEXTURE_EXTERNAL_OES) one. Here: the decoder's NV12 picture is kept as it is (its
// thread), updateTexImage uploads Y and UV and the GPU draws the colours into the game's
// texture, and the game's external textures are plain 2D ones.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game_settings.h"
#include "video_tex.h"

void droid_log(const char *fmt, ...);
void java_frame_available(void *listener, void *self); // java_layer.c: listener.onFrameAvailable(self)

int video_game(void) { return droid_game_in(VIDEO_GAMES); }

// The pictures (NV12) in three buffers taking turns: the decoder fills `spare` and swaps it
// with `ready`; updateTexImage swaps `ready` with `front` and uploads that one, no lock held
// (a 1080p picture as RGBA on the CPU took tens of ms on Unity's decoder thread: it fell
// behind, sought ahead, and showed one picture in a thousand)
struct video_surface {
    unsigned tex;
    void *listener, *self;
    pthread_mutex_t lock;
    uint8_t *ready, *spare, *front;
    size_t ready_cap, spare_cap, front_cap;
    int w, h, fresh;   // the ready one's size; not uploaded yet
    int tex_w, tex_h;  // the texture's size now
    int64_t ts_ns;
    unsigned long frames, uploads;
    double upload_s;
};

video_surface *vsurf_new(unsigned tex) {
    video_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->tex = tex;
    pthread_mutex_init(&s->lock, NULL);
    droid_log("video: SurfaceTexture on the game's texture %u", tex);
    return s;
}

void vsurf_set_listener(video_surface *s, void *listener, void *self) {
    if (!s) return;
    s->listener = listener;
    s->self = self;
}

void vsurf_frame_nv12(video_surface *s, const uint8_t *nv12, int w, int h, int64_t pts_us) {
    if (!s || !nv12 || w <= 0 || h <= 0) return;
    const size_t need = (size_t)w * h + (size_t)w * ((h + 1) / 2);
    if (s->spare_cap < need) { // (only this thread touches spare)
        free(s->spare);
        s->spare = malloc(need);
        s->spare_cap = s->spare ? need : 0;
    }
    if (!s->spare) return;
    memcpy(s->spare, nv12, need);
    pthread_mutex_lock(&s->lock);
    uint8_t *t = s->ready;
    const size_t tc = s->ready_cap;
    s->ready = s->spare;
    s->ready_cap = s->spare_cap;
    s->spare = t;
    s->spare_cap = t ? tc : 0;
    s->w = w;
    s->h = h;
    s->fresh = 1;
    s->ts_ns = pts_us * 1000;
    const unsigned long n = ++s->frames;
    pthread_mutex_unlock(&s->lock);
    if (n == 1 || n % 300 == 0) droid_log("video: picture %lu (%dx%d) for texture %u", n, w, h, s->tex);
    if (s->listener) java_frame_available(s->listener, s->self);
}

int64_t vsurf_timestamp_ns(video_surface *s) { return s ? s->ts_ns : 0; }

// ---- the game's GL ------------------------------------------------------------------------
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
#define V_TEXTURE_2D 0x0DE1
#define V_TEXTURE_EXTERNAL 0x8D65
#define V_TEXTURE_BINDING_2D 0x8069
#define V_TEXTURE_BINDING_EXTERNAL 0x8D67
#define V_PIXEL_UNPACK_BUFFER 0x88EC
#define V_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define V_UNPACK_ALIGNMENT 0x0CF5
#define V_UNPACK_ROW_LENGTH 0x0CF2
#define V_TEXTURE_MIN_FILTER 0x2801
#define V_TEXTURE_MAG_FILTER 0x2800
#define V_TEXTURE_WRAP_S 0x2802
#define V_TEXTURE_WRAP_T 0x2803
#define V_LINEAR 0x2601
#define V_CLAMP_TO_EDGE 0x812F
#define V_RGBA 0x1908
#define V_UNSIGNED_BYTE 0x1401
#define V_ACTIVE_TEXTURE 0x84E0

static void (*real_bind)(GLenum, GLuint);
static void (*real_param_i)(GLenum, GLenum, GLint);
static void (*real_param_f)(GLenum, GLenum, GLfloat);
static void (*real_param_iv)(GLenum, GLenum, const GLint *);
static void (*real_param_fv)(GLenum, GLenum, const GLfloat *);
static void (*real_get_int)(GLenum, GLint *);
static void (*real_shader_source)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*real_tex_image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*real_tex_sub)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*real_pixel_store)(GLenum, GLint);
static void (*real_bind_buffer)(GLenum, GLuint);

static GLenum tgt(GLenum t) { return t == V_TEXTURE_EXTERNAL ? V_TEXTURE_2D : t; }
static void w_bind(GLenum t, GLuint tex) { real_bind(tgt(t), tex); }
static void w_param_i(GLenum t, GLenum p, GLint v) { real_param_i(tgt(t), p, v); }
static void w_param_f(GLenum t, GLenum p, GLfloat v) { real_param_f(tgt(t), p, v); }
static void w_param_iv(GLenum t, GLenum p, const GLint *v) { real_param_iv(tgt(t), p, v); }
static void w_param_fv(GLenum t, GLenum p, const GLfloat *v) { real_param_fv(tgt(t), p, v); }
static void w_get_int(GLenum n, GLint *v) { real_get_int(n == V_TEXTURE_BINDING_EXTERNAL ? V_TEXTURE_BINDING_2D : n, v); }

// the shader with its external sampler a 2D one: the extension's #extension lines out,
// samplerExternalOES -> sampler2D (texture2D / texture take either)
static void w_shader_source(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *all = malloc(total + 1);
    if (!all) {
        real_shader_source(sh, count, str, len);
        return;
    }
    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        const size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        memcpy(all + at, str[i], n);
        at += n;
    }
    all[at] = 0;
    if (!strstr(all, "samplerExternalOES") && !strstr(all, "GL_OES_EGL_image_external")) {
        free(all);
        real_shader_source(sh, count, str, len);
        return;
    }
    char *out = malloc(total + 1), *o = out;
    if (!out) {
        free(all);
        real_shader_source(sh, count, str, len);
        return;
    }
    for (const char *p = all; *p;) {
        const char *eol = strchr(p, '\n');
        const size_t n = eol ? (size_t)(eol - p + 1) : strlen(p);
        int ext_line = 0; // (the console's libc has no memmem)
        if (!strncmp(p + strspn(p, " \t"), "#extension", 10))
            for (size_t k = 0; k + 25 <= n && !ext_line; k++) ext_line = !memcmp(p + k, "GL_OES_EGL_image_external", 25);
        if (!ext_line) {
            memcpy(o, p, n);
            o += n;
        }
        p += n;
    }
    *o = 0;
    for (char *s; (s = strstr(out, "samplerExternalOES"));) { // 18 letters -> "sampler2D" + 9 spaces
        memcpy(s, "sampler2D         ", 18);
    }
    static int told;
    if (told++ < 4) droid_log("video: a shader's external sampler made a 2D one (shader %u)", sh);
    const GLchar *one = out;
    real_shader_source(sh, 1, &one, NULL);
    free(out);
    free(all);
}

void *egl_resolve(const char *name); // the GL the game's names come from (PC: Mesa; PS5: gles_bridge.c)

// The colours by the GPU: Y (R8) and UV (RG8) uploaded, drawn as RGB into the game's texture
// through a framebuffer of ours. Every GL setting touched is given back (Unity keeps its own copy).
typedef unsigned char GLboolean;
#define G(ret, name, args) static ret(*name) args;
G(void, gGetFloatv, (GLenum, GLfloat *)) G(void, gGetBooleanv, (GLenum, GLboolean *)) G(GLboolean, gIsEnabled, (GLenum))
G(void, gEnable, (GLenum)) G(void, gDisable, (GLenum)) G(void, gUseProgram, (GLuint)) G(void, gActiveTexture, (GLenum))
G(void, gBindVertexArray, (GLuint)) G(void, gBindFramebuffer, (GLenum, GLuint)) G(void, gBindSampler, (GLuint, GLuint))
G(void, gViewport, (GLint, GLint, GLsizei, GLsizei)) G(void, gColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
G(void, gGenTextures, (GLsizei, GLuint *)) G(void, gGenBuffers, (GLsizei, GLuint *)) G(void, gGenVertexArrays, (GLsizei, GLuint *))
G(void, gGenFramebuffers, (GLsizei, GLuint *)) G(void, gFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
G(void, gBufferData, (GLenum, long, const void *, GLenum))
G(void, gVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
G(void, gEnableVertexAttribArray, (GLuint)) G(void, gDrawArrays, (GLenum, GLint, GLsizei))
G(GLuint, gCreateShader, (GLenum)) G(void, gCompileShader, (GLuint)) G(GLuint, gCreateProgram, (void))
G(void, gAttachShader, (GLuint, GLuint)) G(void, gBindAttribLocation, (GLuint, GLuint, const char *)) G(void, gLinkProgram, (GLuint))
G(GLint, gGetUniformLocation, (GLuint, const char *)) G(void, gUniform1i, (GLint, GLint)) G(void, gGetProgramiv, (GLuint, GLenum, GLint *))
#undef G
static int gpu_ready(void) {
    static int state;
    if (state) return state > 0;
#define FILL(var, n) if (!var) *(void **)&var = egl_resolve(n);
    FILL(real_bind, "glBindTexture") FILL(real_tex_image, "glTexImage2D") FILL(real_tex_sub, "glTexSubImage2D")
    FILL(real_get_int, "glGetIntegerv") FILL(real_param_i, "glTexParameteri") FILL(real_pixel_store, "glPixelStorei")
    FILL(real_bind_buffer, "glBindBuffer") FILL(real_shader_source, "glShaderSource")
#undef FILL
#define R(var, n) if (!(*(void **)&var = egl_resolve(n))) { droid_log("video: no %s for the GPU colours", n); state = -1; return 0; }
    R(gGetFloatv, "glGetFloatv") R(gGetBooleanv, "glGetBooleanv") R(gIsEnabled, "glIsEnabled") R(gEnable, "glEnable")
    R(gDisable, "glDisable") R(gUseProgram, "glUseProgram") R(gActiveTexture, "glActiveTexture")
    R(gBindVertexArray, "glBindVertexArray") R(gBindFramebuffer, "glBindFramebuffer") R(gBindSampler, "glBindSampler")
    R(gViewport, "glViewport") R(gColorMask, "glColorMask") R(gGenTextures, "glGenTextures") R(gGenBuffers, "glGenBuffers")
    R(gGenVertexArrays, "glGenVertexArrays") R(gGenFramebuffers, "glGenFramebuffers")
    R(gFramebufferTexture2D, "glFramebufferTexture2D") R(gBufferData, "glBufferData")
    R(gVertexAttribPointer, "glVertexAttribPointer") R(gEnableVertexAttribArray, "glEnableVertexAttribArray")
    R(gDrawArrays, "glDrawArrays") R(gCreateShader, "glCreateShader") R(gCompileShader, "glCompileShader")
    R(gCreateProgram, "glCreateProgram") R(gAttachShader, "glAttachShader") R(gBindAttribLocation, "glBindAttribLocation")
    R(gLinkProgram, "glLinkProgram") R(gGetUniformLocation, "glGetUniformLocation") R(gUniform1i, "glUniform1i")
    R(gGetProgramiv, "glGetProgramiv")
#undef R
    state = real_bind && real_tex_image && real_tex_sub && real_get_int && real_param_i && real_pixel_store &&
                    real_bind_buffer && real_shader_source ? 1 : -1;
    return state > 0;
}

static GLuint y_tex, uv_tex, fbo, prog, vbo, vao;
static int planes_w, planes_h;
static int gpu_make(void) {
    if (prog) return 1;
    static const char *vs_src =
        "attribute vec2 p;\n"
        "varying vec2 v;\n"
        "void main() { v = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
    // NV12 -> RGB, BT.709 limited range (HD video); the picture's first row lands in the
    // texture's first row, as a decoder's (the game's transform matrix turns it up)
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec2 v;\n"
        "uniform sampler2D sy;\n"
        "uniform sampler2D suv;\n"
        "void main() {\n"
        "  float y = (texture2D(sy, v).r - 0.0625) * 1.164;\n"
        "  vec2 c = (texture2D(suv, v).rg - 0.5) * 1.138;\n"
        "  gl_FragColor = vec4(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x, 1.0);\n"
        "}\n";
    GLuint vs = gCreateShader(0x8B31), fs = gCreateShader(0x8B30);
    real_shader_source(vs, 1, &vs_src, NULL);
    real_shader_source(fs, 1, &fs_src, NULL);
    gCompileShader(vs);
    gCompileShader(fs);
    prog = gCreateProgram();
    gAttachShader(prog, vs);
    gAttachShader(prog, fs);
    gBindAttribLocation(prog, 0, "p");
    gLinkProgram(prog);
    GLint ok = 0;
    gGetProgramiv(prog, 0x8B82, &ok);
    if (!ok) {
        droid_log("video: the GPU colour shader did not link");
        prog = 0;
        return 0;
    }
    gGenTextures(1, &y_tex);
    gGenTextures(1, &uv_tex);
    gGenFramebuffers(1, &fbo);
    gGenBuffers(1, &vbo);
    gGenVertexArrays(1, &vao);
    gBindVertexArray(vao);
    real_bind_buffer(0x8892, vbo);
    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    gBufferData(0x8892, sizeof(quad), quad, 0x88E4); // STATIC_DRAW
    gVertexAttribPointer(0, 2, 0x1406, 0, 2 * sizeof(float), (const void *)0);
    gEnableVertexAttribArray(0);
    return 1;
}
static void plane_tex(GLuint t, int w, int h, GLint internal, GLenum fmt, const uint8_t *data, int fresh_size) {
    real_bind(V_TEXTURE_2D, t);
    if (fresh_size) {
        real_tex_image(V_TEXTURE_2D, 0, internal, w, h, 0, fmt, V_UNSIGNED_BYTE, data);
        real_param_i(V_TEXTURE_2D, V_TEXTURE_MIN_FILTER, V_LINEAR);
        real_param_i(V_TEXTURE_2D, V_TEXTURE_MAG_FILTER, V_LINEAR);
        real_param_i(V_TEXTURE_2D, V_TEXTURE_WRAP_S, V_CLAMP_TO_EDGE);
        real_param_i(V_TEXTURE_2D, V_TEXTURE_WRAP_T, V_CLAMP_TO_EDGE);
    } else {
        real_tex_sub(V_TEXTURE_2D, 0, 0, 0, w, h, fmt, V_UNSIGNED_BYTE, data);
    }
}

void vsurf_update(video_surface *s) {
    if (!s || !gpu_ready()) return;
    pthread_mutex_lock(&s->lock);
    const int fresh = s->fresh && s->ready;
    if (fresh) { // the newest one is ours now
        uint8_t *t = s->front;
        const size_t tc = s->front_cap;
        s->front = s->ready;
        s->front_cap = s->ready_cap;
        s->ready = t;
        s->ready_cap = t ? tc : 0;
        s->fresh = 0;
    }
    const int w = s->w, h = s->h;
    pthread_mutex_unlock(&s->lock);
    if (!fresh) return;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    // what the game had set
    GLint program, active, tex[2], sampler[2], array_buf, vao_b, draw_fb, read_fb, view[4], unpack, align, row;
    GLboolean mask[4];
    const GLenum caps[] = { 0x0C11, 0x0BE2, 0x0B71, 0x0B44, 0x0B90 }; // scissor, blend, depth, cull, stencil
    GLboolean on[5];
    real_get_int(0x8B8D, &program);
    real_get_int(V_ACTIVE_TEXTURE, &active);
    for (int u = 0; u < 2; u++) {
        gActiveTexture(0x84C0 + u);
        real_get_int(V_TEXTURE_BINDING_2D, &tex[u]);
        real_get_int(0x8919, &sampler[u]);
    }
    real_get_int(0x8894, &array_buf);
    real_get_int(0x85B5, &vao_b);
    real_get_int(0x8CA6, &draw_fb);
    real_get_int(0x8CAA, &read_fb);
    real_get_int(0x0BA2, view);
    real_get_int(V_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    real_get_int(V_UNPACK_ALIGNMENT, &align);
    real_get_int(V_UNPACK_ROW_LENGTH, &row);
    gGetBooleanv(0x0C23, mask);
    for (int i = 0; i < 5; i++) on[i] = gIsEnabled(caps[i]);
    if (gpu_make()) {
        if (unpack) real_bind_buffer(V_PIXEL_UNPACK_BUFFER, 0);
        real_pixel_store(V_UNPACK_ALIGNMENT, 1);
        real_pixel_store(V_UNPACK_ROW_LENGTH, 0);
        const int new_size = planes_w != w || planes_h != h;
        gActiveTexture(0x84C0);
        gBindSampler(0, 0);
        plane_tex(y_tex, w, h, 0x8229, 0x1903, s->front, new_size);                                  // R8, RED
        gActiveTexture(0x84C1);
        gBindSampler(1, 0);
        plane_tex(uv_tex, (w + 1) / 2, (h + 1) / 2, 0x822B, 0x8227, s->front + (size_t)w * h, new_size); // RG8, RG
        planes_w = w;
        planes_h = h;
        if (s->tex_w != w || s->tex_h != h) { // the game's texture: room for the picture (on unit 0: given back below)
            gActiveTexture(0x84C0);
            real_bind(V_TEXTURE_2D, s->tex);
            real_tex_image(V_TEXTURE_2D, 0, V_RGBA, w, h, 0, V_RGBA, V_UNSIGNED_BYTE, NULL);
            real_param_i(V_TEXTURE_2D, V_TEXTURE_MIN_FILTER, V_LINEAR);
            real_param_i(V_TEXTURE_2D, V_TEXTURE_MAG_FILTER, V_LINEAR);
            real_param_i(V_TEXTURE_2D, V_TEXTURE_WRAP_S, V_CLAMP_TO_EDGE);
            real_param_i(V_TEXTURE_2D, V_TEXTURE_WRAP_T, V_CLAMP_TO_EDGE);
            real_bind(V_TEXTURE_2D, 0);
            s->tex_w = w;
            s->tex_h = h;
        }
        gBindFramebuffer(0x8D40, fbo);
        gFramebufferTexture2D(0x8D40, 0x8CE0, V_TEXTURE_2D, s->tex, 0);
        gViewport(0, 0, w, h);
        for (int i = 0; i < 5; i++) gDisable(caps[i]);
        gColorMask(1, 1, 1, 1);
        gUseProgram(prog);
        gUniform1i(gGetUniformLocation(prog, "sy"), 0);
        gUniform1i(gGetUniformLocation(prog, "suv"), 1);
        gBindVertexArray(vao);
        gDrawArrays(0x0005, 0, 4); // a strip
        gFramebufferTexture2D(0x8D40, 0x8CE0, V_TEXTURE_2D, 0, 0);
    }
    // given back
    gBindVertexArray((GLuint)vao_b);
    real_bind_buffer(0x8892, (GLuint)array_buf);
    real_bind_buffer(V_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
    real_pixel_store(V_UNPACK_ALIGNMENT, align);
    real_pixel_store(V_UNPACK_ROW_LENGTH, row);
    for (int u = 0; u < 2; u++) {
        gActiveTexture(0x84C0 + u);
        real_bind(V_TEXTURE_2D, (GLuint)tex[u]);
        gBindSampler(u, (GLuint)sampler[u]);
    }
    gActiveTexture((GLenum)active);
    gUseProgram((GLuint)program);
    gBindFramebuffer(0x8CA9, (GLuint)draw_fb); // DRAW
    gBindFramebuffer(0x8CA8, (GLuint)read_fb); // READ
    gViewport(view[0], view[1], view[2], view[3]);
    gColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < 5; i++) (on[i] ? gEnable : gDisable)(caps[i]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    s->upload_s += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    const unsigned long n = ++s->uploads;
    if (n == 1 || n % 300 == 0)
        droid_log("video: picture %lu into texture %u by the GPU, %.1f ms each", n, s->tex, s->upload_s * 1000 / n);
}

void *video_gl_wrap(const char *name, void *real) {
    if (!real || !name || name[0] != 'g' || name[1] != 'l' || !video_game()) return real;
    // the ones only kept, for the upload
    if (!strcmp(name, "glTexImage2D")) *(void **)&real_tex_image = real;
    else if (!strcmp(name, "glTexSubImage2D")) *(void **)&real_tex_sub = real;
    else if (!strcmp(name, "glPixelStorei")) *(void **)&real_pixel_store = real;
    else if (!strcmp(name, "glBindBuffer")) *(void **)&real_bind_buffer = real;
#define WRAP(n, var, w) if (!strcmp(name, n)) { if (real != (void *)w) *(void **)&var = real; return (void *)w; }
    WRAP("glBindTexture", real_bind, w_bind)
    WRAP("glTexParameteri", real_param_i, w_param_i)
    WRAP("glTexParameterf", real_param_f, w_param_f)
    WRAP("glTexParameteriv", real_param_iv, w_param_iv)
    WRAP("glTexParameterfv", real_param_fv, w_param_fv)
    WRAP("glGetIntegerv", real_get_int, w_get_int)
    WRAP("glShaderSource", real_shader_source, w_shader_source)
#undef WRAP
    return real;
}
