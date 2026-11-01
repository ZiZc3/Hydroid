// sensors.c — Android's motion sensors (NDK ASensorManager / ASensorEventQueue) from the
// DualSense: its accelerometer and gyroscope (app_main.c reads them with the pad, into
// droid_motion), as a phone's. A game asks for a sensor, enables it on a queue tied to its
// looper, and gets events at the rate it set. Tilt is the accelerometer's gravity; a shake is
// its jolt (the same data: a phone's games find it themselves).
//
// Per game (Settings > a game's Options > Controls > Motion): Auto (the games known to use it),
// On, Off. Off, a game sees no sensors at all, as before (a phone without them).

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "android_shim.h"
#include "game_settings.h"
#include "arm_cpu.h"

// the newest sample (app_main.c's pad_poll writes it)
static pthread_mutex_t motion_lock = PTHREAD_MUTEX_INITIALIZER;
static droid_motion_sample motion_now;
static int64_t motion_seq;
void droid_motion_put(const droid_motion_sample *m) {
    pthread_mutex_lock(&motion_lock);
    motion_now = *m;
    motion_seq++;
    pthread_mutex_unlock(&motion_lock);
}
static int64_t motion_get(droid_motion_sample *m) {
    static int fake = -1; // PC tests: DROID_FAKE_TILT=<degrees> rolls a still phone that much
    if (fake < 0) fake = getenv("DROID_FAKE_TILT") != NULL;
    if (fake) {
        const float d = (float)atof(getenv("DROID_FAKE_TILT")) * 3.14159f / 180;
        memset(m, 0, sizeof(*m));
        m->accel[0] = 9.80665f * sinf(d), m->accel[1] = 9.80665f * cosf(d) * 0.7f, m->accel[2] = 9.80665f * cosf(d) * 0.7f;
        return 1;
    }
    pthread_mutex_lock(&motion_lock);
    *m = motion_now;
    int64_t s = motion_seq;
    pthread_mutex_unlock(&motion_lock);
    return s;
}

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// the games that use tilt (Auto turns it on for them)
#define MOTION_GAMES "com.imangi.templerun2,com.sega.sonicboomandroid" // (Sonic Boom: tilt steers its Enerbeam)
int droid_motion_wanted(void) {
    static int said = -1;
    int on = settings.motion == 1 || (settings.motion == 0 && droid_game_in(MOTION_GAMES)) ||
             getenv("DROID_FAKE_TILT") != NULL; // (PC tests)
    if (said != on) {
        said = on;
        droid_log("sensors: the DualSense's motion %s (Controls > Motion: %s)", on ? "is the phone's" : "off",
                  settings.motion == 1 ? "On" : settings.motion == 2 ? "Off" : "Auto");
    }
    return on;
}

// ---- the sensors: Android's types 1 (accelerometer) and 4 (gyroscope)
typedef struct {
    int type, handle;
    const char *name;
    float resolution, range;
    int min_delay_us;
} droid_sensor;
static const droid_sensor sensors[] = {
    { 1, 1, "DualSense Accelerometer", 0.0024f, 78.45f, 4000 },
    { 4, 2, "DualSense Gyroscope", 0.0011f, 34.91f, 4000 },
};
#define NSENSORS 2
static int sensor_manager;

static void spy(const char *what, int v) { // InputSpy: what a game asks of the sensors (once each)
    static char seen[64][48];
    static int n;
    char key[48];
    snprintf(key, sizeof(key), "%s %d", what, v);
    for (int i = 0; i < n; i++)
        if (!strcmp(seen[i], key)) return;
    if (n < 64) snprintf(seen[n++], sizeof(seen[0]), "%s", key);
    droid_log("input: the game %s %d", what, v);
}

static void *s_get_instance(void) { return &sensor_manager; }
static int s_get_list(void *m, const void ***list) {
    (void)m;
    static const void *all[NSENSORS + 1];
    for (int i = 0; i < NSENSORS; i++) all[i] = &sensors[i];
    *list = all;
    spy("lists the sensors", 0);
    return droid_motion_wanted() ? NSENSORS : 0;
}
static const void *s_default_sensor(void *m, int type) {
    (void)m;
    spy("asks for sensor type", type);
    if (!droid_motion_wanted()) return NULL;
    for (int i = 0; i < NSENSORS; i++)
        if (sensors[i].type == type) return &sensors[i];
    return NULL;
}
static const char *s_name(const droid_sensor *s) { return s ? s->name : "none"; }
static const char *s_vendor(const droid_sensor *s) { (void)s; return "Sony (Hydroid)"; }
static int s_type(const droid_sensor *s) { return s ? s->type : 0; }
static float s_resolution(const droid_sensor *s) { return s ? s->resolution : 0; }
static int s_min_delay(const droid_sensor *s) { return s ? s->min_delay_us : 0; }
static int s_handle(const droid_sensor *s) { return s ? s->handle : 0; }
static float s_max_range(const droid_sensor *s) { return s ? s->range : 0; }

// ---- the queues: a pipe the game's looper watches; a byte in it when events are due
#define MAX_QUEUES 8
typedef struct {
    int used, fds[2], pending;
    void *looper;
    int (*callback)(int fd, int events, void *data); // the game's (ARM code: through arm_callback), or NULL
    void *data;
    int64_t period_ns[NSENSORS], last_ns[NSENSORS];
    int enabled[NSENSORS];
} sensor_queue;
static sensor_queue queues[MAX_QUEUES];
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;

int droid_looper_add_fd(void *looper, int fd, int ident, void *callback, void *data); // native_activity.c
int droid_looper_remove_fd(void *looper, int fd);

static void *sensor_thread(void *arg) { // every 4 ms: a queue with an event due gets its byte
    (void)arg;
    for (;;) {
        struct timespec d = { 0, 4000000 };
        nanosleep(&d, NULL);
        int64_t t = now_ns();
        pthread_mutex_lock(&q_lock);
        for (int i = 0; i < MAX_QUEUES; i++) {
            sensor_queue *q = &queues[i];
            if (!q->used || q->pending) continue;
            for (int s = 0; s < NSENSORS; s++)
                if (q->enabled[s] && t - q->last_ns[s] >= q->period_ns[s]) {
                    q->pending = 1;
                    char b = 1;
                    (void)!write(q->fds[1], &b, 1);
                    break;
                }
        }
        pthread_mutex_unlock(&q_lock);
    }
    return NULL;
}

static sensor_queue *s_create_queue(void *m, void *looper, int ident, void *callback, void *data) {
    (void)m;
    static int started;
    pthread_mutex_lock(&q_lock);
    sensor_queue *q = NULL;
    for (int i = 0; i < MAX_QUEUES && !q; i++)
        if (!queues[i].used) q = &queues[i];
    if (!q || pipe(q->fds) != 0) {
        pthread_mutex_unlock(&q_lock);
        return NULL;
    }
    fcntl(q->fds[0], F_SETFL, O_NONBLOCK);
    fcntl(q->fds[1], F_SETFL, O_NONBLOCK);
    q->used = 1, q->pending = 0, q->looper = looper;
    // a queue with a callback is Android's main looper's to serve (Unity's): Java's main thread
    // calls it (droid_sensors_serve) - our main looper doesn't poll native fds
    q->callback = callback ? (droid_arm_game ? (int (*)(int, int, void *))arm_callback((uint64_t)(uintptr_t)callback)
                                             : (int (*)(int, int, void *))callback)
                           : NULL;
    q->data = data;
    for (int s = 0; s < NSENSORS; s++) q->enabled[s] = 0, q->period_ns[s] = 20000000, q->last_ns[s] = 0;
    if (!started) {
        pthread_t th;
        started = pthread_create(&th, NULL, sensor_thread, NULL) == 0;
        if (started) pthread_detach(th);
    }
    pthread_mutex_unlock(&q_lock);
    droid_looper_add_fd(looper, q->fds[0], ident, callback, data);
    spy("makes a sensor queue, ident", ident);
    droid_log("sensors: the queue on looper %p (thread %lu, a callback %s)", looper, (unsigned long)pthread_self(),
              callback ? "yes" : "no");
    return q;
}
static int s_destroy_queue(void *m, sensor_queue *q) {
    (void)m;
    if (!q) return -1;
    droid_looper_remove_fd(q->looper, q->fds[0]);
    pthread_mutex_lock(&q_lock);
    close(q->fds[0]);
    close(q->fds[1]);
    memset(q, 0, sizeof(*q));
    pthread_mutex_unlock(&q_lock);
    return 0;
}
static int sensor_index(const droid_sensor *s) {
    for (int i = 0; i < NSENSORS; i++)
        if (s == &sensors[i]) return i;
    return -1;
}
static int s_enable(sensor_queue *q, const droid_sensor *s) {
    int k = sensor_index(s);
    if (!q || k < 0) return -1;
    spy("turns on sensor type", s->type);
    pthread_mutex_lock(&q_lock);
    q->enabled[k] = 1;
    pthread_mutex_unlock(&q_lock);
    return 0;
}
static int s_disable(sensor_queue *q, const droid_sensor *s) {
    int k = sensor_index(s);
    if (!q || k < 0) return -1;
    pthread_mutex_lock(&q_lock);
    q->enabled[k] = 0;
    pthread_mutex_unlock(&q_lock);
    return 0;
}
static int s_set_rate(sensor_queue *q, const droid_sensor *s, int32_t usec) {
    int k = sensor_index(s);
    if (!q || k < 0) return -1;
    if (usec < 4000) usec = 4000;
    pthread_mutex_lock(&q_lock);
    q->period_ns[k] = (int64_t)usec * 1000;
    pthread_mutex_unlock(&q_lock);
    return 0;
}
static int s_has_events(sensor_queue *q) {
    spy("asks if sensor events wait", 0);
    return q && q->pending ? 1 : 0;
}

// Android's ASensorEvent (104 bytes)
typedef struct {
    int32_t version, sensor, type, reserved0;
    int64_t timestamp;
    float data[16];
    uint32_t flags;
    int32_t reserved1[3];
} a_sensor_event;
_Static_assert(sizeof(a_sensor_event) == 104, "ASensorEvent is 104 bytes");

// Games steered by sideways tilt (Temple Run 2): a held DualSense sits rolled 9-35 degrees to
// one side (user's log, 2026-11-01), which the game read as a lean - the runner drifted on his
// own. Its sideways axis (x) here: the way it's held over the last few seconds is "straight"
// (a slow average), and under ~9 degrees from that is no tilt; a quick real tilt still steers.
#define TILT_LEVEL_GAMES "com.imangi.templerun2"
#define TILT_LEVEL_SECONDS 4.0f
#define TILT_DEAD_ZONE 1.5f // m/s2 (~9 degrees)
static float tilt_level(float x, int64_t t) {
    static int on = -1;
    static float neutral;
    static int64_t last;
    if (on < 0) {
        on = droid_game_in(TILT_LEVEL_GAMES) != 0;
        if (on) droid_log("sensors: sideways tilt levelled to how the pad is held, dead zone %.1f m/s2 (TILT_LEVEL_GAMES)", TILT_DEAD_ZONE);
    }
    if (!on) return x;
    if (!last) neutral = x;
    const float dt = last ? (float)(t - last) / 1e9f : 0;
    last = t;
    const float k = dt > 0 ? fminf(1.0f, dt / TILT_LEVEL_SECONDS) : 0;
    neutral += (x - neutral) * k;
    const float d = x - neutral;
    return fabsf(d) <= TILT_DEAD_ZONE ? 0.0f : d - copysignf(TILT_DEAD_ZONE, d);
}

static long s_get_events(sensor_queue *q, a_sensor_event *ev, size_t count) {
    if (!q || !ev) return -1;
    char drain[64];
    while (read(q->fds[0], drain, sizeof(drain)) > 0) {
    }
    droid_motion_sample m;
    motion_get(&m);
    int64_t t = now_ns();
    long n = 0;
    pthread_mutex_lock(&q_lock);
    for (int s = 0; s < NSENSORS && (size_t)n < count; s++) {
        if (!q->enabled[s] || t - q->last_ns[s] < q->period_ns[s]) continue;
        q->last_ns[s] = t;
        a_sensor_event *e = &ev[n++];
        memset(e, 0, sizeof(*e));
        e->version = (int32_t)sizeof(*e);
        e->sensor = sensors[s].handle;
        e->type = sensors[s].type;
        e->timestamp = m.ns ? m.ns : t;
        const float *v = sensors[s].type == 1 ? m.accel : m.gyro;
        e->data[0] = v[0], e->data[1] = v[1], e->data[2] = v[2];
        if (sensors[s].type == 1) e->data[0] = tilt_level(v[0], t); // (TILT_LEVEL_GAMES only)
        ((int8_t *)&e->data[3])[0] = 3; // SENSOR_STATUS_ACCURACY_HIGH
    }
    q->pending = 0;
    pthread_mutex_unlock(&q_lock);
    static long calls, given;
    calls++, given += n;
    if (calls == 1 || calls % 600 == 0) // InputSpy: the game reads them, and what it got
        droid_log("sensors: the game read events %ld times (%ld events); now accel %.2f %.2f %.2f m/s2", calls, given,
                  m.accel[0], m.accel[1], m.accel[2]);
    return n;
}

// Java's main thread, between its messages: the queues with a callback and events waiting get it
// (the game reads them there: getEvents)
void droid_sensors_serve(void) {
    for (int i = 0; i < MAX_QUEUES; i++) {
        sensor_queue *q = &queues[i];
        if (!q->used || !q->pending || !q->callback) continue;
        static int told;
        if (!told++) droid_log("sensors: the game's callback served on Java's main thread");
        q->callback(q->fds[0], 1 /* ALOOPER_EVENT_INPUT */, q->data);
    }
}

// the names for the loader (linux_abi.c's table)
#define S(name, fn) { name, (void *)fn }
const struct droid_sym { const char *name; void *fn; } droid_sensor_syms[] = {
    S("ASensorManager_getInstance", s_get_instance), S("ASensorManager_getInstanceForPackage", s_get_instance),
    S("ASensorManager_getSensorList", s_get_list), S("ASensorManager_getDefaultSensor", s_default_sensor),
    S("ASensorManager_createEventQueue", s_create_queue), S("ASensorManager_destroyEventQueue", s_destroy_queue),
    S("ASensorEventQueue_enableSensor", s_enable), S("ASensorEventQueue_disableSensor", s_disable),
    S("ASensorEventQueue_setEventRate", s_set_rate), S("ASensorEventQueue_hasEvents", s_has_events),
    S("ASensorEventQueue_getEvents", s_get_events), S("ASensor_getName", s_name), S("ASensor_getVendor", s_vendor),
    S("ASensor_getType", s_type), S("ASensor_getResolution", s_resolution), S("ASensor_getMinDelay", s_min_delay),
    S("ASensor_getHandle", s_handle), S("ASensor_getMaxRange", s_max_range),
    { NULL, NULL },
};
