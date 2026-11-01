// native_activity.c — Android's NativeActivity for a game built on
// android_native_app_glue: the activity and its callbacks, the looper the
// game thread polls (ALooper), the input queue (AInputQueue) the DualSense
// feeds, and AConfiguration.

#include "native_activity.h"
#include "arm_cpu.h"

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game_settings.h"
#include "android_shim.h"

void arm_sig_point(void); // arm_cpu.cpp: the GC pause for a thread between waits

// ---- ALooper: poll() over the fds the game added, one looper per thread -----------------
// As Android: each thread that prepares one has its own, with a pipe ALooper_wake writes to
// (Shadow Fight 3: Unity stopped a thread of its looping in ALooper_pollOnce(-1) with
// ALooper_wake, then joined it; ours had one looper for everyone and a wake that did nothing:
// the join waited forever, the game's downloads with it). The wait goes in short steps: the
// game's GC pause reaches the thread between them.

#define ALOOPER_POLL_WAKE (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT (-3)
#define ALOOPER_POLL_ERROR (-4)
#define ALOOPER_EVENT_INPUT 1
#define MAX_FDS 16

typedef int (*looper_callback)(int fd, int events, void *data);

typedef struct {
    struct {
        int fd, ident, events;
        looper_callback callback;
        void *data;
    } fds[MAX_FDS];
    int count;
    int wake[2]; // ALooper_wake's pipe (-1: not made yet)
    pthread_mutex_t lock;
} droid_looper;

static droid_looper the_looper = { .wake = { -1, -1 }, .lock = PTHREAD_MUTEX_INITIALIZER }; // the first thread's
static int the_looper_taken;
static __thread droid_looper *my_looper;

static int looper_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("DROID_LOOPER_TRACE") != NULL;
    static int lines;
    return on && lines++ < 300;
}

static void looper_wake_pipe(droid_looper *l) {
    if (l->wake[0] >= 0) return;
    if (pipe(l->wake) != 0) {
        l->wake[0] = l->wake[1] = -1;
        return;
    }
    fcntl(l->wake[0], F_SETFL, fcntl(l->wake[0], F_GETFL) | O_NONBLOCK);
    fcntl(l->wake[1], F_SETFL, fcntl(l->wake[1], F_GETFL) | O_NONBLOCK);
}

static void *ALooper_prepare(int opts) {
    (void)opts;
    if (my_looper) return my_looper;
    if (!__atomic_exchange_n(&the_looper_taken, 1, __ATOMIC_ACQ_REL)) {
        my_looper = &the_looper;
    } else {
        droid_looper *l = calloc(1, sizeof(*l));
        if (!l) return &the_looper;
        pthread_mutex_init(&l->lock, NULL);
        l->wake[0] = l->wake[1] = -1;
        my_looper = l;
    }
    looper_wake_pipe(my_looper);
    if (looper_trace()) droid_log("looper: prepare thread %lu -> %p (wake %d/%d)", (unsigned long)pthread_self(),
                                  (void *)my_looper, my_looper->wake[0], my_looper->wake[1]);
    return my_looper;
}

// a thread that never prepared one (the UI thread: Java made its looper) shares the first
static void *ALooper_forThread(void) { return my_looper ? my_looper : (void *)&the_looper; }

static int ALooper_addFd(droid_looper *l, int fd, int ident, int events, looper_callback cb,
                         void *data) {
    if (!l) l = &the_looper;
    pthread_mutex_lock(&l->lock);
    int i;
    for (i = 0; i < l->count && l->fds[i].fd != fd; i++) {}
    if (i == l->count) {
        if (l->count == MAX_FDS) {
            pthread_mutex_unlock(&l->lock);
            return -1;
        }
        l->count++;
    }
    l->fds[i].fd = fd;
    l->fds[i].ident = ident;
    l->fds[i].events = events;
    // an ARM64 game's callback runs in the JIT
    l->fds[i].callback = droid_arm_game ? (looper_callback)arm_callback((uint64_t)cb) : cb;
    l->fds[i].data = data;
    pthread_mutex_unlock(&l->lock);
    if (looper_trace()) droid_log("looper: addFd %p thread %lu fd %d ident %d cb %p data %p", (void *)l,
                                  (unsigned long)pthread_self(), fd, ident, (void *)cb, data);
    if (l->wake[1] >= 0) { char b = 1; (void)!write(l->wake[1], &b, 1); } // a poll under way sees it
    return 1;
}

// sensors.c: its queues' pipes on the game's looper
int droid_looper_add_fd(void *looper, int fd, int ident, void *callback, void *data) {
    return ALooper_addFd((droid_looper *)looper, fd, ident, 1 /* ALOOPER_EVENT_INPUT */, (looper_callback)callback, data);
}
static int ALooper_removeFd(droid_looper *l, int fd);
int droid_looper_remove_fd(void *looper, int fd) { return ALooper_removeFd((droid_looper *)looper, fd); }
static int ALooper_removeFd(droid_looper *l, int fd) {
    if (!l) l = &the_looper;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->count; i++) {
        if (l->fds[i].fd != fd) continue;
        l->fds[i] = l->fds[--l->count];
        pthread_mutex_unlock(&l->lock);
        return 1;
    }
    pthread_mutex_unlock(&l->lock);
    return 0;
}

static int64_t looper_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data) {
    droid_looper *l = my_looper ? my_looper : &the_looper;
    { // which loopers get polled, by which threads (once each: a queue on one nobody polls is never read)
        static struct { void *l; unsigned long th; } seen[16];
        static int nseen;
        int k = 0;
        for (; k < nseen && !(seen[k].l == l && seen[k].th == (unsigned long)pthread_self()); k++) {
        }
        if (k == nseen && nseen < 16) {
            seen[nseen].l = l, seen[nseen].th = (unsigned long)pthread_self(), nseen++;
            droid_log("looper: %p polled by thread %lu (%d fds)", (void *)l, (unsigned long)pthread_self(), l->count);
        }
    }
    int64_t until = timeout_ms < 0 ? -1 : looper_now_ms() + timeout_ms;
    for (;;) {
        struct pollfd p[MAX_FDS + 1];
        pthread_mutex_lock(&l->lock);
        int n = l->count;
        for (int i = 0; i < n; i++) {
            p[i].fd = l->fds[i].fd;
            p[i].events = POLLIN;
            p[i].revents = 0;
        }
        pthread_mutex_unlock(&l->lock);
        int wake_at = -1;
        if (l->wake[0] >= 0) {
            wake_at = n;
            p[n].fd = l->wake[0];
            p[n].events = POLLIN;
            p[n].revents = 0;
        }
        int total = n + (wake_at >= 0);
        int step = 20; // ms: the GC's pause between steps
        if (until >= 0) {
            int64_t left = until - looper_now_ms();
            if (left <= 0) step = 0;
            else if (left < step) step = (int)left;
        }
        int r = poll(p, (nfds_t)total, step);
        arm_sig_point();
        if (r < 0 && errno != EINTR) return ALOOPER_POLL_ERROR;
        if (r <= 0) {
            if (until >= 0 && looper_now_ms() >= until) return ALOOPER_POLL_TIMEOUT;
            continue;
        }
        if (wake_at >= 0 && (p[wake_at].revents & POLLIN)) { // woken: the pipe emptied
            char b[64];
            while (read(l->wake[0], b, sizeof(b)) > 0) {}
            if (r == 1) return ALOOPER_POLL_WAKE;
        }
        for (int i = 0; i < n; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            pthread_mutex_lock(&l->lock);
            int k;
            for (k = 0; k < l->count && l->fds[k].fd != p[i].fd; k++) {}
            if (k == l->count) {
                pthread_mutex_unlock(&l->lock);
                continue;
            }
            int ident = l->fds[k].ident;
            looper_callback cb = l->fds[k].callback;
            void *data = l->fds[k].data;
            pthread_mutex_unlock(&l->lock);
            if (cb) {
                if (cb(p[i].fd, ALOOPER_EVENT_INPUT, data) == 0) ALooper_removeFd(l, p[i].fd);
                return ALOOPER_POLL_CALLBACK;
            }
            if (out_fd) *out_fd = p[i].fd;
            if (out_events) *out_events = ALOOPER_EVENT_INPUT;
            if (out_data) *out_data = data;
            if (looper_trace()) droid_log("looper: poll %p thread %lu -> fd %d ident %d", (void *)l,
                                          (unsigned long)pthread_self(), p[i].fd, ident);
            return ident;
        }
        if (wake_at >= 0 && (p[wake_at].revents & POLLIN)) return ALOOPER_POLL_WAKE;
    }
}

static int ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data) {
    int r;
    while ((r = ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data)) ==
           ALOOPER_POLL_CALLBACK) {}
    return r;
}

static void ALooper_wake(droid_looper *l) {
    if (!l) l = &the_looper;
    looper_wake_pipe(l);
    if (l->wake[1] >= 0) {
        char b = 1;
        (void)!write(l->wake[1], &b, 1);
    }
}
static void ALooper_acquire(droid_looper *l) { (void)l; }
static void ALooper_release(droid_looper *l) { (void)l; }

// ---- AInputQueue: events from the DualSense, a pipe wakes the looper ----

#define QUEUE_SIZE 256

typedef struct {
    int fds[2];
    droid_input_event events[QUEUE_SIZE];
    unsigned head, tail;
    pthread_mutex_t lock;
} droid_input_queue;

static droid_input_queue the_queue = { .fds = { -1, -1 }, .lock = PTHREAD_MUTEX_INITIALIZER };

static void AInputQueue_attachLooper(droid_input_queue *q, droid_looper *l, int ident,
                                     looper_callback cb, void *data) {
    ALooper_addFd(l, q->fds[0], ident, ALOOPER_EVENT_INPUT, cb, data);
}

static void AInputQueue_detachLooper(droid_input_queue *q) {
    ALooper_removeFd(&the_looper, q->fds[0]);
}

static int AInputQueue_hasEvents(droid_input_queue *q) {
    return q->head != q->tail ? 1 : 0;
}

static int AInputQueue_getEvent(droid_input_queue *q, droid_input_event **out) {
    char byte;
    pthread_mutex_lock(&q->lock);
    if (q->head == q->tail) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    (void)read(q->fds[0], &byte, 1);
    *out = &q->events[q->tail % QUEUE_SIZE];
    q->tail++;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static int AInputQueue_preDispatchEvent(droid_input_queue *q, droid_input_event *e) {
    (void)q; (void)e;
    return 0;
}

static void AInputQueue_finishEvent(droid_input_queue *q, droid_input_event *e, int handled) {
    (void)q; (void)e; (void)handled;
}

static droid_window window; // (set up below)
void activity_push_input(const droid_input_event *in) {
    droid_input_queue *q = &the_queue;
    if (q->fds[1] < 0) return;
    // Unreal games: our touch (fingers, 0-1 positions: the PC's mouse, the touchpad) as Android's
    // touchscreen MotionEvent, the first finger (it came as an unknown type: UE's buttons never
    // clicked). Other games: as before (Unity, cocos, GTA have their own input paths).
    droid_input_event touch;
    const droid_input_event *e = in;
    if (in->type == DROID_EVENT_TOUCH && (droid_quirks & QUIRK_UNREAL)) {
        static int down;
        static float last_x, last_y;
        int now = in->axes[0] > 0;
        if (!now && !down) return;
        if (now) {
            last_x = in->axes[1] * (float)window.width;
            last_y = in->axes[2] * (float)window.height;
        }
        memset(&touch, 0, sizeof(touch));
        touch.type = 2;          // AINPUT_EVENT_TYPE_MOTION
        touch.source = 0x1002;   // AINPUT_SOURCE_TOUCHSCREEN
        touch.action = now ? (down ? 2 : 0) : 1; // MOVE / DOWN / UP
        touch.axes[0] = last_x;  // AXIS_X (getX)
        touch.axes[1] = last_y;  // AXIS_Y (getY)
        touch.axes[2] = now ? 1.0f : 0.0f; // AXIS_PRESSURE
        down = now;
        e = &touch;
    }
    pthread_mutex_lock(&q->lock);
    if (q->head - q->tail < QUEUE_SIZE) {
        q->events[q->head % QUEUE_SIZE] = *e;
        q->head++;
        char byte = 1;
        (void)write(q->fds[1], &byte, 1);
    }
    pthread_mutex_unlock(&q->lock);
}

// ---- AConfiguration: a TV, landscape, English -----------------------------

typedef struct { int unused; } droid_config;

static droid_config *AConfiguration_new(void) { return calloc(1, sizeof(droid_config)); }
static void AConfiguration_delete(droid_config *c) { free(c); }
static void AConfiguration_fromAssetManager(droid_config *c, void *am) { (void)c; (void)am; }
static int32_t cfg_zero(droid_config *c) { (void)c; return 0; }
static int32_t AConfiguration_getOrientation(droid_config *c) { (void)c; return phone_view() ? 1 : 2; } // port / land
static int32_t AConfiguration_getDensity(droid_config *c) { (void)c; return 320; }
static int32_t AConfiguration_getScreenSize(droid_config *c) { (void)c; return 4; } // xlarge
static int32_t AConfiguration_getSdkVersion(droid_config *c) { (void)c; return 30; }
static int32_t AConfiguration_getUiModeType(droid_config *c) { (void)c; return 4; } // TV
static int32_t AConfiguration_getTouchscreen(droid_config *c) { (void)c; return 1; } // none
static void AConfiguration_getLanguage(droid_config *c, char *out) { (void)c; out[0] = 'e'; out[1] = 'n'; }
static void AConfiguration_getCountry(droid_config *c, char *out) { (void)c; out[0] = 'U'; out[1] = 'S'; }

// ---- the activity --------------------------------------------------------

static droid_activity_callbacks callbacks;
static droid_activity activity;
static droid_window window = { .width = 1920, .height = 1080 };
static char obb_dir[300];

void activity_set_obb(const char *dir) { snprintf(obb_dir, sizeof(obb_dir), "%s", dir); }

static int32_t ANativeWindow_getWidth(droid_window *w) { return w->width; }
static int32_t ANativeWindow_getHeight(droid_window *w) { return w->height; }
static int32_t ANativeWindow_getFormat(droid_window *w) { (void)w; return 1; } // RGBA_8888
static int32_t ANativeWindow_setBuffersGeometry(droid_window *w, int32_t wd, int32_t h, int32_t f) {
    (void)w; (void)wd; (void)h; (void)f;
    return 0;
}
static void ANativeWindow_acquire(droid_window *w) { (void)w; }
static void ANativeWindow_release(droid_window *w) { (void)w; }
static void ANativeActivity_finish(droid_activity *a) { (void)a; droid_log("game asked to finish"); }
static void ANativeActivity_setWindowFlags(droid_activity *a, uint32_t add, uint32_t remove) {
    (void)a; (void)add; (void)remove;
}

// (its size: the game's screen, 1080x1920 in the phone view; set when it's first asked for)
static droid_window *sized(void) {
    window.width = droid_game_w, window.height = droid_game_h;
    return &window;
}
droid_window *activity_window(void) { return sized(); }
void *activity_window_ptr(void) { return sized(); }

int activity_start(void *on_create, void *vm, void *env, void *clazz) {
    if (pipe(the_queue.fds) != 0) {
        droid_log("activity: input pipe failed");
        return -1;
    }
    activity.callbacks = &callbacks;
    activity.vm = vm;
    activity.env = env;
    activity.clazz = clazz;
    activity.internal_data_path = game_dir;
    activity.external_data_path = game_dir;
    activity.sdk_version = 30;
    activity.asset_manager = (void *)&activity; // non-NULL; no assets read yet
    activity.obb_path = obb_dir[0] ? obb_dir : game_dir;

    typedef void (*create_fn)(droid_activity *, void *, size_t);
    ((create_fn)on_create)(&activity, NULL, 0);
    droid_log("activity: onCreate done, starting the lifecycle");
    if (droid_arm_game) {
        void **cb = (void **)&callbacks;
        for (size_t i = 0; i < sizeof(callbacks) / sizeof(void *); i++)
            cb[i] = arm_callback((uint64_t)cb[i]);
    }

    if (callbacks.on_start) callbacks.on_start(&activity);
    if (callbacks.on_resume) callbacks.on_resume(&activity);
    if (callbacks.on_input_queue_created) callbacks.on_input_queue_created(&activity, &the_queue);
    if (callbacks.on_native_window_created) callbacks.on_native_window_created(&activity, &window);
    if (callbacks.on_window_focus_changed) callbacks.on_window_focus_changed(&activity, 1);
    droid_log("activity: resumed, window and input queue given");
    return 0;
}

#define E(name) { #name, (void *)name }
static const struct { const char *name; void *fn; } table[] = {
    E(ALooper_prepare), E(ALooper_forThread), E(ALooper_addFd), E(ALooper_removeFd),
    E(ALooper_pollOnce), E(ALooper_pollAll), E(ALooper_wake), E(ALooper_acquire),
    E(ALooper_release),
    E(AInputQueue_attachLooper), E(AInputQueue_detachLooper), E(AInputQueue_hasEvents),
    E(AInputQueue_getEvent), E(AInputQueue_preDispatchEvent), E(AInputQueue_finishEvent),
    E(AConfiguration_new), E(AConfiguration_delete), E(AConfiguration_fromAssetManager),
    E(AConfiguration_getOrientation), E(AConfiguration_getDensity),
    E(AConfiguration_getScreenSize), E(AConfiguration_getSdkVersion),
    E(AConfiguration_getUiModeType), E(AConfiguration_getTouchscreen),
    E(AConfiguration_getLanguage), E(AConfiguration_getCountry),
    { "AConfiguration_getMcc", (void *)cfg_zero }, { "AConfiguration_getMnc", (void *)cfg_zero },
    { "AConfiguration_getKeyboard", (void *)cfg_zero },
    { "AConfiguration_getNavigation", (void *)cfg_zero },
    { "AConfiguration_getKeysHidden", (void *)cfg_zero },
    { "AConfiguration_getNavHidden", (void *)cfg_zero },
    { "AConfiguration_getScreenLong", (void *)cfg_zero },
    { "AConfiguration_getUiModeNight", (void *)cfg_zero },
    E(ANativeWindow_getWidth), E(ANativeWindow_getHeight), E(ANativeWindow_getFormat),
    E(ANativeWindow_setBuffersGeometry), E(ANativeWindow_acquire), E(ANativeWindow_release),
    E(ANativeActivity_finish), E(ANativeActivity_setWindowFlags),
};

void *activity_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}
