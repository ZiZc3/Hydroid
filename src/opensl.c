// opensl.c â€” OpenSL ES, as Android games use it: an engine, an output mix and
// buffer-queue audio players fed PCM by the game. Every playing player is
// mixed to 48 kHz 16-bit stereo on one PS5 audio port, on one thread.
// Players that read files or URIs (SLDataLocator_URI / AndroidFD) aren't
// supported yet: CreateAudioPlayer says so and the game goes on without them.

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_shim.h"
#include "arm_cpu.h"

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                    uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *buf);

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef int16_t SLmillibel;
typedef uint32_t SLboolean;
typedef const void *SLInterfaceID;

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PRECONDITIONS_VIOLATED 1
#define SL_RESULT_PARAMETER_INVALID 2
#define SL_RESULT_MEMORY_FAILURE 3
#define SL_RESULT_BUFFER_INSUFFICIENT 7
#define SL_RESULT_CONTENT_UNSUPPORTED 9
#define SL_RESULT_FEATURE_UNSUPPORTED 12
#define SL_OBJECT_STATE_UNREALIZED 1
#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3
#define SL_DATALOCATOR_BUFFERQUEUE 6
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BD
#define SL_DATAFORMAT_PCM 2
#define SL_ANDROID_DATAFORMAT_PCM_EX 4
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT 3

#define OUT_RATE 48000
#define GRAIN 256
#define MAX_PLAYERS 32
#define MAX_QUEUE 64

// ---- interface ids: the game compares the pointers we hand it ---------------

enum { IID_ENGINE, IID_BUFFERQUEUE, IID_ANDROIDSIMPLEBUFFERQUEUE, IID_PLAY, IID_VOLUME,
       IID_EFFECTSEND, IID_OBJECT, IID_ANDROIDCONFIGURATION, IID_KNOWN };
static const char iid_storage[IID_KNOWN + 32][16];
static const void *iid_values[IID_KNOWN + 32];
static const char *iid_names[IID_KNOWN + 32] = {
    "SL_IID_ENGINE", "SL_IID_BUFFERQUEUE", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_PLAY",
    "SL_IID_VOLUME", "SL_IID_EFFECTSEND", "SL_IID_OBJECT", "SL_IID_ANDROIDCONFIGURATION",
};
static int iid_count = IID_KNOWN;
static pthread_mutex_t iid_lock = PTHREAD_MUTEX_INITIALIZER;

static int iid_kind(SLInterfaceID id) {
    for (int i = 0; i < IID_KNOWN; i++)
        if (id == iid_storage[i]) return i;
    return -1;
}

// ---- the object model: an interface is a pointer to its vtable pointer -------

typedef struct sl_object sl_object;
typedef const void *const *sl_itf;

typedef struct {
    const void *buffer;
    uint32_t size;
} queued;

struct sl_object {
    const void *object_vtbl;   // SLObjectItf points here
    const void *engine_vtbl;   // SLEngineItf (engine)
    const void *play_vtbl;     // SLPlayItf (player)
    const void *queue_vtbl;    // SLAndroidSimpleBufferQueueItf / SLBufferQueueItf
    const void *volume_vtbl;   // SLVolumeItf
    const void *effect_vtbl;   // SLEffectSendItf
    const void *config_vtbl;   // SLAndroidConfigurationItf (Unreal games: Wwise uses it unchecked)
    int kind;                  // 0 engine, 1 output mix, 2 player
    int state;
    // player
    int play_state;
    uint32_t channels, rate, bits;
    int is_float;
    float gain;
    int muted;
    queued queue[MAX_QUEUE];
    uint32_t head, count, played; // played = buffers finished, for GetState's index
    double pos;                   // frames into queue[head]
    void (*callback)(sl_itf, void *);
    void *callback_context;
};

#define ITF(obj, field) ((sl_itf)&(obj)->field)
#define OBJ(itf, field) ((sl_object *)((char *)(itf) - offsetof(sl_object, field)))

static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static sl_object *players[MAX_PLAYERS];
static int mixer_started;

// ---- the mixer -----------------------------------------------------------------

static float sample_at(const sl_object *p, const queued *q, uint32_t frame, uint32_t ch) {
    uint32_t c = ch < p->channels ? ch : 0;
    if (p->is_float) return ((const float *)q->buffer)[frame * p->channels + c];
    if (p->bits == 8)
        return (((const uint8_t *)q->buffer)[frame * p->channels + c] - 128) / 128.0f;
    return ((const int16_t *)q->buffer)[frame * p->channels + c] / 32768.0f;
}

static uint32_t buffer_frames(const sl_object *p, const queued *q) {
    uint32_t bytes = p->is_float ? 4 : p->bits / 8;
    return q->size / (bytes * p->channels);
}

static void *mixer_thread(void *arg) {
    (void)arg;
    arm_thread_role("OpenSL mixer");
    sceAudioOutInit();
    int port = sceAudioOutOpen(0xff, 0, 0, GRAIN, OUT_RATE, 1);
    droid_log("OpenSL ES: mixer on sceAudioOut port %#x", port);
    if (port < 0) return NULL;
    float mix[GRAIN * 2];
    int16_t out[GRAIN * 2];
    struct { sl_itf itf; void (*fn)(sl_itf, void *); void *ctx; } done[MAX_PLAYERS * 4];
    // every 5 s: how loud it was and what its players did (a game waiting on a sound to end,
    // Dragon Ball Legends' story voices: told apart from one that never played)
    int peak = 0, grains = 0, played_bufs = 0;
    for (;;) {
        memset(mix, 0, sizeof(mix));
        int ndone = 0;
        pthread_mutex_lock(&mix_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            sl_object *p = players[i];
            if (!p || p->play_state != SL_PLAYSTATE_PLAYING || p->muted) continue;
            double step = (double)p->rate / OUT_RATE;
            for (int f = 0; f < GRAIN && p->count; f++) {
                queued *q = &p->queue[p->head];
                uint32_t frames = buffer_frames(p, q);
                uint32_t at = (uint32_t)p->pos;
                if (at < frames) {
                    mix[2 * f] += sample_at(p, q, at, 0) * p->gain;
                    mix[2 * f + 1] += sample_at(p, q, at, 1) * p->gain;
                }
                p->pos += step;
                if ((uint32_t)p->pos >= frames) {
                    p->pos -= frames;
                    if (p->pos < 0) p->pos = 0;
                    p->head = (p->head + 1) % MAX_QUEUE;
                    p->count--;
                    p->played++;
                    played_bufs++;
                    if (p->callback && ndone < (int)(sizeof(done) / sizeof(done[0]))) {
                        done[ndone].itf = ITF(p, queue_vtbl);
                        done[ndone].fn = p->callback;
                        done[ndone].ctx = p->callback_context;
                        ndone++;
                    }
                }
            }
        }
        pthread_mutex_unlock(&mix_lock);
        // The game refills its queue from these, outside our lock (Enqueue locks).
        for (int i = 0; i < ndone; i++) done[i].fn(done[i].itf, done[i].ctx);
        for (int i = 0; i < GRAIN * 2; i++) {
            float v = mix[i] > 1.0f ? 1.0f : mix[i] < -1.0f ? -1.0f : mix[i];
            out[i] = (int16_t)(v * 32767.0f);
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a > peak) peak = a;
        }
        if (++grains * GRAIN >= OUT_RATE * 5) {
            int playing = 0, queued_bufs = 0;
            pthread_mutex_lock(&mix_lock);
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (players[i]) {
                    if (players[i]->play_state == SL_PLAYSTATE_PLAYING) playing++;
                    queued_bufs += (int)players[i]->count;
                }
            pthread_mutex_unlock(&mix_lock);
            droid_log("OpenSL ES: last 5 s: loudest sample %d of 32767, %d player(s) playing, %d buffers played, %d waiting",
                      peak, playing, played_bufs, queued_bufs);
            peak = grains = played_bufs = 0;
        }
        droid_audio_mute_if_paused(out, GRAIN); // (the Quick menu open: silent)
        sceAudioOutOutput(port, out); // blocks for one grain: the clock
        arm_sig_point();              // the game's GC pause, between blocks
    }
    return NULL;
}

static void start_mixer(void) {
    if (mixer_started) return;
    mixer_started = 1;
    pthread_t t;
    pthread_create(&t, NULL, mixer_thread, NULL);
    pthread_detach(t);
}

// ---- SLObjectItf -------------------------------------------------------------

static SLresult obj_Realize(sl_itf self, SLboolean async) {
    (void)async;
    OBJ(self, object_vtbl)->state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_Resume(sl_itf self, SLboolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_GetState(sl_itf self, SLuint32 *state) {
    *state = (SLuint32)OBJ(self, object_vtbl)->state;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_GetInterface(sl_itf self, SLInterfaceID id, void *out) {
    sl_object *o = OBJ(self, object_vtbl);
    sl_itf itf = NULL;
    switch (iid_kind(id)) {
    case IID_OBJECT: itf = ITF(o, object_vtbl); break;
    case IID_ENGINE: if (o->kind == 0) itf = ITF(o, engine_vtbl); break;
    case IID_PLAY: if (o->kind == 2) itf = ITF(o, play_vtbl); break;
    case IID_BUFFERQUEUE:
    case IID_ANDROIDSIMPLEBUFFERQUEUE: if (o->kind == 2) itf = ITF(o, queue_vtbl); break;
    case IID_VOLUME: if (o->kind == 2) itf = ITF(o, volume_vtbl); break;
    case IID_EFFECTSEND: if (o->kind == 2) itf = ITF(o, effect_vtbl); break;
    case IID_ANDROIDCONFIGURATION: if (o->config_vtbl) itf = ITF(o, config_vtbl); break;
    }
    if (!itf) {
        static int told;
        if (told++ < 8) {
            int named = 0;
            for (int i = 0; i < iid_count; i++)
                if (id == iid_storage[i]) named = 1, droid_log("OpenSL ES: %s on object kind %d: unsupported", iid_names[i], o->kind);
            if (!named) droid_log("OpenSL ES: an unknown interface %p on object kind %d: unsupported", (void *)id, o->kind);
        }
    }
    *(sl_itf *)out = itf;
    return itf ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult obj_RegisterCallback(sl_itf self, void *cb, void *ctx) {
    (void)self; (void)cb; (void)ctx;
    return SL_RESULT_SUCCESS;
}

static void obj_AbortAsyncOperation(sl_itf self) { (void)self; }

static void obj_Destroy(sl_itf self) {
    sl_object *o = OBJ(self, object_vtbl);
    pthread_mutex_lock(&mix_lock);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i] == o) players[i] = NULL;
    pthread_mutex_unlock(&mix_lock);
    free(o);
}

static SLresult obj_SetPriority(sl_itf self, SLint32 prio, SLboolean pre) {
    (void)self; (void)prio; (void)pre;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_GetPriority(sl_itf self, SLint32 *prio, SLboolean *pre) {
    (void)self;
    *prio = 0;
    *pre = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_SetLossOfControlInterfaces(sl_itf self, int16_t n, SLInterfaceID *ids,
                                               SLboolean enabled) {
    (void)self; (void)n; (void)ids; (void)enabled;
    return SL_RESULT_SUCCESS;
}

static const void *object_vtbl[] = {
    obj_Realize, obj_Resume, obj_GetState, obj_GetInterface, obj_RegisterCallback,
    obj_AbortAsyncOperation, obj_Destroy, obj_SetPriority, obj_GetPriority,
    obj_SetLossOfControlInterfaces,
};

// ---- SLPlayItf, buffer queue, volume, effect send ------------------------------

static int sl_traced;
#define SL_TRACE(...) do { if (sl_traced++ < 24) droid_log("OpenSL ES: " __VA_ARGS__); } while (0)
static SLresult play_SetPlayState(sl_itf self, SLuint32 state) {
    sl_object *p = OBJ(self, play_vtbl);
    SL_TRACE("SetPlayState %u", (unsigned)state);
    pthread_mutex_lock(&mix_lock);
    p->play_state = (int)state;
    if (state == SL_PLAYSTATE_STOPPED) p->pos = 0;
    pthread_mutex_unlock(&mix_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(sl_itf self, SLuint32 *state) {
    *state = (SLuint32)OBJ(self, play_vtbl)->play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult play_GetMs(sl_itf self, SLuint32 *ms) {
    (void)self;
    *ms = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult play_ok(void) { SL_TRACE("a play call answered ok"); return SL_RESULT_SUCCESS; }

static const void *play_vtbl[] = {
    play_SetPlayState, play_GetPlayState, play_GetMs, play_GetMs, play_ok, play_ok, play_ok,
    play_ok, play_ok, play_ok, play_ok, play_ok,
};

static SLresult queue_Enqueue(sl_itf self, const void *buffer, SLuint32 size) {
    sl_object *p = OBJ(self, queue_vtbl);
    SL_TRACE("Enqueue %u bytes", (unsigned)size);
    // an empty buffer is taken, as on a phone: it ends at once and its callback asks for the next
    // (CRI's sound engine, Dragon Ball Legends: it starts its queue with one; refused, it never
    // played a sound and its story waited for a voice that never ended)
    if (!buffer && size) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&mix_lock);
    if (p->count == MAX_QUEUE) {
        pthread_mutex_unlock(&mix_lock);
        return SL_RESULT_BUFFER_INSUFFICIENT;
    }
    p->queue[(p->head + p->count) % MAX_QUEUE] = (queued){ buffer, size };
    p->count++;
    pthread_mutex_unlock(&mix_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_Clear(sl_itf self) {
    sl_object *p = OBJ(self, queue_vtbl);
    pthread_mutex_lock(&mix_lock);
    p->count = 0;
    p->pos = 0;
    pthread_mutex_unlock(&mix_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_GetState(sl_itf self, SLuint32 *state) {
    sl_object *p = OBJ(self, queue_vtbl);
    pthread_mutex_lock(&mix_lock);
    state[0] = p->count;  // SLBufferQueueState { count, playIndex }
    state[1] = p->played;
    pthread_mutex_unlock(&mix_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_RegisterCallback(sl_itf self, void (*cb)(sl_itf, void *), void *ctx) {
    sl_object *p = OBJ(self, queue_vtbl);
    SL_TRACE("queue RegisterCallback %p", (void *)cb);
    pthread_mutex_lock(&mix_lock);
    p->callback = droid_arm_game ? (void (*)(sl_itf, void *))arm_callback((uint64_t)cb) : cb;
    p->callback_context = ctx;
    pthread_mutex_unlock(&mix_lock);
    return SL_RESULT_SUCCESS;
}

static const void *queue_vtbl[] = {
    queue_Enqueue, queue_Clear, queue_GetState, queue_RegisterCallback,
};

static SLresult vol_SetVolumeLevel(sl_itf self, SLmillibel mb) {
    OBJ(self, volume_vtbl)->gain = powf(10.0f, mb / 2000.0f);
    return SL_RESULT_SUCCESS;
}

static SLresult vol_GetVolumeLevel(sl_itf self, SLmillibel *mb) {
    float g = OBJ(self, volume_vtbl)->gain;
    *mb = g > 0.0f ? (SLmillibel)(2000.0f * log10f(g)) : -32768;
    return SL_RESULT_SUCCESS;
}

static SLresult vol_GetMaxVolumeLevel(sl_itf self, SLmillibel *mb) {
    (void)self;
    *mb = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult vol_SetMute(sl_itf self, SLboolean mute) {
    OBJ(self, volume_vtbl)->muted = mute ? 1 : 0;
    return SL_RESULT_SUCCESS;
}

static SLresult vol_GetMute(sl_itf self, SLboolean *mute) {
    *mute = (SLboolean)OBJ(self, volume_vtbl)->muted;
    return SL_RESULT_SUCCESS;
}

static const void *volume_vtbl[] = {
    vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute, vol_GetMute,
    play_ok, play_ok, play_ok, play_ok,
};

static const void *effect_vtbl[] = { play_ok, play_ok, play_ok, play_ok, play_ok, play_ok };

// SetConfiguration / GetConfiguration / AcquireJavaProxy / ReleaseJavaProxy: taken, nothing kept
static SLresult config_GetConfiguration(sl_itf self, const char *key, SLuint32 *size, void *value) {
    (void)self; (void)key; (void)value;
    if (size) *size = 0;
    return SL_RESULT_SUCCESS;
}
static const void *config_vtbl[] = { play_ok, config_GetConfiguration, play_ok, play_ok };

// ---- SLEngineItf -----------------------------------------------------------------

// An ARM64 game calls through ARM copies of the tables: every entry a thunk.
static const void **arm_table(const void *const *vt, size_t n, const void ***cache, const char *tag) {
    if (!droid_arm_game) return (const void **)vt;
    if (!*cache) {
        const void **c = malloc(n * sizeof(*c));
        for (size_t i = 0; i < n; i++) {
            char name[48];
            snprintf(name, sizeof(name), "opensl:%s#%zu", tag, i);
            c[i] = (const void *)arm_thunk(name, (void *)vt[i]);
        }
        *cache = c;
    }
    return *cache;
}
#define VT(t) arm_table(t, sizeof(t) / sizeof(t[0]), &arm_##t, #t)
static const void **arm_object_vtbl, **arm_play_vtbl, **arm_queue_vtbl, **arm_volume_vtbl,
    **arm_effect_vtbl, **arm_engine_vtbl, **arm_config_vtbl;

static sl_object *new_object(int kind) {
    sl_object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->object_vtbl = VT(object_vtbl);
    o->engine_vtbl = NULL;
    o->play_vtbl = VT(play_vtbl);
    o->queue_vtbl = VT(queue_vtbl);
    o->volume_vtbl = VT(volume_vtbl);
    o->effect_vtbl = VT(effect_vtbl);
    o->config_vtbl = (droid_quirks & QUIRK_UNREAL) ? VT(config_vtbl) : NULL;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->play_state = SL_PLAYSTATE_STOPPED;
    o->gain = 1.0f;
    return o;
}

static SLresult engine_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult engine_CreateOutputMix(sl_itf self, sl_itf *mix, SLuint32 n,
                                       const SLInterfaceID *ids, const SLboolean *req) {
    (void)self; (void)n; (void)ids; (void)req;
    sl_object *o = new_object(1);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *mix = ITF(o, object_vtbl);
    return SL_RESULT_SUCCESS;
}

typedef struct { void *locator; void *format; } SLDataSource;

static SLresult engine_CreateAudioPlayer(sl_itf self, sl_itf *player, const SLDataSource *src,
                                         const void *sink, SLuint32 n, const SLInterfaceID *ids,
                                         const SLboolean *req) {
    (void)self; (void)sink; (void)n; (void)ids; (void)req;
    if (!src || !src->locator || !src->format) return SL_RESULT_PARAMETER_INVALID;
    uint32_t locator = *(const uint32_t *)src->locator;
    if (locator != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && locator != SL_DATALOCATOR_BUFFERQUEUE) {
        droid_log("OpenSL ES: player with locator %#x (file/URI) not supported yet", locator);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    // SLDataFormat_PCM: formatType, numChannels, samplesPerSec (mHz),
    // bitsPerSample, containerSize, channelMask, endianness[, representation]
    const uint32_t *fmt = src->format;
    if (fmt[0] != SL_DATAFORMAT_PCM && fmt[0] != SL_ANDROID_DATAFORMAT_PCM_EX)
        return SL_RESULT_CONTENT_UNSUPPORTED;
    sl_object *o = new_object(2);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->channels = fmt[1] ? fmt[1] : 1;
    o->rate = fmt[2] / 1000;
    o->bits = fmt[3];
    o->is_float = fmt[0] == SL_ANDROID_DATAFORMAT_PCM_EX && fmt[7] == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
    if (o->rate == 0 || o->channels > 2 || (!o->is_float && o->bits != 8 && o->bits != 16)) {
        droid_log("OpenSL ES: PCM %u ch, %u Hz, %u bits not supported", o->channels, o->rate,
                  o->bits);
        free(o);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    pthread_mutex_lock(&mix_lock);
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS && slot < 0; i++)
        if (!players[i]) slot = i;
    if (slot >= 0) players[slot] = o;
    pthread_mutex_unlock(&mix_lock);
    if (slot < 0) {
        free(o);
        return SL_RESULT_MEMORY_FAILURE;
    }
    droid_log("OpenSL ES: player %d: %u ch, %u Hz, %u-bit%s", slot, o->channels, o->rate,
              o->is_float ? 32 : o->bits, o->is_float ? " float" : "");
    start_mixer();
    *player = ITF(o, object_vtbl);
    return SL_RESULT_SUCCESS;
}

static SLresult engine_QueryNum(sl_itf self, SLuint32 *n) {
    (void)self;
    *n = 0;
    return SL_RESULT_SUCCESS;
}

static const void *engine_vtbl[] = {
    engine_unsupported,       // CreateLEDDevice
    engine_unsupported,       // CreateVibraDevice
    engine_CreateAudioPlayer, // CreateAudioPlayer
    engine_unsupported,       // CreateAudioRecorder
    engine_unsupported,       // CreateMidiPlayer
    engine_unsupported,       // CreateListener
    engine_unsupported,       // Create3DGroup
    engine_CreateOutputMix,   // CreateOutputMix
    engine_unsupported,       // CreateMetadataExtractor
    engine_unsupported,       // CreateExtensionObject
    engine_unsupported,       // QueryNumSupportedInterfaces
    engine_unsupported,       // QuerySupportedInterfaces
    engine_QueryNum,          // QueryNumSupportedExtensions
    engine_unsupported,       // QuerySupportedExtension
    engine_unsupported,       // IsExtensionSupported
};

static SLresult slCreateEngine(sl_itf *engine, SLuint32 n_options, const void *options,
                               SLuint32 n_interfaces, const SLInterfaceID *ids,
                               const SLboolean *req) {
    (void)n_options; (void)options; (void)n_interfaces; (void)ids; (void)req;
    sl_object *o = new_object(0);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->engine_vtbl = VT(engine_vtbl);
    *engine = ITF(o, object_vtbl);
    droid_log("OpenSL ES: engine created");
    return SL_RESULT_SUCCESS;
}

// ---- names -----------------------------------------------------------------------

void *opensl_resolve(const char *name) {
    if (strcmp(name, "slCreateEngine") == 0) return (void *)slCreateEngine;
    if (strncmp(name, "SL_IID_", 7) != 0) return NULL;
    // An SL_IID_* variable holds a pointer: give each name its own one.
    pthread_mutex_lock(&iid_lock);
    if (!iid_values[0])
        for (int i = 0; i < IID_KNOWN + 32; i++) iid_values[i] = iid_storage[i];
    int i;
    for (i = 0; i < iid_count && strcmp(iid_names[i], name) != 0; i++) {}
    if (i == iid_count && iid_count < IID_KNOWN + 32) {
        char *copy = strdup(name);
        iid_names[iid_count++] = copy;
    }
    void *p = i < iid_count ? (void *)&iid_values[i] : NULL;
    pthread_mutex_unlock(&iid_lock);
    return p;
}
