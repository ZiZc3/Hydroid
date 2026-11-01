// Android title: runs an Android game's .so on the PS5.
// Runs as an installed title (/data/homebrew/<title id>), so it has video out.
//
// /app0/lib/libgame.so present: a NativeActivity game (android_native_app_glue).
// Otherwise /app0/lib/libdroid_game.so, our test game (phases 1-3).

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef DROID_PC
#include <execinfo.h>
#endif
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "FalsoJNI.h"
#include "game_settings.h"
#include "quick_menu.h"
#include "version.h"
#include "android_shim.h"
#include "apk.h"
#include "arm_cpu.h"
#include "display.h"
#include "godot_host.h"
#include "gameactivity_host.h"
#include "gtasa_host.h"
#include "cocos_host.h"
#include "mortar_host.h"
#include "unity_host.h"
#include "unity_patch.h"
#include "apk_sign.h"
#include "game_file.h"
#include "arm_modules.h"
#include "jni_impl.h"
#include "java_layer.h"
#include "native_activity.h"
#include "soloader.h"

int sceKernelSendNotificationRequest(int device, const void *buf, int size, int blocking);
int sceSystemServiceHideSplashScreen(void);
int sceKernelUsleep(uint32_t us);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *params);
// the classic two motors (VLC-PS5's): mode 2 ("compatible") first, then their strengths 0-255
typedef struct { uint8_t large_motor, small_motor; } pad_vibration;
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetMotionSensorState(int32_t handle, int32_t enable);
int scePadSetVibration(int32_t handle, const pad_vibration *vibration);
int droid_import_missing(const char *name); // below: an import the console gave no code
int scePadRead(int32_t handle, void *samples, int32_t capacity);

#define LOG_PATH APP0 "/android.log"
#define TEST_LIB APP0 "/lib/libdroid_game.so"
#define GAME_LIB APP0 "/lib/libgame.so"

struct notification_request {
    int type; int req_id; int priority; int msg_id; int target_id;
    int user_id; int unk1; int unk2; int app_id; int error_num; int unk3;
    char use_icon_image_uri;
    char message[1024]; char uri[1024]; char unk[1024];
};

void droid_bench(const char *file);     // linux_abi.c
void droid_readahead(const char *path); // linux_abi.c
void droid_log_managed_exceptions(void); // bionic_shim.c

static FILE *log_file;

static void notify(const char *msg) {
    struct notification_request n;
    memset(&n, 0, sizeof(n));
    n.target_id = -1;
    strncpy(n.message, msg, sizeof(n.message) - 1);
    sceKernelSendNotificationRequest(0, &n, sizeof(n), 0);
}

// The log is written by its own thread: a game thread only copies its line into
// memory (writing + flushing each line to disk and klog cost ~1 ms, every frame:
// stutter). Flushed every 50 ms, and at once on a crash (log_flush_now).
#define LOG_RING (1u << 20)
static char log_ring[LOG_RING];
static size_t log_head, log_tail; // bytes put in / taken out (ever)
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_writer_started;

// The live log: tools/live_log.py on the PC connects to port 9025 and reads the log as it is
// written, the last 256 KB first (no FTP, no 50 MB file). One reader at a time; a reader that
// falls behind skips ahead. Our own thread; the game's threads never wait for the network.
#define NET_RING (256u << 10)
#define LOG_PORT 9025
static char net_ring[NET_RING];
static size_t net_head; // bytes ever put
static int net_reader;  // someone connected: worth copying for
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t net_more = PTHREAD_COND_INITIALIZER;

static void net_put(const char *p, size_t n) {
    pthread_mutex_lock(&net_lock);
    for (size_t i = 0; i < n; i++) net_ring[(net_head + i) % NET_RING] = p[i];
    net_head += n;
    if (net_reader) pthread_cond_signal(&net_more);
    pthread_mutex_unlock(&net_lock);
}

static void *log_server(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (s >= 0) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(LOG_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
        droid_log("live log: port %d not open (errno %d)", LOG_PORT, errno);
        if (s >= 0) close(s);
        return NULL;
    }
    droid_log("live log: port %d open (tools/live_log.py)", LOG_PORT);
    static char chunk[16 * 1024];
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            usleep(500 * 1000);
            continue;
        }
        struct timeval tv = { 2, 0 }; // a reader that stops reading: dropped after 2 s
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        pthread_mutex_lock(&net_lock);
        net_reader = 1;
        size_t at = net_head > NET_RING ? net_head - NET_RING : 0;
        for (;;) {
            while (at == net_head) pthread_cond_wait(&net_more, &net_lock);
            if (net_head - at > NET_RING) at = net_head - NET_RING; // behind: the oldest it can still have
            size_t n = net_head - at;
            if (n > sizeof(chunk)) n = sizeof(chunk);
            for (size_t i = 0; i < n; i++) chunk[i] = net_ring[(at + i) % NET_RING];
            at += n;
            pthread_mutex_unlock(&net_lock);
            size_t done = 0;
            while (done < n) {
                ssize_t r = send(c, chunk + done, n - done, 0);
                if (r <= 0) break;
                done += (size_t)r;
            }
            pthread_mutex_lock(&net_lock);
            if (done < n) break;
        }
        net_reader = 0;
        pthread_mutex_unlock(&net_lock);
        close(c);
    }
    return NULL;
}

static void log_flush_now(void);
void droid_log_flush(void) { log_flush_now(); }

// The startup timeline: from the game's pick, each step's time ("startup: 12.3 s <step>"), to see
// where a long black screen goes (every engine: the libraries, their startup code, the first frame)
static int64_t startup_at_ns, startup_done;
static int64_t startup_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
void droid_startup_begin(void) {
    startup_at_ns = startup_now_ns();
    startup_done = 0;
    droid_log("startup: 0.0 s the game picked");
}
void droid_startup(const char *fmt, ...) {
    if (!startup_at_ns || startup_done) return;
    char what[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    droid_log("startup: %.1f s %s", (startup_now_ns() - startup_at_ns) / 1e9, what);
}
// the first frame on the screen: the timeline's end
void droid_startup_frame(const char *how) {
    if (!startup_at_ns || startup_done) return;
    droid_startup("first frame on the screen (%s)", how);
    startup_done = 1;
} // (arm_cpu.cpp: before the process ends)
void arm_calldiff_flush(void); // arm_cpu.cpp: CallDiff's file too
static void log_flush_now(void) {
    arm_calldiff_flush();
    static char out[64 * 1024];
    static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&out_lock);
    for (;;) {
        pthread_mutex_lock(&log_lock);
        size_t n = log_head - log_tail;
        if (n > sizeof(out)) n = sizeof(out);
        for (size_t i = 0; i < n; i++) out[i] = log_ring[(log_tail + i) % LOG_RING];
        log_tail += n;
        pthread_mutex_unlock(&log_lock);
        if (!n) break;
        if (log_file) fwrite(out, 1, n, log_file);
        fwrite(out, 1, n, stderr);
        net_put(out, n); // the live log's reader (log_server)
    }
    if (log_file) fflush(log_file);
    fflush(stderr);
    pthread_mutex_unlock(&out_lock);
}

static void *log_writer(void *arg) {
    (void)arg;
    for (;;) {
        usleep(50 * 1000);
        log_flush_now();
    }
    return NULL;
}

static void say(int show, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    pthread_mutex_lock(&log_lock);
    if (log_head - log_tail + (size_t)n <= LOG_RING) { // full: the line is dropped
        for (int i = 0; i < n; i++) log_ring[(log_head + (size_t)i) % LOG_RING] = buf[i];
        log_head += (size_t)n;
    }
    int start = !log_writer_started;
    log_writer_started = 1;
    pthread_mutex_unlock(&log_lock);
    if (start) {
        pthread_t t;
        pthread_create(&t, NULL, log_writer, NULL);
        pthread_detach(t);
    }
    if (show) {
        buf[n - 1] = 0;
        extern char droid_last_shown[300];
        snprintf(droid_last_shown, sizeof(droid_last_shown), "%s", !strncmp(buf, "Android: ", 9) ? buf + 9 : buf);
        notify(buf);
        log_flush_now(); // shown on screen: in the file too, now
    }
}

void droid_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    say(0, "%s", buf);
}

static void missing(const char *name) {
    say(0, "missing import: %s", name);
}

// The game on its way: when it can't go on, the app closes saying why
// with why (data/return.txt "error"), never a black screen
char droid_last_shown[300]; // say(1, ...)'s last line
static volatile int game_started;
static void back_with_error(const char *msg) {
#ifndef DROID_PC
    static volatile int once;
    if (!game_started || once++) return;
    log_flush_now();
    quick_menu_relaunch_msg("error", msg);
#else
    (void)msg;
#endif
}

static void halt(void) {
    log_flush_now();
    if (game_started) {
        char msg[400];
        snprintf(msg, sizeof(msg), "The game couldn't start: %s", droid_last_shown[0] ? droid_last_shown : "see android.log");
        back_with_error(msg);
    }
    sceKernelUsleep(5000000); // the notification stays readable
    exit(1);
}

// ---- DualSense -> Android input events --------------------------------------

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t reserved_to_connected[66];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
} pad_sample;
_Static_assert(sizeof(pad_sample) == 120, "pad samples are 120 bytes");
#define PAD_INTERCEPTED 0x80000000u

#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2
#define AINPUT_SOURCE_GAMEPAD 0x401
#define AINPUT_SOURCE_JOYSTICK 0x1000010
#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2

static const struct { uint32_t ps5; int32_t keycode; } key_map[] = {
    { 0x4000, 96 },    // Cross    -> BUTTON_A
    { 0x2000, 97 },    // Circle   -> BUTTON_B
    { 0x8000, 99 },    // Square   -> BUTTON_X
    { 0x1000, 100 },   // Triangle -> BUTTON_Y
    { 0x400, 102 },    // L1
    { 0x800, 103 },    // R1
    { 0x2, 106 },      // L3 -> THUMBL
    { 0x4, 107 },      // R3 -> THUMBR
    { 0x8, 108 },      // Options  -> START
    { 0x100000, 109 }, // Touchpad -> SELECT
    { 0x10, 19 },      // D-pad up
    { 0x40, 20 },      // down
    { 0x80, 21 },      // left
    { 0x20, 22 },      // right
};

typedef void (*input_sink)(const droid_input_event *);

static float axis(uint8_t v) {
    float f = (v - 127.5f) / 127.5f;
    return (f > -0.12f && f < 0.12f) ? 0.0f : f;
}

static int pad = -1;
static uint32_t pad_prev;
static uint8_t prev_axes[6];

static int rumble_ok;        // the pad's vibration mode set (the calls have code on this console)
static double rumble_until; // when the rumble stops (0: still)
static double pad_now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void pad_open(void) {
    if (pad >= 0) return; // opened already
    int32_t user = -1;
    int ui = sceUserServiceInitialize(NULL);
    int pi = scePadInit();
    int gu = sceUserServiceGetInitialUser(&user);
    pad = scePadOpen(user, 0, 0, NULL);
    int mode = -1;
#ifndef DROID_PC
    // as VLC-PS5: only when the loader gave them code, and after the mode (else the motors stay still)
    if (pad >= 0 && !droid_import_missing("scePadSetVibrationMode") && !droid_import_missing("scePadSetVibration"))
        rumble_ok = (mode = scePadSetVibrationMode(pad, 2)) == 0;
#endif
    int motion_rc = -1; // the accelerometer and gyroscope in the samples (sensors.c: a phone's)
#ifndef DROID_PC
    if (pad >= 0 && !droid_import_missing("scePadSetMotionSensorState")) motion_rc = scePadSetMotionSensorState(pad, 1);
#endif
    say(0, "pad: user service %#x, pad init %#x, user %d (%#x), handle %#x, vibration %s (%#x), motion %#x", ui, pi, user, gu,
        pad, rumble_ok ? "on" : "off", mode, motion_rc);
}

// a short rumble (the menus): strength 0-1, for ms; menu_pad stops it
void droid_pad_rumble(float strength, int ms) {
    if (!rumble_ok || pad < 0 || !app_settings.vibration) return;
    int v = (int)(strength * 255 + 0.5f);
    v = v < 0 ? 0 : v > 255 ? 255 : v;
#ifndef DROID_PC
    pad_vibration p = { (uint8_t)v, (uint8_t)v };
    scePadSetVibration(pad, &p);
#endif
    rumble_until = pad_now_s() + ms / 1000.0;
}
static void rumble_tick(void) {
    if (!rumble_until || pad_now_s() < rumble_until) return;
    rumble_until = 0;
#ifndef DROID_PC
    pad_vibration p = { 0, 0 };
    scePadSetVibration(pad, &p);
#endif
}

// The menus' view of the DualSense (the Quick menu): its newest buttons and left stick.
static int menu_touch_n; // the touchpad's first finger, as menu_pad last read it
static float menu_touch_x, menu_touch_y;
void droid_menu_touch(int *n, float *x, float *y) { *n = menu_touch_n, *x = menu_touch_x, *y = menu_touch_y; }
static void menu_pad(uint32_t *buttons, uint8_t *lx, uint8_t *ly) {
    pad_open();
    if (pad < 0) return;
    rumble_tick();
    pad_sample samples[16];
    int n = scePadRead(pad, samples, 16);
    if (n <= 0 || n > 16) return;
    const pad_sample *p = &samples[0];
    for (int i = 1; i < n; i++)
        if (samples[i].timestamp_us > p->timestamp_us) p = &samples[i];
    if (!p->connected || (p->buttons & PAD_INTERCEPTED)) return;
    *buttons = p->buttons;
    *lx = p->lx;
    *ly = p->ly;
    const uint8_t *t = p->reserved_to_connected + 42; // the touchpad (as pad_poll reads it)
    menu_touch_n = t[0] <= 2 ? t[0] : 0;
    menu_touch_x = (t[8] | t[9] << 8) / 1919.0f, menu_touch_y = (t[10] | t[11] << 8) / 1079.0f;
}

// The Quick menu's view of it (quick_menu.c: read on the swap's thread while the game is paused)
void droid_menu_pad(uint32_t *buttons, uint8_t *lx, uint8_t *ly) { menu_pad(buttons, lx, ly); }

// Touchpad fingers as events too (DROID_EVENT_TOUCH): only for Unity's sink, the
// others don't know the kind.
static int pad_touch_events;
// The touchpad as the touchscreen. A finger that lands waits a moment (TP_WAIT) before it's the
// game's: a click of the touchpad in that time is the hosts' tap at the cursor (keycode 109), and
// that finger never touches (a click always lands a finger first). Moved, a second finger, or held
// past it: the game's touch, from where it landed. Off again before it: a tap there.
#define TP_WAIT 0.09
static int tp_mode;            // 0 no finger, 1 one waiting, 2 the game's touch, 3 a click (no touch till all up)
static double tp_since;
static float tp_land[2][3];    // where the fingers landed (x, y 0..1, id)
static int tp_land_n;
static float tp_sent[2][3];    // what the game has
static int tp_sent_n = -1;

static void tp_send(input_sink sink, int n, float xy[2][3]) {
    if (n == tp_sent_n && !memcmp(xy, tp_sent, sizeof(float) * 3 * (size_t)n)) return;
    droid_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = DROID_EVENT_TOUCH;
    e.axes[0] = (float)n;
    for (int f = 0; f < n; f++)
        for (int k = 0; k < 3; k++) e.axes[1 + f * 3 + k] = xy[f][k];
    memcpy(tp_sent, xy, sizeof(tp_sent));
    tp_sent_n = n;
    sink(&e);
}

int droid_quirks; // QUIRK_* (android_shim.h): set from the APK before its libraries load

#ifdef DROID_PC
void pc_input_poll(void (*sink)(const droid_input_event *)); // pc/pc_input.c: the PC's mouse and keys
#endif

// Options held (0.45 s) opens the Quick menu (quick_menu.c); a short press is the game's, sent as
// it's let go. Paused, the game gets nothing (its keys let go once); after the resume the buttons
// still down from the menu (its Cross) wait to be let go.
#define PAD_OPTIONS 0x8
static int options_state;  // 0, 1: down (not the game's yet), 2: the menu asked (until let go)
static double options_at;
static int options_tap;    // a short press: down now, up at the next poll
static uint32_t swallowed; // buttons held over a resume

static void pad_poll(input_sink sink) {
#ifdef DROID_PC
    pc_input_poll(sink);
#endif
    if (pad < 0) return;
    rumble_tick(); // a game's vibration (droid_pad_rumble) stops on time
    pad_sample samples[16];
    int n = 0;
    const pad_sample *p = NULL;
    if (!droid_game_paused) { // (paused: the menu reads the pad)
        n = scePadRead(pad, samples, 16);
        if (n <= 0 || n > 16) return;
        p = &samples[0];
        for (int i = 1; i < n; i++)
            if (samples[i].timestamp_us > p->timestamp_us) p = &samples[i];
    }
    int live = p && p->connected && !(p->buttons & PAD_INTERCEPTED);
    uint32_t buttons = live ? p->buttons : 0;
    if (live) { // the motion: the accelerometer (g) at offset 28, the gyroscope (rad/s) at 40, turned into a
        // phone's frame (landscape, screen towards you: x right, y up the screen, z out of it); the
        // DualSense's: x right, y up out of its top, z towards you
        float a[3], g[3];
        memcpy(a, p->reserved_to_connected + 18, sizeof(a));
        memcpy(g, p->reserved_to_connected + 30, sizeof(g));
        const float G = 9.80665f;
        droid_motion_sample m = { { a[0] * G, -a[2] * G, a[1] * G }, { g[0], -g[2], g[1] }, (int64_t)(pad_now_s() * 1e9) };
        droid_motion_put(&m);
        static int told;
        if (told < 3 && (a[0] != 0 || a[1] != 0 || a[2] != 0)) {
            told++;
            say(0, "pad: motion sample: accel %.3f %.3f %.3f g, gyro %.3f %.3f %.3f rad/s", a[0], a[1], a[2], g[0], g[1], g[2]);
        }
    }
    if (quick_menu_resumed()) swallowed = buttons;
    swallowed &= buttons;
    buttons &= ~swallowed;
    if (options_tap) { // the short press's up
        options_tap = 0;
        buttons &= ~PAD_OPTIONS;
    } else if (buttons & PAD_OPTIONS) {
        double t = pad_now_s();
        if (!options_state) options_state = 1, options_at = t;
        if (options_state == 1 && t - options_at >= 0.45) {
            options_state = 2;
            quick_menu_request();
            droid_log("pad: Options held: the Quick menu");
        }
        buttons &= ~PAD_OPTIONS;
    } else {
        if (options_state == 1) { // let go in time: the game's press
            buttons |= PAD_OPTIONS;
            options_tap = 1;
        }
        options_state = 0;
    }

    droid_input_event e;
    uint32_t changed = buttons ^ pad_prev;
    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        if (!(changed & key_map[i].ps5)) continue;
        memset(&e, 0, sizeof(e));
        e.type = AINPUT_EVENT_TYPE_KEY;
        e.source = AINPUT_SOURCE_GAMEPAD;
        e.action = (buttons & key_map[i].ps5) ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
        e.keycode = key_map[i].keycode;
        sink(&e);
    }
    pad_prev = buttons;

    uint8_t now[6] = { 128, 128, 128, 128, 0, 0 };
    if (live) {
        now[0] = p->lx; now[1] = p->ly; now[2] = p->rx;
        now[3] = p->ry; now[4] = p->l2; now[5] = p->r2;
    }
    if (memcmp(now, prev_axes, sizeof(now)) != 0) {
        memcpy(prev_axes, now, sizeof(now));
        memset(&e, 0, sizeof(e));
        e.type = AINPUT_EVENT_TYPE_MOTION;
        e.source = AINPUT_SOURCE_JOYSTICK;
        e.action = AMOTION_EVENT_ACTION_MOVE;
        e.axes[0] = axis(now[0]);     // AXIS_X
        e.axes[1] = axis(now[1]);     // AXIS_Y
        e.axes[11] = axis(now[2]);    // AXIS_Z
        e.axes[14] = axis(now[3]);    // AXIS_RZ
        e.axes[17] = now[4] / 255.0f; // AXIS_LTRIGGER
        e.axes[18] = now[5] / 255.0f; // AXIS_RTRIGGER
        e.axes[23] = e.axes[17];      // AXIS_BRAKE
        e.axes[22] = e.axes[18];      // AXIS_GAS
        sink(&e);
    }

    // touchpad: ScePadData's touch data at offset 52: the count, then 8 bytes a finger (x u16,
    // y u16, id u8) from byte 8: 24 bytes (the second finger was read past a 16-byte copy before)
    if (pad_touch_events) {
        uint8_t t[24] = { 0 };
        if (live) memcpy(t, p->reserved_to_connected + 42, 24);
        const int n = live && t[0] <= 2 ? t[0] : 0;
        float xy[2][3] = { { 0 } };
        for (int f = 0; f < n; f++) {
            const uint8_t *fp = t + 8 + f * 8;
            xy[f][0] = (fp[0] | fp[1] << 8) / 1919.0f;
            xy[f][1] = (fp[2] | fp[3] << 8) / 1079.0f;
            xy[f][2] = fp[4];
        }
        const int click = (buttons & 0x100000) != 0;
        const double now = pad_now_s();
        if (n == 0) {
            if (tp_mode == 1) tp_send(sink, tp_land_n, tp_land); // a quick touch: down there, then up (a tap)
            tp_mode = 0;
            tp_send(sink, 0, xy);
        } else if (tp_mode == 0) {
            tp_mode = click ? 3 : 1;
            tp_since = now;
            memcpy(tp_land, xy, sizeof(tp_land));
            tp_land_n = n;
        } else if (tp_mode == 1) {
            const float dx = xy[0][0] - tp_land[0][0], dy = xy[0][1] - tp_land[0][1];
            if (click) {
                tp_mode = 3; // the cursor's tap (keycode 109), no touch
            } else if (now - tp_since >= TP_WAIT || dx * dx + dy * dy > 0.02f * 0.02f || n != tp_land_n) {
                tp_mode = 2;
                tp_send(sink, tp_land_n, tp_land);
                tp_send(sink, n, xy);
            }
        } else if (tp_mode == 2) {
            tp_send(sink, n, xy);
        }
    }
}

// ---- the test game (phases 1-3) ---------------------------------------------

typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t frame;
    uint64_t (*tile_offset)(uint32_t x, uint32_t y);
} droid_surface;

static int32_t (*test_on_input)(const droid_input_event *);

static void test_sink(const droid_input_event *e) {
    if (test_on_input) test_on_input(e);
}

static void run_test_game(so_module *mod) {
    typedef void (*draw_fn)(droid_surface *);
    draw_fn draw = (draw_fn)so_sym(mod, "droid_draw_frame");
    if (!draw) {
        say(1, "Android: droid_draw_frame not found");
        halt();
    }
    typedef int (*init_fn)(void);
    init_fn game_init = (init_fn)so_sym(mod, "game_init");
    test_on_input = (int32_t(*)(const droid_input_event *))so_sym(mod, "game_on_input");
    int audio = game_init ? game_init() : -1;
    say(0, "game_init (AAudio) = %d, game_on_input %p", audio, (void *)test_on_input);

    // Java: as Android does, JNI_OnLoad first, then the Activity's natives.
    typedef jint (*onload_fn)(JavaVM *, void *);
    onload_fn onload = (onload_fn)so_sym(mod, "JNI_OnLoad");
    jint jni_version = onload ? onload(&jvm, NULL) : -1;
    typedef jint (*native_init_fn)(JNIEnv *, jclass, jobject);
    native_init_fn native_init = (native_init_fn)jni_find_native("nativeInit");
    // On the heap: games DeleteGlobalRef their Activity, which FalsoJNI frees.
    jobject activity = (jobject)strdup("android/app/Activity");
    jint passed = -1;
    if (native_init) {
        jclass game_class = jni->FindClass(&jni, "com/ps5/android/Game");
        passed = native_init(&jni, game_class, activity);
    }
    say(0, "JNI_OnLoad = %#x, nativeInit %p, tests passed mask %#x", jni_version,
        (void *)native_init, passed);
    pad_open();
    say(1, "Android: game running. Sticks, buttons, hold Cross for sound");

    droid_surface surf = { .width = DISPLAY_WIDTH, .height = DISPLAY_HEIGHT,
                           .tile_offset = display_tile_offset };
    for (uint32_t frame = 0;; frame++) {
        int cur = frame & 1;
        surf.pixels = display_buffer(cur);
        surf.frame = frame;
        pad_poll(test_sink);
        draw(&surf);
        display_flip(cur, (int64_t)frame);
        if (frame == 120) say(0, "120 frames drawn");
    }
}

// ---- ARM64 code through the JIT (phase 5a check) ---------------------------------

#define ARM_TEST_LIB APP0 "/lib/libtest_arm64.so"

static void arm_self_test(void) {
    struct stat st;
    if (stat(ARM_TEST_LIB, &st) != 0) return;
    so_module mod;
    int rc = so_load(ARM_TEST_LIB, &mod);
    if (rc != 0 || !mod.arm64) {
        say(1, "Android: ARM64 test lib load fail rc=%d", rc);
        return;
    }
    int ok = 1;
    for (int i = 0; i < mod.ctor_count && ok; i++) arm_call(mod.ctors[i], NULL, 0, &ok);
    void *hello = so_sym(&mod, "droid_hello");
    void *version = so_sym(&mod, "droid_version");
    if (!hello || !version) {
        say(1, "Android: ARM64 test symbols not found");
        return;
    }
    uint64_t args[2] = { 40, 2 };
    uint64_t t0 = __builtin_ia32_rdtsc();
    uint64_t sum = arm_call((uint64_t)hello, args, 2, &ok);
    uint64_t t1 = __builtin_ia32_rdtsc();
    int ok2 = 0;
    uint64_t text = arm_call((uint64_t)version, NULL, 0, &ok2);
    say(0, "arm: droid_hello(40,2) = %d ok=%d (%llu cycles incl. first translation), "
        "droid_version ok=%d: %s", (int)sum, ok, (unsigned long long)(t1 - t0), ok2,
        ok2 && text ? (const char *)text : "(none)");

    // ARM64 code calling PS5 functions through its imports (thunks).
    void *imports = so_sym(&mod, "droid_arm_imports");
    if (!imports) {
        say(1, "Android: droid_arm_imports not found");
        return;
    }
    int ok3 = 0;
    uint64_t t2 = __builtin_ia32_rdtsc();
    uint32_t mask = (uint32_t)arm_call((uint64_t)imports, NULL, 0, &ok3);
    uint64_t t3 = __builtin_ia32_rdtsc();
    int passed = __builtin_popcount(mask & 0x3ff);
    say(0, "arm: imports test ok=%d mask %#x (%d/10 passed, %llu cycles), %d thunks", ok3, mask,
        passed, (unsigned long long)(t3 - t2), arm_thunk_total());
    say(1, "Android: ARM64 calls PS5 code: %d/10 tests passed (mask %#x)", passed, mask);

    // PS5 code calling ARM code back, and ARM threads.
    void *callbacks = so_sym(&mod, "droid_arm_callbacks");
    if (!callbacks) {
        say(1, "Android: droid_arm_callbacks not found");
        return;
    }
    int ok4 = 0;
    uint64_t t4 = __builtin_ia32_rdtsc();
    uint32_t cmask = (uint32_t)arm_call((uint64_t)callbacks, NULL, 0, &ok4);
    uint64_t t5 = __builtin_ia32_rdtsc();
    int cpassed = __builtin_popcount(cmask & 0x1ff);
    say(0, "arm: callbacks test ok=%d mask %#x (%d/9 passed, %llu cycles)", ok4, cmask, cpassed,
        (unsigned long long)(t5 - t4));
    say(1, "Android: ARM64 threads + callbacks: %d/9 tests passed (mask %#x)", cpassed, cmask);
}

// ---- a NativeActivity game -----------------------------------------------------

// Java GameActivity.onResume calls nativeResumeMainInit: it sets the flag AndroidMain waits
// for (0x12dd5bf8) and then waits for the engine to be ready. We run no Java, so call it here.
static void *native_resume_thread(void *arg) {
    usleep(100000);
    void *fn = arm_callback((uint64_t)(uintptr_t)arg);
    ((void (*)(void *, void *))fn)(arm_jni_env(), NULL);
    return NULL;
}

static void crash_setup(void);
// Drivers and engines install their own SIGSEGV handlers as they start; put ours back so a
// crash still says which ARM library and host call it was in.
static void *crash_guard(void *arg) {
    (void)arg;
    for (;;) { sceKernelUsleep(50000); crash_setup(); }
    return NULL;
}

static void *native_watchdog(void *arg) {
    (void)arg;
    for (int i = 1;; i++) {
        sleep(5);
        if (i == 1 || droid_tool("ThreadScope")) {
            droid_log("native: %d s in: what every thread is doing:", i * 5);
            arm_threads_report();
        }
#ifdef DROID_PC
        if (i == 1 && droid_tool("ThreadScope")) {
            void arm_host_backtraces(void); // arm_cpu.cpp: where each host thread is, in our code
            arm_host_backtraces();
        }
#endif
    }
    return NULL;
}

static char base_path[256], package[128];
// Unity games on OpenGL ES go on ANGLE (the setting Auto) only when tried there: every other one
// stays on ps5-opengl, as it worked (ANGLE for every Unity game broke Shadow Fight 4)
static int unity_kept_on_gl(const char *pkg) {
    // (the package itself: decided before java_package is set). Sonic Dash: ~9000 draws a second on
    // ps5-opengl at 45 us each held it at 40 fps with stutters, 12 ms of its own work a frame
    return strncmp(pkg, "com.imangi.", 11) != 0 && strcmp(pkg, "com.sega.sonicdash") != 0 && strcmp(pkg, "com.sega.sonicboomandroid") != 0;
}
static long apk_vcode;
static char apk_vname[64];
const char *droid_version_name = apk_vname; // the APK's versionName (gameactivity_host.c)
static char droid_obb_file[400];

static void run_native_activity(so_module *mod) {
    void *on_create = so_sym(mod, "ANativeActivity_onCreate");
    if (!on_create) {
        say(1, "Android: ANativeActivity_onCreate not found");
        halt();
    }
    mkdir(APP0 "/data", 0777);
    jobject clazz = (jobject)strdup("android/app/NativeActivity");
    void *vm = mod->arm64 ? arm_java_vm() : (void *)&jvm;
    void *env = mod->arm64 ? arm_jni_env() : (void *)&jni;
    // Its Java class may load the library with System.loadLibrary (UE4's GameActivity
    // does), which calls JNI_OnLoad first: the engine keeps the JavaVM from there.
    void *onload = so_sym(mod, "JNI_OnLoad");
    if (onload && (droid_quirks & QUIRK_UNREAL)) { // (Unreal only: other NativeActivity games as before)
        typedef jint (*onload_fn)(void *, void *);
        void *fn = mod->arm64 ? arm_callback((uint64_t)onload) : onload;
        jint v = ((onload_fn)fn)(vm, NULL);
        say(0, "NativeActivity: JNI_OnLoad = %#x", v);
    }
    // An ARM64 game: its onCreate runs in the JIT and gets the ARM JNI tables.
    void *create = mod->arm64 ? arm_callback((uint64_t)on_create) : on_create;
    if (activity_start(create, vm, env, clazz) != 0) {
        say(1, "Android: activity start failed");
        halt();
    }
    pad_open();
    say(1, "Android: game started");
    // What GameActivity.java does after super.onCreate (we run no Java): the paths, package,
    // version and OBB file the engine reads before it resumes.
    if (mod->arm64) {
        typedef void (*jfn)(void *, void *, void *, void *, void *, void *, void *, void *);
        void *env = arm_jni_env(), *act = (void *)strdup("com/epicgames/ue4/GameActivity");
        char files[300], ext[300];
        snprintf(files, sizeof(files), "%s/files", game_dir);
        snprintf(ext, sizeof(ext), "%s/files", game_dir);
        mkdir(files, 0777);
        jobject j_files = java_new_string(files), j_ext = java_new_string(ext);
        jobject j_apk = java_new_string(base_path), j_obb = java_new_string(droid_obb_file);
        jobject j_empty = java_new_string("");
        jobject j_pkg = java_new_string(package), j_name = java_new_string("Client");
        jobject j_ver = java_new_string(apk_vname);
        jobject j_rel = java_new_string("11"), j_man = java_new_string("Sony");
        jobject j_model = java_new_string("PlayStation 5"), j_disp = java_new_string("RP1A");
        jobject j_loc = java_new_string("en_US"), j_lang = java_new_string("en"), j_country = java_new_string("US");
        struct { const char *name; jfn fn; } table[] = {
            { "Java_com_epicgames_ue4_GameActivity_nativeSetObbFilePaths", NULL },
            { "Java_com_epicgames_ue4_GameActivity_nativeSetGlobalActivity", NULL },
            { "Java_com_epicgames_ue4_GameActivity_nativeSetWindowInfo", NULL },
            { "Java_com_epicgames_ue4_GameActivity_nativeSetAndroidStartupState", NULL },
            { "Java_com_epicgames_ue4_GameActivity_nativeSetAndroidVersionInformation", NULL },
            { "Java_com_epicgames_ue4_GameActivity_nativeSetAndroidExVersionInformation", NULL },
            { "Java_com_epicgames_ue4_GameActivity_nativeSetObbInfo", NULL },
        };
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            void *p = so_sym(mod, table[i].name);
            table[i].fn = p ? (jfn)arm_callback((uint64_t)p) : NULL;
        }
        jfn f;
        if ((f = table[0].fn)) f(env, act, j_obb, j_empty, j_empty, j_empty, NULL, NULL);
        if ((f = table[1].fn)) f(env, act, (void *)(uintptr_t)1, (void *)(uintptr_t)1, j_files, j_ext,
                                 (void *)(uintptr_t)0, j_apk);
        // (bIsPortrait 0: the TV is landscape; 1 made UE draw 1080x1920, its left square seen)
        if ((f = table[2].fn)) f(env, act, (void *)(uintptr_t)0, (void *)(uintptr_t)0, NULL, NULL, NULL, NULL);
        if ((f = table[3].fn)) f(env, act, (void *)(uintptr_t)0, NULL, NULL, NULL, NULL, NULL);
        if ((f = table[4].fn)) f(env, act, j_rel, (void *)(uintptr_t)30, j_man, j_model, j_disp, j_loc);
        if ((f = table[5].fn)) f(env, act, j_lang, j_country, NULL, NULL, NULL, NULL);
        if ((f = table[6].fn)) f(env, act, j_name, j_pkg, (void *)(uintptr_t)apk_vcode, (void *)(uintptr_t)0, j_ver, NULL);
        say(0, "NativeActivity: GameActivity natives set (obb %s, pkg %s, code %ld)", droid_obb_file,
            package, apk_vcode);
    }
    void *native_resume = so_sym(mod, "Java_com_epicgames_ue4_GameActivity_nativeResumeMainInit");
    if (native_resume && mod->arm64) {
        pthread_t rt;
        if (pthread_create(&rt, NULL, native_resume_thread, native_resume) == 0) pthread_detach(rt);
        say(0, "NativeActivity: nativeResumeMainInit queued");
    }
    pthread_t wd;
    if ((droid_quirks & QUIRK_UNREAL) && pthread_create(&wd, NULL, native_watchdog, NULL) == 0) pthread_detach(wd);
    // The game draws and presents on its own thread; this one feeds input.
    for (;;) {
        pad_poll(activity_push_input);
        sceKernelUsleep(8000);
    }
}

// ---- a real APK, or a split one (.xapk / .apks / .apkm): the app's game file ------------
// Its files go in the game's own folder (game_settings.c): apk/ (a bundle's APKs) and
// apk-lib/ (its libraries).

#define APK_PATH APP0 "/game.apk"
#define XAPK_PATH APP0 "/game.xapk"
#define APKS_PATH APP0 "/game.apks"
#define APK_MAX 24
static char apk_dir[300], apk_libs[300];

static int lib_extract(const char *name, const char *dest) {
    char entry[256];
    snprintf(entry, sizeof(entry), "lib/arm64-v8a/%s", name);
    return apk_extract(entry, dest);
}

static void count_entry(const char *name, void *ctx) {
    (void)name;
    (*(int *)ctx)++;
}

static int has_entry(const char *name) {
    int n = 0;
    apk_each(name, count_entry, &n);
    return n > 0;
}

typedef struct {
    char names[160][128]; // a bundle may hold 60+ APKs (a language pack each)
    int n;
} name_list;
#define LIST_MAX(l) ((int)(sizeof((l)->names) / sizeof((l)->names[0])))

static void collect_inner_apk(const char *name, void *ctx) {
    name_list *l = ctx;
    size_t len = strlen(name);
    if (strchr(name, '/') || len < 5 || strcmp(name + len - 4, ".apk") != 0) return;
    if (l->n < LIST_MAX(l)) snprintf(l->names[l->n++], sizeof(l->names[0]), "%s", name);
}

static void collect_lib(const char *name, void *ctx) {
    name_list *l = ctx;
    const char *base = name + strlen("lib/arm64-v8a/");
    size_t len = strlen(base);
    if (strchr(base, '/') || len < 4 || strcmp(base + len - 3, ".so") != 0) return;
    if (l->n < LIST_MAX(l)) snprintf(l->names[l->n++], sizeof(l->names[0]), "%s", base);
}

// A split we leave out: another language's texts (config.nl, split_config.de...; English
// stays) or another CPU's libraries (x86, armeabi). Geometry Dash's bundle has dozens.
static int unneeded_split(const char *name) {
    const char *c = strstr(name, "config.");
    if (!c) return 0;
    c += 7;
    char part[32];
    size_t n = strcspn(c, ".");
    if (n == 0 || n >= sizeof(part) || strcmp(c + n, ".apk")) return 0;
    memcpy(part, c, n);
    part[n] = 0;
    if (!strcmp(part, "x86") || !strcmp(part, "x86_64") || !strcmp(part, "armeabi_v7a") || !strcmp(part, "armeabi"))
        return 1;
    // a language: "nl", "fil", "pt_BR" (letters, an optional _region); not "en", not a density
    int lang = n == 2 || n == 3 || (n == 5 && part[2] == '_');
    for (size_t i = 0; i < n && lang; i++)
        if (!(i == 2 && part[i] == '_') && !isalpha((unsigned char)part[i])) lang = 0;
    if (!strcmp(part, "en") || !strncmp(part, "en_", 3)) return 0;
    return lang;
}

static int is_split_name(const char *name) {
    // config.* / split_*: the device's pieces; *asset_pack*: Play asset packs (Hay Day's
    // install_time_asset_pack.apk was taken for the game)
    return strncmp(name, "config.", 7) == 0 || strncmp(name, "split_", 6) == 0 || strstr(name, "asset_pack") ||
           strstr(name, "assetpack") || strstr(name, "asset-pack");
}

static int ends_with(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && strcasecmp(s + a - b, end) == 0;
}

// the game's engine (its folder's notes)
static void note_engine(const char *engine) {
    char p[300];
    FILE *f = fopen(game_path(p, sizeof(p), "engine.txt"), "w");
    if (f) {
        fprintf(f, "%s\n", engine);
        fclose(f);
    }
}

// The game's files made ready: its bundle unpacked (once), its archives open, its
// libraries out, its APKs read ahead into the file cache. This runs on a
// thread during its launch animation (so the game starts sooner); run_apk() after.
// Errors are kept for run_apk to show: this may run on that thread.
static char prep_file[256], prep_error[160];
// What the load is doing, when it takes long
const char *volatile load_note;

// The game's package as read from its APK (com.kiloo.subwaysurf), else ours:
// games keep it in their saves' names and send it to their servers
static char known_package[128];

static void set_known_package(const lib_game *g) {
    const char *id = g ? g->id : "";
    size_t n = strlen(id);
    int ok = strchr(id, '.') && !(n > 4 && (!strcasecmp(id + n - 4, ".apk") || !strcasecmp(id + n - 5, ".xapk")));
    snprintf(known_package, sizeof(known_package), "%s", ok ? id : "");
}
static char split_paths[APK_MAX][256];
static const char *splits[APK_MAX];
static int split_count, prep_state; // 0 not yet, 1 done, -1 failed

static double secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int prepare_apk(const char *file) {
    const char *bundle = ends_with(file, ".xapk") || ends_with(file, ".apks") || ends_with(file, ".apkm") ? file : NULL;
    snprintf(prep_file, sizeof(prep_file), "%s", file);
    snprintf(package, sizeof(package), "%s", known_package[0] ? known_package : "com.ps5.game");
    snprintf(base_path, sizeof(base_path), "%s", file);
    split_count = 0;
    prep_state = -1;
    game_path(apk_dir, sizeof(apk_dir), "apk");
    game_path(apk_libs, sizeof(apk_libs), "apk-lib");
    mkdir(apk_libs, 0777);
    double t0 = secs();
    if (bundle) {
        // a zip of APKs: base + config.* splits, unpacked once
        if (apk_open(bundle) != 0) {
            snprintf(prep_error, sizeof(prep_error), "Android: %s open fail", bundle);
            return -1;
        }
        mkdir(apk_dir, 0777);
        if (!known_package[0]) { // not read yet: the bundle's manifest says it
            size_t mn = 0;
            char *m = (char *)apk_load("manifest.json", &mn);
            const char *key = "\"package_name\"";
            if (!m) { // .apkm (APKMirror): info.json, "pname"
                m = (char *)apk_load("info.json", &mn);
                key = "\"pname\"";
            }
            const char *k = m ? strstr(m, key) : NULL;
            if (k && (k = strchr(k + strlen(key), ':')) && (k = strchr(k, '"'))) {
                size_t n = strcspn(k + 1, "\"");
                if (n < sizeof(known_package)) snprintf(known_package, sizeof(known_package), "%.*s", (int)n, k + 1);
            }
            free(m);
        }
        name_list inner = { .n = 0 };
        apk_each("", collect_inner_apk, &inner);
        // the base: <package>.apk when the bundle has it (its manifest's package); everything
        // else a split (Among Us' UnityDataAssetPack.apk, Wuthering Waves' obbassets.apk were
        // taken for the game: "path not found")
        int base_at = -1;
        if (known_package[0])
            for (int i = 0; i < inner.n; i++) {
                char want[160];
                snprintf(want, sizeof(want), "%s.apk", known_package);
                if (!strcmp(inner.names[i], want)) base_at = i;
            }
        for (int i = 0; i < inner.n && base_at < 0; i++) // .apks / .apkm: base.apk
            if (!strcmp(inner.names[i], "base.apk")) base_at = i;
        for (int pass = 0; pass < 2; pass++) { // base first, then the splits
            for (int i = 0; i < inner.n; i++) {
                int split = base_at >= 0 ? i != base_at : is_split_name(inner.names[i]);
                if (split != pass) continue;
                if (pass == 1 && unneeded_split(inner.names[i])) continue; // another language / CPU
                if (pass == 1 && split_count >= APK_MAX) {
                    say(0, "apk: %s left out (more than %d splits)", inner.names[i], APK_MAX);
                    continue;
                }
                char path[256];
                snprintf(path, sizeof(path), "%s/%s", apk_dir, inner.names[i]);
                if (apk_extract(inner.names[i], path) != 0 || apk_open(path) != 0) {
                    if (pass == 1) { // a split we can't open: without it (logged), not a failed start
                        say(0, "apk: %s from the bundle: can't open, left out", inner.names[i]);
                        continue;
                    }
                    snprintf(prep_error, sizeof(prep_error), "Android: %s from the bundle fail", inner.names[i]);
                    return -1;
                }
                if (pass == 0) {
                    snprintf(base_path, sizeof(base_path), "%s", path);
                    if (strcmp(inner.names[i], "base.apk") != 0) { // xapk: <package>.apk
                        snprintf(package, sizeof(package), "%s", inner.names[i]);
                        package[strlen(package) - 4] = 0;
                    }
                } else if (split_count < APK_MAX) {
                    snprintf(split_paths[split_count], sizeof(split_paths[0]), "%s", path);
                    splits[split_count] = split_paths[split_count];
                    split_count++;
                }
            }
        }
    } else if (apk_open(file) != 0) {
        snprintf(prep_error, sizeof(prep_error), "Android: %s open fail", file);
        return -1;
    }
    // splitSourceDirs as a phone lists them: the device's config.* splits first, the asset
    // splits after (Genshin Impact looked for its AssetBundles in the last one: config.arm64_v8a)
    for (int i = 1; i < split_count; i++) {
        const char *b = strrchr(splits[i], '/');
        if (strncmp(b ? b + 1 : splits[i], "config.", 7)) continue;
        for (int k = i; k > 0; k--) {
            const char *p = strrchr(splits[k - 1], '/');
            if (!strncmp(p ? p + 1 : splits[k - 1], "config.", 7)) break;
            const char *t = splits[k];
            splits[k] = splits[k - 1];
            splits[k - 1] = t;
        }
    }
    // the libraries out now (arm_module_load finds them there, same size: not again)
    name_list libs = { .n = 0 };
    apk_each("lib/arm64-v8a/", collect_lib, &libs);
    // cocos2d-x games draw with OpenGL ES: on ANGLE when the renderer setting is Auto (decided
    // here, before their libraries load and look their GL functions up)
    extern int droid_angle_auto; // gles_bridge.c
    for (int i = 0; i < libs.n; i++)
        if (!strncmp(libs.names[i], "libcocos2d", 10) || !strcmp(libs.names[i], "libmortargame.so") ||
            (!strcmp(libs.names[i], "libunity.so") && !unity_kept_on_gl(package)) ||
            (!strcmp(libs.names[i], "libcandycrushsaga.so") && !strcmp(package, "com.king.candycrushsaga")))
            droid_angle_auto = 1; // (and Halfbrick's Mortar: Fruit Ninja, Jetpack Joyride; and Unity on
                                  // OpenGL ES - a Unity game on Vulkan stays on Vulkan: this is GLES only;
                                  // and Candy Crush's GameActivity: ps5-opengl refused its EGL config)
    for (int i = 0; i < libs.n; i++) {
        char path[400];
        snprintf(path, sizeof(path), "%s/%s", apk_libs, libs.names[i]);
        lib_extract(libs.names[i], path);
    }
    // the signing certificate, as PackageManager reports it (before a Vulkan copy: that one
    // isn't signed)
    {
        extern char droid_apk_signature[64]; // java_layer.c
        if (apk_signature(base_path, droid_apk_signature, sizeof(droid_apk_signature)) == 0)
            say(0, "apk: signed, certificate SHA-1 %s", droid_apk_signature);
        else
            say(0, "apk: no v2/v3 signature found");
    }
    // Vulkan first, for every Unity game that has it (the game's settings may say GLES)
    // (the copy is kept out of the game's folder: that one is named after the game's file,
    // "....apk", and Unity takes a path up to its first ".apk/" for the APK)
    if (!renderer_gles() && has_entry("lib/arm64-v8a/libunity.so")) {
        char copy[300], name[200];
        const char *slash = strrchr(game_dir, '/');
        snprintf(name, sizeof(name), "%s", slash ? slash + 1 : game_dir);
        char *dot = strrchr(name, '.'); // the game file's extension off: .apk / .xapk / .apks
        if (dot && (!strcasecmp(dot, ".apk") || !strcasecmp(dot, ".xapk") || !strcasecmp(dot, ".apks") || !strcasecmp(dot, ".apkm"))) *dot = 0;
        snprintf(copy, sizeof(copy), APP0 "/data/vulkan-first/%s.apk", name);
        // usually: changed in memory as Unity unpacks it (no copy); a stored list: the copy
        if (unity_vulkan_plan(base_path) == 2) {
            mkdir(APP0 "/data/vulkan-first", 0777);
            load_note = "First start: setting up Vulkan (once)";
            if (unity_vulkan_first(base_path, copy) == 0) snprintf(base_path, sizeof(base_path), "%s", copy);
            load_note = NULL;
        } else {
            unlink(copy); // an older build's copy: not needed any more
        }
    }
#ifdef DROID_PC
    if (getenv("DROID_BASE_OVERRIDE")) snprintf(base_path, sizeof(base_path), "%s", getenv("DROID_BASE_OVERRIDE")); // tests
#endif
    droid_readahead(base_path);
    for (int i = 0; i < split_count; i++) droid_readahead(splits[i]);
    prep_state = 1;
    say(0, "apk: ready in %.2f s (%d libraries)", secs() - t0, libs.n);
    droid_startup("APK ready (%d libraries)", libs.n);
    return 0;
}


// Vulkan first: the device says it has Vulkan (PackageManager.hasSystemFeature, what Unity
// asks before it picks its graphics API), so a game built with both takes Vulkan, and one
// without keeps OpenGL ES. The game's settings may say GLES (then: no Vulkan here).
int droid_vulkan_wanted = 1; // java_layer.c answers hasSystemFeature with it

static int manifest_has(const char *what) {
    size_t n = 0;
    unsigned char *m = apk_load("AndroidManifest.xml", &n);
    if (!m) return 0;
    size_t w = strlen(what);
    int hit = 0;
    for (size_t i = 0; i + w * 2 <= n && !hit; i++) { // the manifest's strings: UTF-16 (or UTF-8)
        size_t k = 0;
        while (k < w && m[i + k * 2] == (unsigned char)what[k] && m[i + k * 2 + 1] == 0) k++;
        if (k == w) hit = 1;
        if (!hit && i + w <= n && !memcmp(m + i, what, w)) hit = 1;
    }
    free(m);
    return hit;
}

// The phone view: a portrait-only game gets a phone's 1080x1920 screen, shown in the middle of the
// TV at its full height, black at the sides (as LDPlayer shows one). By hand, game by game (the
// user: no setting): PHONE_VIEW_GAMES, each tested in it. What a game says about itself (its
// Unity settings, its manifest) is only logged, for the next game to add.
#define PHONE_VIEW_GAMES "com.imangi.templerun,com.sega.sonicdash,com.sega.sonicboomandroid," \
                         "com.king.candycrushsaga,com.kiloo.subwaysurf,com.imangi.harbormasterremix,com.zeptolab.ctr.ads"
void view_decide(const char *apk, const unsigned char *manifest, size_t manifest_len) {
    const int portrait = droid_game_in(PHONE_VIEW_GAMES);
    const int unity = unity_portrait_only(apk);
    long o = -1;
    char os[32];
    if (manifest && axml_attr(manifest, manifest_len, "screenOrientation", os, sizeof(os), &o) && o < 0) o = -1;
    const int says_portrait = unity == 1 || o == 1 || o == 7 || o == 9 || o == 12;
    droid_game_w = portrait ? 1080 : 1920;
    droid_game_h = portrait ? 1920 : 1080;
    void jni_set_screen(int w, int h); // jni_impl.c
    jni_set_screen(droid_game_w, droid_game_h);
    say(0, "view: %s: the game gets %dx%d (it says it's %s: its Unity settings %d, its manifest %ld)",
        portrait ? "phone (portrait, PHONE_VIEW_GAMES)" : "wide (TV)", droid_game_w, droid_game_h,
        says_portrait ? "portrait only" : "not portrait only", unity, o);
}

static void choose_renderer(void) {
    int vulkan = manifest_has("android.hardware.vulkan");
    droid_vulkan_wanted = !renderer_gles();
    say(0, "renderer: %s (the game %s Vulkan; setting %s)", droid_vulkan_wanted && vulkan ? "Vulkan" : "OpenGL ES",
        vulkan ? "has" : "doesn't declare", settings.renderer == RENDERER_ANGLE ? "ANGLE" : settings.renderer == RENDERER_GLES ? "GLES"
                                            : settings.renderer == RENDERER_VULKAN ? "Vulkan" : "auto");
}

// UE4 reads its content from the OBB: <obb dir>/main.<versionCode>.<package>.obb.
// An XAPK keeps it as assets/main.obb.png in the obbassets split (stored, 800 MB).
static void setup_obb(void) {
    char dir[300], dest[400];
    snprintf(dir, sizeof(dir), "%s/obb", game_dir);
    mkdir(dir, 0777);
    snprintf(dest, sizeof(dest), "%s/main.%ld.%s.obb", dir, apk_vcode, package);
    static const char *const names[] = { "assets/main.obb.png", "assets/main.obb" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int rc = apk_extract_stream(names[i], dest);
        if (rc == 0) {
            activity_set_obb(dir);
            snprintf(droid_obb_file, sizeof(droid_obb_file), "%s", dest);
            say(1, "obb: %s (ready)", dest);
            return;
        }
        if (rc != -1) say(1, "obb: %s failed (%d)", names[i], rc);
    }
    say(1, "obb: none in the APK (the game may ask for its data)");
}

static void run_apk(const char *file) {
    if (prep_state == 0 || strcmp(prep_file, file) != 0) prepare_apk(file); // no launcher: here
    if (prep_state != 1) {
        say(1, "%s", prep_error);
        halt();
    }
    droid_arm_game = 1;
    {
        extern const char *java_package; // jni_impl.c: known before any of the game's code runs
        java_package = strdup(package);  // (the per-game fixes ask for it: droid_game_in)
    }
    arm_modules_init(apk_libs, lib_extract);
    const char *activity_cls = has_entry("lib/arm64-v8a/libUE4.so") ? "com/epicgames/ue4/GameActivity"
                              : has_entry("lib/arm64-v8a/libUnreal.so") ? "com/epicgames/unreal/GameActivity"
                              : "com/unity3d/player/UnityPlayerActivity";
    jni_set_app(package, base_path, apk_libs, splits, split_count, strdup(activity_cls));
    say(0, "apk: package %s, base %s, %d split(s)", package, base_path, split_count);
    {
        size_t mn = 0;
        unsigned char *m = apk_load("AndroidManifest.xml", &mn);
        char vname[64] = "";
        long vcode = 0;
        if (m) {
            axml_attr(m, mn, "versionName", vname, sizeof(vname), NULL);
            axml_attr(m, mn, "versionCode", NULL, 0, &vcode);
        }
        view_decide(base_path, m, mn);
        free(m);
        jni_set_version(vname, vcode);
        apk_vcode = vcode;
        snprintf(apk_vname, sizeof(apk_vname), "%s", vname);
        say(0, "apk: version %s (code %ld)", vname[0] ? vname : "?", vcode);
        void frc_start(const char *version); // firebase_rc.c: Remote Config for the games on its list
        frc_start(vname);
    }

    choose_renderer();
    // the engines that get their own fixes (android_shim.h); the rest keep the old behaviour
    if (has_entry("lib/arm64-v8a/libyuanshen.so")) droid_quirks |= QUIRK_MIHOYO;
    if (has_entry("lib/arm64-v8a/libGame.so")) droid_quirks |= QUIRK_ROCKSTAR;
    if (has_entry("lib/arm64-v8a/libUE4.so") || has_entry("lib/arm64-v8a/libUnreal.so")) droid_quirks |= QUIRK_UNREAL;
    if (droid_quirks)
        say(0, "quirks: %s%s%s (their own fixes; every other game the old behaviour)",
            droid_quirks & QUIRK_MIHOYO ? "miHoYo " : "", droid_quirks & QUIRK_ROCKSTAR ? "Rockstar " : "",
            droid_quirks & QUIRK_UNREAL ? "Unreal" : "");
    if (droid_quirks & QUIRK_UNREAL) setup_obb(); // its OBB out of the XAPK (Unreal reads it as a file)
    if (settings.exact_atomics || has_entry("lib/arm64-v8a/libyuanshen.so")) {
        void arm_set_exact_atomics(int on);
        arm_set_exact_atomics(1);
        say(0, "arm: exact atomics (the exclusive monitor) for this game");
    }
    if (has_entry("lib/arm64-v8a/libunity.so")) {
        note_engine("Unity");
        pad_open();
        say(1, "Android: Unity game from the APK");
        droid_bench(base_path); // memory and file speed, in the log
        {
            void droid_fs_bench(void); // linux_abi.c: FsBench, file work in each writable place
            extern int droid_tool(const char *name);
            if (droid_tool("FsBench")) droid_fs_bench();
        }
        // IL2CPP's garbage collector pauses the other threads with signals to scan them:
        // delivered to ARM threads now (arm_cpu.cpp). DROID_GC_OFF=1: no collection (tests).
        if (getenv("DROID_GC_OFF")) {
            setenv("GC_DONT_GC", "1", 1);
            say(0, "unity: garbage collection off (DROID_GC_OFF)");
        }
        pad_touch_events = 1;
        void arm_warm_start(void); // arm_cpu.cpp: WarmStart (its threads wait for Unity's libraries)
        arm_warm_start();
        int rc = unity_run(apk_libs, pad_poll);
        say(1, "Android: Unity stopped (%d)", rc);
        halt();
    }
    if (has_entry("lib/arm64-v8a/libgodot_android.so")) {
        so_module *m = arm_module_load("libgodot_android.so");
        if (m && godot_detect(m)) {
            note_engine("Godot");
            pad_open();
            say(1, "Android: Godot game from the APK");
            godot_run(m, pad_poll);
        }
        say(1, "Android: Godot engine library failed");
        halt();
    }
    name_list libs = { .n = 0 };
    apk_each("lib/arm64-v8a/", collect_lib, &libs);
    for (int i = 0; i < libs.n; i++) {
        // Flutter's: libapp.so is its compiled Dart (data for libflutter, not code to run),
        // loaded only by Flutter's own engine (Hay Day crashed loading it here)
        if (!strcmp(libs.names[i], "libapp.so") || !strcmp(libs.names[i], "libflutter.so")) continue;
        arm_module_load(libs.names[i]);
    }
    void arm_warm_start(void); // arm_cpu.cpp: WarmStart - last launch's blocks translated ahead
    arm_warm_start();
    for (int i = 0; i < arm_module_count(); i++) {
        if (cocos_detect(arm_module(i))) { // cocos2d-x 2.x (Geometry Dash)
            note_engine("cocos2d-x");
            pad_open();
            say(1, "Android: cocos2d-x game (%s)", arm_module_path(i));
            pad_touch_events = 1; // the touchpad as its touchscreen
            cocos_run(arm_module(i), base_path, pad_poll);
            say(1, "Android: cocos2d-x game: its natives are missing");
            halt();
        }
    }
    for (int i = 0; i < arm_module_count(); i++) {
        if (mortar_detect(arm_module(i))) { // Halfbrick's Mortar (Fruit Ninja 2.x, Jetpack Joyride)
            note_engine("Mortar");
            pad_open();
            say(1, "Android: Mortar game (%s)", arm_module_path(i));
            pad_touch_events = 1; // the touchpad as its touchscreen
            mortar_run(arm_module(i), base_path, pad_poll);
            say(1, "Android: Mortar game: its natives are missing");
            halt();
        }
    }
    for (int i = 0; i < arm_module_count(); i++) {
        if (gtasa_detect(arm_module(i))) { // Rockstar's oswrapper (GTA San Andreas 2.11)
            note_engine("Rockstar");
            pad_open();
            say(1, "Android: Rockstar game (%s)", arm_module_path(i));
            pad_touch_events = 1; // the touchpad as its touchscreen (its menus are touch first)
            gtasa_run(arm_module(i), pad_poll);
            say(1, "Android: Rockstar game: its natives are missing");
            halt();
        }
    }
    int unity_is_merged(so_module *m);
    int unity_run_merged(so_module *m, const char *lib_dir, unity_poll_fn poll);
    for (int i = 0; i < arm_module_count(); i++) {
        if (unity_is_merged(arm_module(i))) { // Unity inside the game's own library (Genshin Impact)
            note_engine("Unity");
            pad_open();
            say(1, "Android: Unity game (its player in %s)", arm_module_path(i));
            pad_touch_events = 1;
            int rc = unity_run_merged(arm_module(i), apk_libs, pad_poll);
            say(1, "Android: Unity stopped (%d)", rc);
            halt();
        }
    }
    for (int i = 0; i < arm_module_count(); i++) {
        if (gameactivity_detect(arm_module(i))) { // Google's GameActivity (King's engine: Candy Crush Saga)
            note_engine("GameActivity");
            pad_open();
            say(1, "Android: GameActivity game (%s)", arm_module_path(i));
            pad_touch_events = 1;
            // a new engine: ANGLE on Auto (ps5-opengl refused its EGL config: eglCreateWindowSurface
            // EGL_BAD_CONFIG, a black screen)
            extern int droid_angle_auto;
            droid_angle_auto = 1;
            gameactivity_run(arm_module(i), pad_poll);
            say(1, "Android: GameActivity game: its start failed");
            halt();
        }
    }
    for (int i = 0; i < arm_module_count(); i++) {
        if (so_sym(arm_module(i), "ANativeActivity_onCreate")) {
            note_engine("Native");
            say(1, "Android: NativeActivity game from the APK");
            run_native_activity(arm_module(i));
        }
    }
    say(1, "Android: no game entry point in the APK libraries");
    // (a 32-bit only copy: no arm64 libraries) the app closes saying so: not a black screen
    quick_menu_relaunch("nolib");
    halt();
}

// ---- crash report: where a fault happened, in android.log -------------------------

#include <signal.h>
#include <ucontext.h>
#ifdef __linux__ // the PC test runner
#define UC_RIP(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
#define UC_RSP(uc) ((uc)->uc_mcontext.gregs[REG_RSP])
#else
#define UC_RIP(uc) ((uc)->uc_mcontext.mc_rip)
#define UC_RSP(uc) ((uc)->uc_mcontext.mc_rsp)
#endif

int main(void);
void thread_place_init(void);

void droid_trace_write(const char *line, int n); // arm_cpu.cpp: trace.txt, a write() a line
static void crash_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t rip = (uint64_t)UC_RIP(uc);
    { // first, to trace.txt with a bare write(): the log below takes locks the faulting thread may hold
        char l[160];
        int k = snprintf(l, sizeof(l), "CRASH signal %d at %p, rip %#llx (eboot+%#llx)\n", sig, info->si_addr,
                         (unsigned long long)rip, (unsigned long long)(rip - (uint64_t)(uintptr_t)&main));
        droid_trace_write(l, k);
        const char *imp = arm_last_import();
        k = snprintf(l, sizeof(l), "CRASH thread %#llx: ARM depth %d, last PS5 function from ARM: %.60s\n",
                     (unsigned long long)(uintptr_t)pthread_self(), arm_depth(), imp ? imp : "-");
        droid_trace_write(l, k);
        extern _Thread_local const void *droid_memmove_last[3]; // bionic_shim.c
        k = snprintf(l, sizeof(l), "CRASH last own memmove: dst %p src %p n %#llx\n", droid_memmove_last[0],
                     droid_memmove_last[1], (unsigned long long)(uintptr_t)droid_memmove_last[2]);
        droid_trace_write(l, k);
        { // this thread's last calls out of ARM code, with the ARM caller (library+offset)
            const char *tn;
            uint64_t tlr, t0, t1, t2, tr;
            for (int i = 0; i < 12 && arm_trace_get(i, &tn, &tlr, &t0, &t1, &t2, &tr); i++) {
                const char *mn = "?";
                so_module *m = arm_module_at(tlr, &mn, NULL);
                k = snprintf(l, sizeof(l), "CRASH call %2d: %.40s from %s+%#llx (%#llx, %#llx, %#llx) -> %#llx\n", i,
                             tn, mn, (unsigned long long)(m ? tlr - (uint64_t)m->base : tlr), (unsigned long long)t0,
                             (unsigned long long)t1, (unsigned long long)t2, (unsigned long long)tr);
                droid_trace_write(l, k);
            }
        }
        // return addresses on the stack: values inside our own code (eboot+...), the JIT's, or the
        // console's libraries (0x8000...); the callers of abort()
        const uint64_t *sp = (const uint64_t *)UC_RSP(uc);
        const uint64_t me = (uint64_t)(uintptr_t)&main;
        for (int i = 0, shown = 0; i < 512 && shown < 40; i++) {
            const uint64_t v = sp[i];
            const int ours = v > me && v - me < (96u << 20), sys = v >= 0x800000000ull && v < 0x900000000ull;
            if (!ours && !sys && !arm_in_jit(v)) continue;
            k = snprintf(l, sizeof(l), "CRASH  sp[%d] %#llx %s+%#llx\n", i, (unsigned long long)v,
                         ours ? "eboot" : sys ? "system" : "jit", (unsigned long long)(ours ? v - me : v));
            droid_trace_write(l, k);
            shown++;
        }
    }
    const char *where = arm_in_jit(rip) ? "JIT code (translated ARM)" : "PS5 code";
    const char *mod_name = NULL;
    if (arm_module_at(rip, &mod_name, NULL)) where = "ARM library memory (executed as x86!)";
    say(0, "CRASH signal %d at address %p: rip %#llx (eboot+%#llx, %s), rsp %#llx", sig,
        info->si_addr, (unsigned long long)rip,
        (unsigned long long)(rip - (uint64_t)(uintptr_t)&main), where,
        (unsigned long long)UC_RSP(uc));
    say(0, "CRASH thread: ARM depth %d, last PS5 function called from ARM: %s", arm_depth(),
        arm_last_import());
#ifdef DROID_PC
    { void *bt[48]; int n = backtrace(bt, 48); backtrace_symbols_fd(bt, n, 2); }
    { // the mapping the fault is in (anonymous memory: whose? JIT, a thunk pool, a driver's own code)
        FILE *mf = fopen("/proc/self/maps", "r");
        char ml[512];
        while (mf && fgets(ml, sizeof(ml), mf)) {
            unsigned long lo = 0, hi = 0;
            if (sscanf(ml, "%lx-%lx", &lo, &hi) == 2 && rip >= lo && rip < hi) {
                ml[strcspn(ml, "\n")] = 0;
                say(0, "CRASH rip in mapping: %s (offset %#lx)", ml, (unsigned long)(rip - lo));
            }
        }
        if (mf) fclose(mf);
    }
#endif
    const char *tname;
    uint64_t lr, a0, a1, a2, ret;
    for (int i = 0; arm_trace_get(i, &tname, &lr, &a0, &a1, &a2, &ret); i++) {
        const char *mname = "?";
        so_module *m = arm_module_at(lr, &mname, NULL);
        say(0, "CRASH call %2d: %-26s from %s+%#llx (%#llx, %#llx, %#llx) -> %#llx", i, tname, mname,
            (unsigned long long)(m ? lr - (uint64_t)m->base : lr), (unsigned long long)a0,
            (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)ret);
        // a string call: its text too (a game's own message just before abort(): Vector 2 built one
        // of 87 characters and showed it nowhere). Read only when a pipe write says it's readable.
        if (!strcmp(tname, "strlen") || !strcmp(tname, "strcpy") || !strcmp(tname, "strcat") ||
            !strcmp(tname, "strdup") || !strcmp(tname, "__strlen_chk")) {
            static int probe[2] = { -1, -1 };
            if (probe[0] < 0 && pipe(probe) != 0) probe[0] = probe[1] = -2;
            char text[200];
            const char *s = (const char *)(uintptr_t)a0;
            size_t k = 0;
            for (; probe[1] >= 0 && k + 1 < sizeof(text); k++) {
                if (write(probe[1], s + k, 1) != 1) break; // (EFAULT: not readable)
                char c;
                if (read(probe[0], &c, 1) != 1 || !c) break;
                text[k] = c >= 32 && c < 127 ? c : '.';
            }
            text[k] = 0;
            if (k) say(0, "CRASH call %2d text: \"%s\"", i, text);
        }
    }
    arm_crash_state();
    droid_log_managed_exceptions(); // an IL2CPP game: its C# exceptions on the stack
    uint64_t *sp = (uint64_t *)UC_RSP(uc);
    for (int i = 0; i < 16; i++) {
        uint64_t v = sp[i];
        if (v > 0x1000000) say(0, "CRASH stack[%d] %#llx (eboot+%#llx)%s", i, (unsigned long long)v,
                                (unsigned long long)(v - (uint64_t)(uintptr_t)&main),
                                arm_in_jit(v) ? " jit" : "");
    }
    log_flush_now();
    { // a game's crash: the app closes, saying so (not a frozen or black screen)
        char msg[300];
        snprintf(msg, sizeof(msg), "The game stopped: it crashed (signal %d in %s). android.log has the details.", sig,
                 mod_name ? mod_name : where);
        back_with_error(msg);
    }
    signal(sig, SIG_DFL);
}

static void crash_setup(void) {
    if (getenv("DROID_NO_CRASH_HANDLER")) return; // debugger/ASAN keeps its own
    static int said;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char alt[64 << 10];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
    if (!said++) say(0, "crash report on; main at %p", (void *)&main);
}

// Functions the console's libc lacks: the loader leaves their slots 0 and the first call
// jumps to address 0 (umask did). /app0/imports.txt (from the build): main's address, then
// each import's slot. Every empty slot, logged once at start.
static char missing_names[4096]; // check_imports: the empty slots' names, " a b c "
int droid_import_missing(const char *name) {
    char key[168];
    snprintf(key, sizeof(key), " %s ", name);
    return strstr(missing_names, key) != NULL;
}
static void check_imports(void) {
#ifndef DROID_PC
    strcpy(missing_names, " ");
    FILE *f = fopen(APP0 "/imports.txt", "r");
    if (!f) return;
    char name[160];
    unsigned long long off, main_off = 0;
    int missing = 0, total = 0;
    if (fscanf(f, "%llx %159s", &main_off, name) != 2 || strcmp(name, "main") != 0) {
        fclose(f);
        return;
    }
    uintptr_t base = (uintptr_t)&main - (uintptr_t)main_off;
    while (fscanf(f, "%llx %159s", &off, name) == 2) {
        total++;
        if (*(const uint64_t *)(base + (uintptr_t)off) == 0) {
            missing++;
            say(0, "imports: %s has no code on this console (slot empty)", name);
            if (strlen(missing_names) + strlen(name) + 2 < sizeof(missing_names)) {
                strcat(missing_names, name);
                strcat(missing_names, " ");
            }
        }
    }
    fclose(f);
    say(0, "imports: %d of %d without code", missing, total);
#endif
}

int arm_imports_known(const char *in_path); // arm_imports.c: the APK scanner's list

// exit() from anywhere (a game's libc exit, our own halt): which thread, in trace.txt
static void exit_note(void) {
    char l[96];
    int k = snprintf(l, sizeof(l), "EXIT called on thread %#llx\n", (unsigned long long)(uintptr_t)pthread_self());
    droid_trace_write(l, k);
}

// A one-game app (the "Subway Surfers" app): /app0/single_game.txt names its package, and that
// game starts straight away, no launcher. Its file goes in /app0/game.
// -1: not a one-game app; 0: *g is the game; 1: its file isn't there
int droid_single_game; // quick_menu.c: no library to go back to, the app closes
static int single_game_pick(lib_game *g) {
    FILE *f = fopen(APP0 "/single_game.txt", "r");
    if (!f) return -1;
    char pkg[128] = "";
    if (!fgets(pkg, sizeof(pkg), f)) pkg[0] = 0;
    fclose(f);
    pkg[strcspn(pkg, "\r\n \t")] = 0;
    droid_single_game = 1;
    static lib_game games[LIB_MAX];
    const int n = library_scan_in(APP0 "/game", games, LIB_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(games[i].id, pkg) || n == 1) { // (one file there: that one, whatever it says it is)
            *g = games[i];
            g->icon = NULL;
            if (strcmp(g->id, pkg)) say(0, "one-game app: /app0/game has %s, not %s: it runs", g->id, pkg);
            return 0;
        }
    say(0, "one-game app: %s isn't in /app0/game (%d games there)", pkg, n);
    say(1, "No game file: put the game in this app's game folder");
    return 1;
}

int main(void) {
    atexit(exit_note);
    const char *known = getenv("DROID_KNOWN_IMPORTS"); // tools/apk_scan.py: which imports we have
    if (known) return arm_imports_known(known);
    struct stat st;
    int apk_game = stat(APP0 "/game.apk", &st) == 0 || stat(APP0 "/game.xapk", &st) == 0 ||
                   stat(APP0 "/game.apks", &st) == 0 ||
                   stat(APP0 "/single_game.txt", &st) == 0;
    int native_game = apk_game || stat(GAME_LIB, &st) == 0;

    // A NativeActivity game draws through EGL (ps5-opengl), which opens the
    // screen itself; the test game draws into our own framebuffers.
    int busy_tries = 0;
    int video = native_game ? 0 : display_open(&busy_tries);

    // the log is kept across launches; past 64 MB it starts again (the last one kept as
    // android-old.log): a game retrying a failed call wrote 946 MB of it
    struct stat lst;
    if (stat(LOG_PATH, &lst) == 0 && lst.st_size > (64 << 20)) {
        unlink(APP0 "/android-old.log");
        rename(LOG_PATH, APP0 "/android-old.log");
    }
    log_file = fopen(LOG_PATH, "a");
    say(0, "==== launch, pid %d (Hydroid " HYDROID_VERSION ") ====", getpid());
    pthread_t log_srv;
    if (pthread_create(&log_srv, NULL, log_server, NULL) == 0) pthread_detach(log_srv);
    crash_setup();
    if (getenv("DROID_CRASH_GUARD")) { pthread_t cg; if (pthread_create(&cg, NULL, crash_guard, NULL) == 0) pthread_detach(cg); }
    signal(SIGPIPE, SIG_IGN); // a closed connection: an error from send(), not the app's end
    check_imports();
    thread_place_init(); // thread_place.c: the main and render threads' own cores
    sceSystemServiceHideSplashScreen();
    int rc;
    if (!native_game) {
        say(0, "videoOut handle %#x after %d busy tries", video, busy_tries);
        if (video < 0) {
            say(1, "Android: videoOut open fail %#x", video);
            halt();
        }
        rc = display_setup();
        if (rc < 0) {
            say(1, "Android: display setup fail %#x", rc);
            halt();
        }
    }
    const char *lib = apk_game ? APP0 "/game.apk" : native_game ? GAME_LIB : TEST_LIB;
    say(1, "Android started");
    say(0, "Android: starting %s", lib);

    jni_setup();
    so_import_resolver = droid_resolve;
    so_missing_import = missing;
    so_arm_import_resolver = arm_import;
    if (apk_game) {
        // the app's game: single_game.txt names it, its file is in /app0/game
        static lib_game g;
        const int single = single_game_pick(&g);
        if (single < 0) say(1, "No single_game.txt in this app (title/make_game_app.py writes it)");
        if (single == 0) {
            game_started = 1;
            say(1, "Android: %s", g.name);
            droid_startup_begin();
            game_settings_load(g.dir);
            game_data_prepare();
            set_known_package(&g);
            run_apk(g.path);
        }
        say(1, "Android: no game to run");
        halt();
    }
    arm_self_test();
    so_module mod;
    rc = so_load(lib, &mod);
    if (rc != 0) {
        say(1, "Android: %s load fail rc=%d", lib, rc);
        halt();
    }
    if (mod.arm64) {
        droid_arm_game = 1;
        int ok = 1;
        for (int i = 0; i < mod.ctor_count && ok; i++) arm_call(mod.ctors[i], NULL, 0, &ok);
        say(0, "ARM64 game: %d constructors in the JIT (ok=%d), %d thunks", mod.ctor_count, ok,
            arm_thunk_total());
    }
    say(0, "%s at %p (%llu KB), constructors run", lib, mod.base,
        (unsigned long long)(mod.size >> 10));

    if (native_game) run_native_activity(&mod);
    else run_test_game(&mod);
}
