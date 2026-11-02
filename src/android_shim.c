// android_shim.c — the PS5 side of the Android NDK calls a game imports:
// input events (libandroid) and AAudio (libaaudio), plus the libc names.

#include "arm_cpu.h"
#include "game_settings.h"
#include "android_shim.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                    uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *buf);
int sceKernelUsleep(uint32_t us);

// ---- input ---------------------------------------------------------------

int32_t AInputEvent_getType(const droid_input_event *e) { return e->type; }
int32_t AInputEvent_getSource(const droid_input_event *e) { return e->source; }
int32_t AInputEvent_getDeviceId(const droid_input_event *e) { (void)e; return 1; }
int32_t AKeyEvent_getAction(const droid_input_event *e) { return e->action; }
int32_t AKeyEvent_getKeyCode(const droid_input_event *e) { return e->keycode; }
int32_t AKeyEvent_getRepeatCount(const droid_input_event *e) { (void)e; return 0; }
int32_t AKeyEvent_getMetaState(const droid_input_event *e) { (void)e; return 0; }
int32_t AMotionEvent_getAction(const droid_input_event *e) { return e->action; }
size_t AMotionEvent_getPointerCount(const droid_input_event *e) { (void)e; return 1; }
float AMotionEvent_getAxisValue(const droid_input_event *e, int32_t axis, size_t pointer) {
    (void)pointer;
    return (axis >= 0 && axis < DROID_AXES) ? e->axes[axis] : 0.0f;
}
float AMotionEvent_getX(const droid_input_event *e, size_t p) { (void)p; return e->axes[0]; }
float AMotionEvent_getY(const droid_input_event *e, size_t p) { (void)p; return e->axes[1]; }
int32_t AMotionEvent_getPointerId(const droid_input_event *e, size_t p) { (void)e; return (int32_t)p; }

// ---- AAudio --------------------------------------------------------------

#define AAUDIO_OK 0
#define AAUDIO_ERROR_ILLEGAL_ARGUMENT (-898)
#define AAUDIO_ERROR_NO_MEMORY (-887)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_FORMAT_PCM_FLOAT 2
#define AAUDIO_STREAM_STATE_OPEN 2
#define AAUDIO_STREAM_STATE_STARTED 4
#define AAUDIO_STREAM_STATE_STOPPED 10
#define AAUDIO_STREAM_STATE_CLOSED 12
#define GRAIN 256

typedef int32_t (*aaudio_callback)(void *stream, void *user, void *data, int32_t frames);

typedef struct {
    int32_t format, channels, rate;
    aaudio_callback callback;
    void *user;
} aaudio_builder;

typedef struct {
    aaudio_builder cfg;
    int port;
    volatile int state;
    pthread_t thread;
    int has_thread; // a callback stream's own output thread (a write stream: the game's writes play)
    uint64_t frames_written;
    int16_t pend[GRAIN * 2]; // a write stream's frames until a grain is full
    int npend;
} aaudio_stream;

static int audio_inited;

int32_t AAudio_createStreamBuilder(aaudio_builder **out) {
    aaudio_builder *b = calloc(1, sizeof(*b));
    if (!b) return AAUDIO_ERROR_NO_MEMORY;
    b->format = AAUDIO_FORMAT_PCM_FLOAT;
    b->channels = 2;
    b->rate = 48000;
    *out = b;
    return AAUDIO_OK;
}
void AAudioStreamBuilder_setFormat(aaudio_builder *b, int32_t f) { b->format = f; }
void AAudioStreamBuilder_setChannelCount(aaudio_builder *b, int32_t n) { b->channels = n; }
void AAudioStreamBuilder_setSamplesPerFrame(aaudio_builder *b, int32_t n) { b->channels = n; }
void AAudioStreamBuilder_setSampleRate(aaudio_builder *b, int32_t r) { b->rate = r; }
void AAudioStreamBuilder_setDataCallback(aaudio_builder *b, aaudio_callback cb, void *user) {
    b->callback = cb;
    b->user = user;
}
void AAudioStreamBuilder_setIgnored(aaudio_builder *b, int32_t v) { (void)b; (void)v; }
int32_t AAudioStreamBuilder_delete(aaudio_builder *b) { free(b); return AAUDIO_OK; }

int32_t AAudioStreamBuilder_openStream(aaudio_builder *b, aaudio_stream **out) {
    // The PS5 port is 48 kHz, 16-bit stereo (as VLC/XPSemu open it).
    if (b->rate != 48000 || b->channels < 1 || b->channels > 2 ||
        (b->format != AAUDIO_FORMAT_PCM_FLOAT && b->format != AAUDIO_FORMAT_PCM_I16))
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if (!audio_inited) {
        sceAudioOutInit();
        audio_inited = 1;
    }
    int port = sceAudioOutOpen(0xff, 0, 0, GRAIN, 48000, 1);
    droid_log("AAudio: open %d ch, format %d -> sceAudioOut port %#x", b->channels, b->format,
              port);
    if (port < 0) return AAUDIO_ERROR_UNAVAILABLE;
    aaudio_stream *s = calloc(1, sizeof(*s));
    if (!s) return AAUDIO_ERROR_NO_MEMORY;
    s->cfg = *b;
    s->port = port;
    s->state = AAUDIO_STREAM_STATE_OPEN;
    *out = s;
    return AAUDIO_OK;
}

static int16_t to_i16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

// (held back instead, FMOD's AAudio callbacks stopped for the pause: after one of 5 s Geometry
// Dash stayed silent - FMOD took its stream for dead)
extern volatile int droid_game_paused; // quick_menu.c
void droid_audio_mute_if_paused(int16_t *stereo, int frames) {
    if (droid_game_paused) memset(stereo, 0, sizeof(int16_t) * 2 * (size_t)frames);
}

static void *audio_thread(void *arg) {
    aaudio_stream *s = arg;
    arm_thread_role("AAudio");
    float fbuf[GRAIN * 2];
    int16_t ibuf[GRAIN * 2];
    int16_t out[GRAIN * 2];
    while (s->state == AAUDIO_STREAM_STATE_STARTED) {
        int ch = s->cfg.channels;
        void *data = s->cfg.format == AAUDIO_FORMAT_PCM_FLOAT ? (void *)fbuf : (void *)ibuf;
        memset(data, 0, sizeof(fbuf));
        if (s->cfg.callback) s->cfg.callback(s, s->cfg.user, data, GRAIN);
        for (int i = 0; i < GRAIN; i++) {
            for (int c = 0; c < 2; c++) {
                int src = i * ch + (ch == 1 ? 0 : c);
                out[2 * i + c] = s->cfg.format == AAUDIO_FORMAT_PCM_FLOAT ? to_i16(fbuf[src])
                                                                         : ibuf[src];
            }
        }
        { // every ~5 s: how loud the game's sound was (0: it sends silence)
            static __thread int grains, peak;
            for (int i = 0; i < GRAIN * 2; i++) {
                int v = out[i] < 0 ? -out[i] : out[i];
                if (v > peak) peak = v;
            }
            if (++grains == 940) {
                droid_log("AAudio: port %#x, last 5 s: loudest sample %d of 32767", s->port, peak);
                grains = peak = 0;
            }
        }
        droid_audio_mute_if_paused(out, GRAIN); // (the Quick menu open: silent)
        sceAudioOutOutput(s->port, out); // blocks for one grain: the clock
        arm_sig_point();                 // the game's GC pause, between blocks
        s->frames_written += GRAIN;
    }
    return NULL;
}

int32_t AAudioStream_requestStart(aaudio_stream *s) {
    if (s->state == AAUDIO_STREAM_STATE_STARTED) return AAUDIO_OK;
    s->state = AAUDIO_STREAM_STATE_STARTED;
    if (!s->cfg.callback) { // a write stream: AAudioStream_write plays (no thread on the same port)
        droid_log("AAudio: port %#x started, the game writes to it", s->port);
        return AAUDIO_OK;
    }
    if (pthread_create(&s->thread, NULL, audio_thread, s) != 0) {
        s->state = AAUDIO_STREAM_STATE_OPEN;
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    s->has_thread = 1;
    return AAUDIO_OK;
}
int32_t AAudioStream_requestStop(aaudio_stream *s) {
    droid_log("AAudio: port %#x stopped by the game", s->port);
    if (s->state == AAUDIO_STREAM_STATE_STARTED) {
        s->state = AAUDIO_STREAM_STATE_STOPPED;
        if (s->has_thread) pthread_join(s->thread, NULL);
        s->has_thread = 0;
    }
    return AAUDIO_OK;
}
// AAudioStream_write (a stream without a data callback: the Infinity Blade ports' mixer): the frames
// into 256-frame grains, each played on the stream's port as it fills - sceAudioOutOutput blocks
// for one grain, as a blocking write on a phone waits for room. All frames are taken.
int32_t AAudioStream_write(aaudio_stream *s, const void *buffer, int32_t frames, int64_t timeout_ns) {
    (void)timeout_ns;
    if (!s || !buffer || frames < 0) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if (s->state != AAUDIO_STREAM_STATE_STARTED) return -895; // AAUDIO_ERROR_INVALID_STATE
    const int ch = s->cfg.channels;
    const float *f = buffer;
    const int16_t *i16 = buffer;
    for (int32_t k = 0; k < frames; k++) {
        for (int c = 0; c < 2; c++) {
            const int src = k * ch + (ch == 1 ? 0 : c);
            s->pend[2 * s->npend + c] = s->cfg.format == AAUDIO_FORMAT_PCM_FLOAT ? to_i16(f[src]) : i16[src];
        }
        if (++s->npend == GRAIN) {
            droid_audio_mute_if_paused(s->pend, GRAIN);
            sceAudioOutOutput(s->port, s->pend);
            s->frames_written += GRAIN;
            s->npend = 0;
        }
    }
    static int told;
    if (!told++) droid_log("AAudio: port %#x, the game's first write: %d frames", s->port, frames);
    return frames;
}
int32_t AAudioStream_requestPause(aaudio_stream *s) { return AAudioStream_requestStop(s); }
int32_t AAudioStream_close(aaudio_stream *s) {
    AAudioStream_requestStop(s);
    free(s);
    return AAUDIO_OK;
}
int32_t AAudioStream_getState(aaudio_stream *s) { return s->state; }
int32_t AAudioStream_getSampleRate(aaudio_stream *s) { return s->cfg.rate; }
int32_t AAudioStream_getChannelCount(aaudio_stream *s) { return s->cfg.channels; }
int32_t AAudioStream_getFormat(aaudio_stream *s) { return s->cfg.format; }
int32_t AAudioStream_getFramesPerBurst(aaudio_stream *s) { (void)s; return GRAIN; }
int32_t AAudioStream_getBufferSizeInFrames(aaudio_stream *s) { (void)s; return GRAIN * 2; }
int64_t AAudioStream_getFramesWritten(aaudio_stream *s) { return (int64_t)s->frames_written; }
const char *AAudio_convertResultToText(int32_t r) { return r == AAUDIO_OK ? "AAUDIO_OK" : "AAUDIO_ERROR"; }
// the rest of AAudio a full user asks (FMOD): settings taken and ignored, answers of a phone's
// shared low-latency stream (missing ones came out NULL through dlsym: FMOD called address 0)
static void aaudio_builder_ignore3(aaudio_builder *b, void *x, void *y) { (void)b; (void)x; (void)y; }
static int32_t aaudio_zero(aaudio_stream *s) { (void)s; return 0; }
static int32_t aaudio_ok(aaudio_stream *s) { (void)s; return AAUDIO_OK; }
static int32_t AAudioStream_getPerformanceMode(aaudio_stream *s) { (void)s; return 12; } // LOW_LATENCY
static int32_t AAudioStream_getSharingMode(aaudio_stream *s) { (void)s; return 1; }      // SHARED
static int32_t AAudioStream_getBufferCapacityInFrames(aaudio_stream *s) { (void)s; return GRAIN * 4; }
static int32_t AAudioStream_getFramesPerDataCallback(aaudio_stream *s) { (void)s; return GRAIN; }
static int32_t AAudioStream_setBufferSizeInFrames(aaudio_stream *s, int32_t n) { (void)s; return n < GRAIN * 2 ? n : GRAIN * 2; }
static int32_t AAudioStream_waitForStateChange(aaudio_stream *s, int32_t input, int32_t *next, int64_t ns) {
    (void)input; (void)ns;
    if (next) *next = s->state;
    return AAUDIO_OK;
}
static int32_t AAudioStream_getTimestamp(aaudio_stream *s, int32_t clock, int64_t *frame, int64_t *ns) {
    (void)s; (void)clock; (void)frame; (void)ns;
    return -895; // AAUDIO_ERROR_INVALID_STATE: no timestamp yet (allowed)
}

// ---- liblog --------------------------------------------------------------

static const char *log_level(int prio) {
    static const char *names[] = { "?", "?", "V", "D", "I", "W", "E", "F" };
    return (prio >= 0 && prio < 8) ? names[prio] : "?";
}

// Info and up by default: games log thousands of Debug/Verbose lines (Endless
// Tunnel dumps every vertex when a scene starts), and each one written to the
// console's disk stalled the game. /app0/data/debug-log brings them back.
static int log_min_prio(void) {
    static int min = -1;
    if (min < 0) {
        min = settings.debug_log ? 2 : 4;
    }
    return min;
}

int __android_log_write(int prio, const char *tag, const char *text) {
    if (prio < log_min_prio()) return 1;
    droid_log("[%s/%s] %s", log_level(prio), tag ? tag : "", text ? text : "");
    return 1;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    if (prio < log_min_prio()) return 1;
    char text[1024];
    vsnprintf(text, sizeof(text), fmt, ap);
    return __android_log_write(prio, tag, text);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    char text[1024] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
    }
    droid_log("[ASSERT/%s] %s %s", tag ? tag : "", cond ? cond : "", text);
}

// ---- the name table ------------------------------------------------------

#define E(name) { #name, (void *)name }
static const struct { const char *name; void *fn; } table[] = {
    E(AInputEvent_getType), E(AInputEvent_getSource), E(AInputEvent_getDeviceId),
    E(AKeyEvent_getAction), E(AKeyEvent_getKeyCode), E(AKeyEvent_getRepeatCount),
    E(AKeyEvent_getMetaState), E(AMotionEvent_getAction), E(AMotionEvent_getPointerCount),
    E(AMotionEvent_getAxisValue), E(AMotionEvent_getX), E(AMotionEvent_getY),
    E(AMotionEvent_getPointerId),
    E(AAudio_createStreamBuilder), E(AAudioStreamBuilder_setFormat),
    E(AAudioStreamBuilder_setChannelCount), E(AAudioStreamBuilder_setSamplesPerFrame),
    E(AAudioStreamBuilder_setSampleRate), E(AAudioStreamBuilder_setDataCallback),
    { "AAudioStreamBuilder_setPerformanceMode", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setSharingMode", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setDirection", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setBufferCapacityInFrames", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setUsage", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setContentType", (void *)AAudioStreamBuilder_setIgnored },
    E(AAudioStreamBuilder_delete), E(AAudioStreamBuilder_openStream),
    E(AAudioStream_requestStart), E(AAudioStream_requestStop), E(AAudioStream_requestPause),
    E(AAudioStream_close), E(AAudioStream_getState), E(AAudioStream_getSampleRate),
    E(AAudioStream_getChannelCount), E(AAudioStream_getFormat),
    E(AAudioStream_getFramesPerBurst), E(AAudioStream_getBufferSizeInFrames),
    E(AAudioStream_getFramesWritten), E(AAudio_convertResultToText), E(AAudioStream_write),
    { "AAudioStreamBuilder_setErrorCallback", (void *)aaudio_builder_ignore3 },
    { "AAudioStreamBuilder_setDeviceId", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setFramesPerDataCallback", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setInputPreset", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setAllowedCapturePolicy", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setSessionId", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setPrivacySensitive", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStreamBuilder_setChannelMask", (void *)AAudioStreamBuilder_setIgnored },
    { "AAudioStream_getDeviceId", (void *)aaudio_zero }, { "AAudioStream_getXRunCount", (void *)aaudio_zero },
    { "AAudioStream_getFramesRead", (void *)aaudio_zero }, { "AAudioStream_getSessionId", (void *)aaudio_zero },
    { "AAudioStream_requestFlush", (void *)aaudio_ok }, { "AAudioStream_release", (void *)aaudio_ok },
    E(AAudioStream_getPerformanceMode), E(AAudioStream_getSharingMode), E(AAudioStream_getBufferCapacityInFrames),
    E(AAudioStream_getFramesPerDataCallback), E(AAudioStream_setBufferSizeInFrames),
    E(AAudioStream_waitForStateChange), E(AAudioStream_getTimestamp),
    E(__android_log_write), E(__android_log_vprint), E(__android_log_print),
    E(__android_log_assert),
    E(memset), E(memcpy), E(memcmp), // (memmove: bionic_shim.c's b_memmove, Candy Crush's own)
    E(strlen), E(strcmp), E(malloc),
    E(calloc), E(free), E(realloc),
};

void *bionic_resolve(const char *name);
void *activity_resolve(const char *name);
void *egl_resolve(const char *name);
void *opensl_resolve(const char *name);
void *apk_resolve(const char *name);
void *mediandk_resolve(const char *name); // mediandk.c: Unity's VideoPlayer

void *droid_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    void *p;
    void *texspy_wrap(const char *name, void *real); // texspy.c (tools.txt "TexSpy")
    void *video_gl_wrap(const char *name, void *real); // video_tex.c (video games only)
    int phone_gl_wants(const char *name);             // phone_gl.c (the phone view)
    void *phone_gl_wrap(const char *name, void *real, void *(*resolver)(const char *));
    if (phone_gl_wants(name) && (p = egl_resolve(name))) return phone_gl_wrap(name, p, egl_resolve);
    if ((!strncmp(name, "glTex", 5) || !strncmp(name, "glCompressedTex", 15)) && (p = egl_resolve(name)))
        return video_gl_wrap(name, texspy_wrap(name, p)); // (the same egl_resolve as below; TexSpy / video in between)
    if ((p = bionic_resolve(name)) || (p = activity_resolve(name)) || (p = egl_resolve(name)) ||
        (p = opensl_resolve(name)) || (p = apk_resolve(name)) || (p = mediandk_resolve(name)))
        return video_gl_wrap(name, p);
    return NULL;
}
