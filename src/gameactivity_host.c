// gameactivity_host.c — the Java side of a GameActivity game (Google's Android Game SDK), played
// by us: what the game's Activity and com.google.androidgamesdk.GameActivity do on a phone.
// GameActivity keeps its setup in the game's library: Java calls initializeNativeCode (it makes
// the activity and starts the game's own android_main thread) and then the lifecycle natives
// it registered (onStart, onResume, the surface, focus); the game draws on its thread.
// King's engine (Candy Crush Saga) first makes its own native side: PlatformProxy
// .createNativeInstance(activity), then NativeApplication.create(that, options, version, assets),
// as CandyCrushSagaActivity / EngineActivity.onCreate do before GameActivity.onCreate.
// Games in GAMEACTIVITY_GAMES only (a new engine path).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "FalsoJNI.h"
#include "android_shim.h"
#include "arm_cpu.h"
#include "game_settings.h"
#include "gameactivity_host.h"
#include "dpad_grid.h"
#include "keymap.h"
#include "java_layer.h"
#include "jni_impl.h"
#include "native_activity.h"

#define GAMEACTIVITY_GAMES "com.king.candycrushsaga"
#define GA "Java_com_google_androidgamesdk_GameActivity_"

int sceKernelUsleep(uint32_t us);
extern char game_dir[];

static so_module *game;
static void *env;

int gameactivity_detect(so_module *mod) {
    return droid_game_in(GAMEACTIVITY_GAMES) && so_sym(mod, GA "initializeNativeCode") != NULL;
}

// an exported native (Java_...), callable from here
static void *exported(const char *name) {
    void *p = so_sym(game, name);
    if (!p) droid_log("gameactivity: %s not exported", name);
    return p && game->arm64 ? arm_callback((uint64_t)p) : p;
}
// a native the library registered (RegisterNatives), by its method name
static void *registered(const char *name) {
    void *p = jni_find_native(name);
    if (!p) droid_log("gameactivity: native %s was not registered", name);
    return p;
}

// the ARM address behind a registered native (our trampoline: mov r10, <guest>; mov r11, ...)
static uint64_t guest_of(void *tramp) {
    const uint8_t *p = tramp;
    if (p && p[0] == 0x49 && p[1] == 0xBA) {
        uint64_t g;
        memcpy(&g, p + 2, 8);
        return g;
    }
    return (uint64_t)(uintptr_t)tramp;
}

// ---- touches: the touchpad / the PC's mouse as the touchscreen (as unity_host.c) ----------------
#define SLOTS (4 + KEYMAP_FINGERS) // 0-3 the touchpad / the cursor, then the keymap's (keymap.c)
#define SCREEN_W ((float)droid_game_w) // the game's screen: 1920x1080, a phone's 1080x1920 in the phone view
#define SCREEN_H ((float)droid_game_h)
static struct { int down; float x, y; } slot[SLOTS];
static int64_t touch_down_ms;
static uint64_t touch_fn; // onTouchEventNative's ARM address
static void *ga_activity;
static int64_t ga_handle;
static int64_t now_ms_i(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void touch(int s, int down, float x, float y) {
    const int was = slot[s].down;
    if (!was && !down) return;
    if (was && down && slot[s].x == x && slot[s].y == y) return;
    const int64_t t = now_ms_i();
    int others = 0;
    for (int i = 0; i < SLOTS; i++) others += i != s && slot[i].down;
    if (!was && !others) touch_down_ms = t;
    slot[s].x = x;
    slot[s].y = y;
    slot[s].down = 1; // an UP event still lists the finger
    int ids[SLOTS], index = 0, n = 0;
    float xs[SLOTS], ys[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        if (!slot[i].down) continue;
        if (i == s) index = n;
        ids[n] = i;
        xs[n] = slot[i].x;
        ys[n] = slot[i].y;
        n++;
    }
    int action;
    if (!was) action = others ? 5 | (index << 8) : 0;       // POINTER_DOWN / DOWN
    else if (!down) action = others ? 6 | (index << 8) : 1; // POINTER_UP / UP
    else action = 2;                                         // MOVE
    slot[s].down = down;
    if (!touch_fn) return;
    jobject ev = java_motion_event(action, 0x1002, 1, n, ids, xs, ys, NULL, t * 1000000, touch_down_ms * 1000000);
    // onTouchEventNative(env, this, handle, event, pointerCount, historySize, deviceId, source | action,
    // eventTime, downTime, flags, metaState, actionButton, buttonState, classification, edgeFlags | xPrecision, yPrecision)
    const uint64_t x8[8] = { (uint64_t)(uintptr_t)env, (uint64_t)(uintptr_t)ga_activity, (uint64_t)ga_handle,
                             (uint64_t)(uintptr_t)ev, (uint64_t)n, 0, 1, 0x1002 };
    const uint64_t stk[9] = { (uint64_t)action, (uint64_t)t, (uint64_t)touch_down_ms, 0, 0, 0, 0, 0, 0 };
    const float prec[2] = { 1.0f, 1.0f };
    int ok = 0;
    arm_call_full(touch_fn, x8, 8, prec, 2, stk, 9, &ok);
    static int told;
    if (told++ < 6) droid_log("gameactivity: touch %d at %.0f,%.0f (action %#x, %d finger(s))", s, x, y, action, n);
}
// ---- the cursor (as unity_host.c's): a stick glides it, R2 touches there (held: drags); drawn
// by gles_bridge.c from these while it's in use. The touchpad's fingers: slots 0-2, the cursor 3
// the cursor's touch: finger 0, as a phone's first finger (on slot 3 the game saw a pointer id 3
// with no 0-2: the console closed at its Play button; the PC's mouse, finger 0, played)
#define CURSOR_SLOT 0
extern float droid_cursor_x, droid_cursor_y; // unity_host.c
extern volatile int droid_cursor_shown;
static float cursor_sx, cursor_sy;
static int cursor_pressing;
static int64_t cursor_last_ms, cursor_used_ms;
// ---- the D-pad: the cursor one tile that way (a candy), gliding onto its centre; with a finger
// down (Cross held), a swipe of one tile (a swap). The tiles are found in the picture
// (dpad_grid.c); none found: a step of DPAD_STEP. Held: again every DPAD_REPEAT.
#define DPAD_STEP 80.0f
#define DPAD_GLIDE_MS 90
#define DPAD_REPEAT_MS 170
static int dpad_dir_x, dpad_dir_y;     // the direction held
static int64_t dpad_next_ms;           // its next step (held)
static int dpad_pending;               // a step waiting for the board's answer
static int64_t dpad_asked_ms;
static float glide_from_x, glide_from_y, glide_to_x, glide_to_y;
static int64_t glide_start_ms = -1;
// the board finder's answer in the game's points (it reads the TV's picture: in the phone view the
// game is smaller there, and in the middle)
static int grid_get(dpad_grid *g) {
    if (!dpad_grid_get(g)) return 0;
    if (phone_view()) {
        const float k = (float)droid_game_h / 1080; // TV pixels -> the game's
        view_from_tv(g->cx, g->cy, &g->cx, &g->cy);
        view_from_tv(g->x0, g->y0, &g->x0, &g->y0);
        view_from_tv(g->x1, g->y1, &g->x1, &g->y1);
        g->pitch_x *= k, g->pitch_y *= k;
    }
    return 1;
}
static void grid_request(float gx, float gy) { // around a point of the game's
    float tx, ty;
    view_to_tv(gx, gy, &tx, &ty);
    dpad_grid_request(tx, ty);
}
static void dpad_step(int dx, int dy) {
    dpad_grid g;
    float step_x = DPAD_STEP, step_y = DPAD_STEP, x = droid_cursor_x, y = droid_cursor_y;
    if (grid_get(&g) && g.ok) { // from the nearest tile's centre, one tile over
        step_x = g.pitch_x, step_y = g.pitch_y;
        x = g.cx + roundf((x - g.cx) / g.pitch_x) * g.pitch_x;
        y = g.cy + roundf((y - g.cy) / g.pitch_y) * g.pitch_y;
    }
    glide_from_x = droid_cursor_x, glide_from_y = droid_cursor_y;
    glide_to_x = x + dx * step_x, glide_to_y = y + dy * step_y;
    if (glide_to_x < 0) glide_to_x = 0;
    if (glide_to_y < 0) glide_to_y = 0;
    if (glide_to_x > SCREEN_W - 1) glide_to_x = SCREEN_W - 1;
    if (glide_to_y > SCREEN_H - 1) glide_to_y = SCREEN_H - 1;
    glide_start_ms = now_ms_i();
    static int told;
    if (told++ < 20) droid_log("dpad: the cursor %.0f,%.0f -> %.0f,%.0f (a step %.0f x %.0f)", glide_from_x, glide_from_y,
                               glide_to_x, glide_to_y, step_x, step_y);
}
static void dpad_press(int dx, int dy) {
    dpad_dir_x = dx, dpad_dir_y = dy;
    dpad_next_ms = now_ms_i() + 350;
    dpad_grid g;
    const int fresh = grid_get(&g) && g.ok && now_ms_i() / 1000.0 - g.at < 1.5 && droid_cursor_x > g.x0 + 60 &&
                      droid_cursor_x < g.x1 - 60 && droid_cursor_y > g.y0 + 60 && droid_cursor_y < g.y1 - 60;
    if (fresh) {
        dpad_step(dx, dy);
    } else { // the board first: at the next frame (or a plain step if none comes)
        grid_request(droid_cursor_x, droid_cursor_y);
        dpad_pending = 1;
        dpad_asked_ms = now_ms_i();
    }
    cursor_used_ms = now_ms_i();
}
static void dpad_tick(int64_t t) {
    if (dpad_pending) {
        dpad_grid g;
        if ((grid_get(&g) && g.at * 1000 >= dpad_asked_ms - 1) || t - dpad_asked_ms > 120) {
            dpad_pending = 0;
            dpad_step(dpad_dir_x, dpad_dir_y);
        }
    }
    if ((dpad_dir_x || dpad_dir_y) && !dpad_pending && t >= dpad_next_ms) { // held: again
        dpad_next_ms = t + DPAD_REPEAT_MS;
        grid_request(droid_cursor_x + dpad_dir_x * 100, droid_cursor_y + dpad_dir_y * 100);
        dpad_step(dpad_dir_x, dpad_dir_y);
    }
    if (glide_start_ms >= 0) {
        float k = (float)(t - glide_start_ms) / DPAD_GLIDE_MS;
        if (k >= 1) k = 1, glide_start_ms = -1;
        k = k * k * (3 - 2 * k);
        droid_cursor_x = glide_from_x + (glide_to_x - glide_from_x) * k;
        droid_cursor_y = glide_from_y + (glide_to_y - glide_from_y) * k;
        cursor_used_ms = t;
        if (cursor_pressing) touch(CURSOR_SLOT, 1, droid_cursor_x, droid_cursor_y); // a swipe
    }
}

static void ga_km_finger(int id, int down, float x, float y);
static void cursor_tick(void) {
    if (!settings.cursor) {
        droid_cursor_shown = 0;
        return;
    }
    const int64_t t = now_ms_i();
    double dt = cursor_last_ms ? (t - cursor_last_ms) / 1000.0 : 0;
    if (dt > 0.05) dt = 0.05;
    cursor_last_ms = t;
    double m = sqrt(cursor_sx * cursor_sx + cursor_sy * cursor_sy);
    if (m > 1) m = 1;
    if (m > 0.14) {
        const double speed = 1700.0 * pow((m - 0.14) / 0.86, 2.2) * dt; // px/s at full tilt, a curve
        droid_cursor_x += (float)(cursor_sx / m * speed);
        droid_cursor_y += (float)(cursor_sy / m * speed);
        if (droid_cursor_x < 0) droid_cursor_x = 0;
        if (droid_cursor_y < 0) droid_cursor_y = 0;
        if (droid_cursor_x > SCREEN_W - 1) droid_cursor_x = SCREEN_W - 1;
        if (droid_cursor_y > SCREEN_H - 1) droid_cursor_y = SCREEN_H - 1;
        cursor_used_ms = t;
        if (cursor_pressing) touch(CURSOR_SLOT, 1, droid_cursor_x, droid_cursor_y);
    }
    dpad_tick(t);
    keymap_tick(ga_km_finger);
    if (cursor_pressing) cursor_used_ms = t;
    droid_cursor_shown = cursor_used_ms && t - cursor_used_ms < 3000;
}
static int ga_r2, ga_click, ga_cross; // what holds the cursor's finger: R2, the touchpad's click, Cross
static void ga_cursor_hold(void) {
    const int down = ga_r2 || ga_click || ga_cross;
    if (settings.cursor && down != cursor_pressing) {
        cursor_pressing = down;
        cursor_used_ms = now_ms_i();
        touch(CURSOR_SLOT, down, droid_cursor_x, droid_cursor_y);
    }
}
static void ga_km_finger(int id, int down, float x, float y) { touch(4 + id, down, x, y); } // the keymap's
static void ga_input(const droid_input_event *e) {
    keymap_sync(); // (the Quick menu's editor may have changed it)
    if (e->type == 1 && keymap_key(e->keycode, e->action == 0, ga_km_finger)) return; // a mapped button: its spot
    if (e->type == 1 && e->keycode == 109) { // the touchpad's click: a tap at the cursor (two quick ones: a double tap)
        ga_click = e->action == 0;
        ga_cursor_hold();
        return;
    }
    if (e->type == 1 && e->keycode == 96 && settings.cursor) { // Cross: a tap at the cursor (held + the D-pad: a swipe)
        ga_cross = e->action == 0;
        ga_cursor_hold();
        return;
    }
    if (e->type == 1 && e->keycode >= 19 && e->keycode <= 22 && settings.cursor) { // the D-pad: one tile
        const int dx = e->keycode == 21 ? -1 : e->keycode == 22 ? 1 : 0, dy = e->keycode == 19 ? -1 : e->keycode == 20 ? 1 : 0;
        if (e->action == 0) dpad_press(dx, dy);
        else if (dx == dpad_dir_x && dy == dpad_dir_y) dpad_dir_x = dpad_dir_y = 0;
        return;
    }
    if (e->type == DROID_EVENT_TOUCH) { // the touchpad is the touchscreen
        const int n = (int)e->axes[0];
        for (int f = 0; f < SLOTS; f++) {
            if (f == CURSOR_SLOT && cursor_pressing) continue; // (the cursor holds it)
            if (f < n) touch(f, 1, e->axes[1 + f * 3] * SCREEN_W, e->axes[2 + f * 3] * SCREEN_H);
            else touch(f, 0, slot[f].x, slot[f].y);
        }
        return;
    }
    if (e->type == 2) { // the sticks (either one) glide the cursor, R2 touches at it
        float lx = e->axes[0], ly = e->axes[1], rx = e->axes[11], ry = e->axes[14];
        keymap_stick(lx, ly); // a keymap's joysticks: the left stick's, the right one's
        keymap_stick2(rx, ry);
        keymap_triggers(e->axes[17], e->axes[18], ga_km_finger); // L2 / R2 on their spots, if mapped
        // the cursor: a stick no joystick took (the left one first, unless it has one)
        const int left_used = !keymap_has_stick() && (keymap_has_stick2() || lx * lx + ly * ly >= rx * rx + ry * ry);
        if (keymap_has_stick() && keymap_has_stick2()) rx = ry = 0;
        cursor_sx = left_used ? lx : rx;
        cursor_sy = left_used ? ly : ry;
        ga_r2 = !keymap_has_key(KEY_R2) && e->axes[18] > 0.5f; // (R2 mapped: its spot, not the cursor)
        ga_cursor_hold();
    }
}

// ---- tap.txt (a test tool): "x y [ms]" lines put into the app folder over FTP are tapped one by
// one as finger 0 (1920x1080, held ms, default 120), then the file goes; looked for 4 times a second.
// A 4th number 1: every host call goes to the log from that tap's press to 2.5 s after it lets go
// (the console's silent deaths)
extern int droid_host_trace; // arm_cpu.cpp
static int tap_x[16], tap_y[16], tap_ms[16], tap_tr[16], tap_n, tap_i;
static int64_t tap_up_ms;    // when the held tap lets go (0: none down)
static int64_t tap_trace_ms; // when the trace goes off
static void tap_tick(int look) {
    const int64_t t = now_ms_i();
    if (tap_trace_ms && t >= tap_trace_ms) {
        droid_host_trace = 0;
        tap_trace_ms = 0;
    }
    if (tap_up_ms) {
        if (t < tap_up_ms) return;
        touch(0, 0, tap_x[tap_i], tap_y[tap_i]);
        if (tap_tr[tap_i]) tap_trace_ms = t + 2500;
        droid_log("tap.txt: up at %d,%d", tap_x[tap_i], tap_y[tap_i]);
        tap_up_ms = 0;
        tap_i++;
        return;
    }
    if (tap_i < tap_n) {
        if (tap_tr[tap_i]) droid_host_trace = 1;
        touch(0, 1, tap_x[tap_i], tap_y[tap_i]);
        droid_log("tap.txt: down at %d,%d for %d ms", tap_x[tap_i], tap_y[tap_i], tap_ms[tap_i]);
        tap_up_ms = t + tap_ms[tap_i];
        return;
    }
    if (!look) return;
    FILE *f = fopen(APP0 "/tap.txt", "r");
    if (!f) return;
    char line[64];
    tap_n = tap_i = 0;
    while (tap_n < 16 && fgets(line, sizeof line, f)) {
        int x, y, ms = 120, tr = 0;
        if (sscanf(line, "%d %d %d %d", &x, &y, &ms, &tr) >= 2) {
            tap_x[tap_n] = x, tap_y[tap_n] = y, tap_ms[tap_n] = ms > 0 ? ms : 120, tap_tr[tap_n] = tr;
            tap_n++;
        }
    }
    fclose(f);
    unlink(APP0 "/tap.txt");
    droid_log("tap.txt: %d tap(s)", tap_n);
}

void gameactivity_run(so_module *mod, void (*poll)(void (*)(const droid_input_event *))) {
    game = mod;
    env = mod->arm64 ? arm_jni_env() : (void *)&jni;
    void *vm = mod->arm64 ? arm_java_vm() : (void *)&jvm;
    mkdir(APP0 "/data", 0777);
    // System.loadLibrary: JNI_OnLoad first (GameActivity and King register their natives there)
    void *onload = so_sym(mod, "JNI_OnLoad");
    if (onload) {
        typedef jint (*onload_fn)(void *, void *);
        void *fn = mod->arm64 ? arm_callback((uint64_t)onload) : onload;
        droid_log("gameactivity: JNI_OnLoad = %#x", ((onload_fn)fn)(vm, NULL));
    }
    void *activity = strdup("com/king/candycrushsaga/CandyCrushSagaActivity");
    void *assets = strdup("android/content/res/AssetManager");
    void *options = strdup("com/king/core/GameView$InitializationOptions"); // its fields read as 0: false, false
    void *native_app = strdup("com/king/core/gameactivity/NativeApplication");
    void *config = strdup("android/content/res/Configuration");
    void *surface = strdup("android/view/Surface");

    // CandyCrushSagaActivity.onCreate: PlatformProxy.createNativeInstance(this)
    typedef int64_t (*create_instance_fn)(void *, void *, void *);
    create_instance_fn create_instance = (create_instance_fn)exported("Java_com_king_candycrushsaga_PlatformProxy_createNativeInstance");
    int64_t proxy = 0;
    if (create_instance) {
        proxy = create_instance(env, strdup("com/king/candycrushsaga/PlatformProxy"), activity);
        droid_log("gameactivity: PlatformProxy.createNativeInstance -> %#llx", (unsigned long long)proxy);
    }
    // EngineActivity.onCreateImpl: new NativeApplication().create(proxy, options, version, assets)
    typedef void (*app_create_fn)(void *, void *, int64_t, void *, void *, void *);
    app_create_fn app_create = (app_create_fn)exported("Java_com_king_core_gameactivity_NativeApplication_create");
    if (app_create) {
        extern const char *droid_version_name; // app_main.c: the APK's versionName
        app_create(env, native_app, proxy, options, jni->NewStringUTF(&jni, droid_version_name ? droid_version_name : "1.0"),
                   assets);
        droid_log("gameactivity: NativeApplication.create done");
    }
    // GameActivity.onCreate: initializeNativeCode(filesDir, obbDir, externalFilesDir, assets, state, config)
    typedef int64_t (*init_fn)(void *, void *, void *, void *, void *, void *, void *, void *);
    init_fn init = (init_fn)exported(GA "initializeNativeCode");
    if (!init) return;
    char files[400];
    snprintf(files, sizeof(files), "%s/files", game_dir);
    mkdir(files, 0777);
    void *jfiles = jni->NewStringUTF(&jni, files), *jdir = jni->NewStringUTF(&jni, game_dir);
    extern int arm_big_stack_next; // arm_cpu.cpp: the app thread it starts draws (shader compiles)
    arm_big_stack_next = 1;
    const int64_t handle = init(env, activity, jfiles, jdir, jfiles, assets, NULL, config);
    arm_big_stack_next = 0;
    droid_log("gameactivity: initializeNativeCode -> %#llx", (unsigned long long)handle);
    if (!handle) return;
    // the lifecycle, as GameActivity's onStart / onResume / surfaceCreated / surfaceChanged /
    // onWindowFocusChanged pass it on
    typedef void (*h_fn)(void *, void *, int64_t);
    typedef void (*h_obj_fn)(void *, void *, int64_t, void *);
    typedef void (*h_changed_fn)(void *, void *, int64_t, void *, int32_t, int32_t, int32_t);
    typedef void (*h_bool_fn)(void *, void *, int64_t, int32_t);
    h_fn on_start = (h_fn)registered("onStartNative"), on_resume = (h_fn)registered("onResumeNative");
    h_obj_fn on_surface = (h_obj_fn)registered("onSurfaceCreatedNative");
    h_changed_fn on_changed = (h_changed_fn)registered("onSurfaceChangedNative");
    h_bool_fn on_focus = (h_bool_fn)registered("onWindowFocusChangedNative");
    if (on_start) on_start(env, activity, handle);
    if (on_resume) on_resume(env, activity, handle);
    droid_window *w = activity_window();
    if (on_surface) on_surface(env, activity, handle, surface);
    if (on_changed) on_changed(env, activity, handle, surface, 1, w ? w->width : 1920, w ? w->height : 1080); // RGBA_8888
    if (on_focus) on_focus(env, activity, handle, 1);
    droid_log("gameactivity: started, resumed, surface %dx%d, focus: the game's thread runs it",
              w ? w->width : 1920, w ? w->height : 1080);
    ga_activity = activity;
    ga_handle = handle;
    touch_fn = guest_of(registered("onTouchEventNative"));
    // the touches, 120 times a second; every 20 s for the first 2 minutes: what every thread is doing
    for (int s = 1;; s++) {
        sceKernelUsleep(1000000 / 120);
        if (poll) poll(ga_input);
        cursor_tick();
        tap_tick(s % 30 == 0);
        if (s % 2400 == 0 && s <= 14400) {
            droid_log("gameactivity: %d s in: what every thread is doing:", s / 120);
            arm_threads_report();
        }
    }
}
