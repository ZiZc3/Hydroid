// gtasa_host.c — the Java side of Rockstar's Android ports (GTA San Andreas 2.11's oswrapper),
// played by us: what com.rockstargames.oswrapper.GameActivityBase / GameThread / GameView do on a
// phone. Every GameNative.impl* call runs on one thread (GameThread's executor), in the order
// the Java side queues them: initial setup (device, assets, data packs), Rockstar setup, activity
// created, surface created / changed, resume, then implOnDrawFrame(dt) every frame while
// implIsInitialized(). The engine makes its own EGL context on the window it is given.
// The DualSense goes in as Rockstar's gamepad (InputHandler's button numbers).

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FalsoJNI.h"
#include "android_shim.h"
#include "arm_cpu.h"
#include "gtasa_host.h"
#include "java_layer.h"

#define GN "Java_com_rockstargames_oswrapper_GameNative_"

static struct {
    void (*initial_setup)(void *, void *, void *, void *, void *, void *);
    void (*rockstar_setup)(void *, void *, void *, void *);
    void (*rockstar_initial)(void *, void *);
    void (*activity_created)(void *, void *, void *, uint8_t);
    void (*surface_created)(void *, void *);
    void (*surface_changed)(void *, void *, void *, int32_t, int32_t);
    void (*resume)(void *, void *);
    uint8_t (*is_initialized)(void *, void *);
    void (*draw_frame)(void *, void *, float);
    void (*pad_connected)(void *, void *, int32_t);
    void (*pad_down)(void *, void *, int32_t, int32_t);
    void (*pad_up)(void *, void *, int32_t, int32_t);
    void (*pad_axes)(void *, void *, int32_t, float, float, float, float, float, float);
    void (*touch_start)(void *, void *, int32_t, float, float);
    void (*touch_move)(void *, void *, int32_t, float, float);
    void (*touch_end)(void *, void *, int32_t, float, float);
} gn;

static void *env, *clazz;
static so_module *game;
static gtasa_poll_fn poll_input;

// ---- GamePlatformServices' answers ------------------------------------------------------------
// The engine asks the Java side (RockstarServices) for Social Club's first login, the "play"
// gate, sign-in, its HTTP requests... and waits for GameThread.onRockstar*Complete, posted to the
// game thread's queue and run between frames. Here offline: done at once, the gate passed, HTTP
// failed (java_layer.c posts them). Unanswered, the engine waited and its frames did nothing.
static struct {
    void (*gate_complete)(void *, void *, int32_t, uint8_t);
    void (*sign_in_complete)(void *, void *);
    void (*sign_out_complete)(void *, void *);
    void (*cloud_disabled_complete)(void *, void *);
    void (*deletion_complete)(void *, void *);
    void (*http_error)(void *, void *, int32_t, int32_t);
    void (*playlist_open_complete)(void *, void *, uint8_t, int32_t);
} gs;
#define POSTED_MAX 64
static struct { int kind, id; } posted[POSTED_MAX];
static int posted_n;
static pthread_mutex_t posted_lock = PTHREAD_MUTEX_INITIALIZER;

void gtasa_post(int kind, int id) {
    pthread_mutex_lock(&posted_lock);
    if (posted_n < POSTED_MAX) {
        posted[posted_n].kind = kind;
        posted[posted_n++].id = id;
    }
    pthread_mutex_unlock(&posted_lock);
}

static void run_posted(void) {
    pthread_mutex_lock(&posted_lock);
    int n = posted_n;
    typeof(posted[0]) todo[POSTED_MAX];
    memcpy(todo, posted, sizeof(todo[0]) * (size_t)n);
    posted_n = 0;
    pthread_mutex_unlock(&posted_lock);
    for (int i = 0; i < n; i++) {
        static const char *const what[] = { "first login", "gate", "sign-in", "sign-out", "cloud disabled",
                                            "account deletion", "HTTP request", "playlist (user tracks)" };
        const int k = todo[i].kind;
        droid_log("gtasa: %s %d -> complete (offline)", k >= 0 && k <= GTASA_PLAYLIST ? what[k] : "?", todo[i].id);
        switch (k) {
        case GTASA_INITIAL: if (gn.rockstar_initial) gn.rockstar_initial(env, clazz); break;
        case GTASA_GATE: if (gs.gate_complete) gs.gate_complete(env, clazz, todo[i].id, 1); break;
        case GTASA_SIGN_IN: if (gs.sign_in_complete) gs.sign_in_complete(env, clazz); break;
        case GTASA_SIGN_OUT: if (gs.sign_out_complete) gs.sign_out_complete(env, clazz); break;
        case GTASA_CLOUD_DISABLED: if (gs.cloud_disabled_complete) gs.cloud_disabled_complete(env, clazz); break;
        case GTASA_DELETION: if (gs.deletion_complete) gs.deletion_complete(env, clazz); break;
        case GTASA_HTTP: if (gs.http_error) gs.http_error(env, clazz, todo[i].id, -1); break;
        case GTASA_PLAYLIST: // no music library here: none, 0 tracks
            if (gs.playlist_open_complete) gs.playlist_open_complete(env, clazz, 0, 0);
            break;
        }
    }
}

int gtasa_detect(so_module *mod) { return so_sym(mod, GN "implOnDrawFrame") != NULL; }

static void *native(const char *name) {
    char full[128];
    snprintf(full, sizeof(full), GN "%s", name);
    void *p = so_sym(game, full);
    if (!p) droid_log("gtasa: %s not found", full);
    return p ? arm_callback((uint64_t)p) : NULL;
}

// ---- DualSense -> Rockstar's gamepad ------------------------------------------------------
// InputHandler.toGamepadButton: Cross 0, Circle 1, Square 2, Triangle 3, L1 6, R1 7, the D-pad
// 8-11 (up, down, left, right: from the hat axes), Back 14, Menu 15 (L3 / R3 / Start / Select:
// their case list was lost by the decompiler - 12 / 13 / 15 / 14, as the numbers run)
static int pad_button(int keycode) {
    switch (keycode) {
    case 96: return 0;   // Cross
    case 97: return 1;   // Circle
    case 99: return 2;   // Square
    case 100: return 3;  // Triangle
    case 102: return 6;  // L1
    case 103: return 7;  // R1
    case 19: return 8;   // D-pad up
    case 20: return 9;   // down
    case 21: return 10;  // left
    case 22: return 11;  // right
    case 106: return 12; // L3
    case 107: return 13; // R3
    case 109: return 14; // touchpad / Select: Back
    case 108: return 15; // Options: Menu
    case 4: return 14;
    case 82: return 15;
    }
    return -1;
}

// Touch (the PC's mouse, the DualSense touchpad as the screen): Android pixels, a pointer id per
// finger, as GameView's onTouchEvent hands them on. Without it the menus took no clicks
static struct { int down, id; float x, y; } fingers[2];
static void gtasa_touch(const droid_input_event *e) {
    const int n = (int)e->axes[0];
    for (int f = 0; f < 2; f++) {
        const int down = f < n;
        const float x = down ? e->axes[1 + f * 3] * 1920.0f : fingers[f].x;
        const float y = down ? e->axes[2 + f * 3] * 1080.0f : fingers[f].y;
        if (down && !fingers[f].down && gn.touch_start) gn.touch_start(env, clazz, f, x, y);
        else if (down && (x != fingers[f].x || y != fingers[f].y) && gn.touch_move) gn.touch_move(env, clazz, f, x, y);
        else if (!down && fingers[f].down && gn.touch_end) gn.touch_end(env, clazz, f, x, y);
        fingers[f].down = down;
        fingers[f].x = x;
        fingers[f].y = y;
    }
}

static void gtasa_input(const droid_input_event *e) {
    if (e->type == DROID_EVENT_TOUCH) {
        gtasa_touch(e);
        return;
    }
    if (e->type == 1) { // key
        int b = pad_button(e->keycode);
        if (b < 0) return;
        if (e->action == 0) gn.pad_down(env, clazz, 0, b);
        else gn.pad_up(env, clazz, 0, b);
        return;
    }
    static float last[6];
    // InputHandler: the right stick at half, the triggers as the most of their axes
    const float now[6] = { e->axes[0], e->axes[1], e->axes[11] * 0.5f, e->axes[14] * 0.5f, e->axes[17], e->axes[18] };
    if (!memcmp(now, last, sizeof(now))) return;
    memcpy(last, now, sizeof(now));
    gn.pad_axes(env, clazz, 0, now[0], now[1], now[2], now[3], now[4], now[5]);
}

// ---- the game thread --------------------------------------------------------------------------

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

void thread_place_role(int role, const char *why);
void arm_want_big_stack(void);

// A frame that doesn't come back: every thread's state, from a thread of its own (the game
// thread is the stuck one). GTA San Andreas's first implOnDrawFrame never returned on the PS5
// - a black screen with nothing in the log
static volatile uint64_t frames_begun, frames_done;
static void *watchdog(void *arg) {
    (void)arg;
    uint64_t last = 0;
    int reports = 0;
    for (;;) {
        struct timespec w = { 5, 0 };
        nanosleep(&w, NULL);
        const uint64_t done = frames_done;
        if (frames_begun > done && done == last && reports < 6) {
            reports++;
            droid_log("gtasa: frame %llu has not come back for 5 s: what every thread is doing",
                      (unsigned long long)done + 1);
            arm_threads_report();
        }
        last = done;
    }
    return NULL;
}

static void *game_thread(void *arg) {
    (void)arg;
    arm_want_big_stack();
    thread_place_role(0, "GTA"); // every frame on this thread: the main core
    const double start = now_ms();

    // GameActivityBase.onCreate: onInitialSetup (device, assets, packs "data_main" in the APK),
    // onRockstarSetup, onActivityCreated(services, firstInit)
    void *device = strdup("com/rockstargames/oswrapper/DeviceInfo");
    void *assets = strdup("android/content/res/AssetManager");
    jobjectArray names = jni->NewObjectArray(&jni, 1, NULL, NULL);
    jni->SetObjectArrayElement(&jni, names, 0, jni->NewStringUTF(&jni, "data_main"));
    jobjectArray paths = jni->NewObjectArray(&jni, 1, NULL, NULL); // null: the pack is the APK's assets
    droid_log("gtasa: initial setup");
    gn.initial_setup(env, clazz, device, assets, names, paths);
    if (gn.rockstar_setup) {
        droid_log("gtasa: Rockstar setup (offline)");
        gn.rockstar_setup(env, clazz, jni->NewStringUTF(&jni, "prod"), jni->NewStringUTF(&jni, ""));
    }
    void *services = strdup("com/rockstargames/oswrapper/GamePlatformServices");
    droid_log("gtasa: activity created");
    gn.activity_created(env, clazz, services, 1);
    run_posted(); // what it asked for there (Social Club's first login: rockstarShowInitial)

    // GameView: surfaceCreated, surfaceChanged(surface, w, h); onResume
    void *surface = strdup("android/view/Surface");
    droid_log("gtasa: surface");
    gn.surface_created(env, clazz);
    gn.surface_changed(env, clazz, surface, 1920, 1080);
    gn.resume(env, clazz);
    // DROID_GTASA_TOUCH=1 (test): no gamepad announced, its hints name the touch controls
    if (gn.pad_connected && !getenv("DROID_GTASA_TOUCH")) gn.pad_connected(env, clazz, 0);
    droid_log("gtasa: running (%.1f s after start)", (now_ms() - start) / 1000);

    // GameThread's executor: implOnDrawFrame(dt) while implIsInitialized()
    double last = now_ms(), window = last, draw_total = 0;
    int frames = 0, said_init = 0;
    for (uint64_t frame = 0;; frame++) {
        poll_input(gtasa_input);
        run_posted(); // GameThread's event queue first, as its executor does
        const double t0 = now_ms();
        const uint8_t ready = gn.is_initialized(env, clazz);
        if (ready) {
            if (!said_init++) {
                droid_log("gtasa: initialized, frames from now (%.1f s)", (t0 - start) / 1000);
                void arm_prof_start(void); // arm_cpu.cpp: ArmProf, when tools.txt has it
                arm_prof_start();
            }
            frames_begun++;
            gn.draw_frame(env, clazz, (float)((t0 - last) / 1000.0));
            frames_done++;
            frames++;
        } else {
            struct timespec w = { 0, 4 * 1000 * 1000 };
            nanosleep(&w, NULL); // still loading on its own threads
        }
        const double t1 = now_ms();
        draw_total += t1 - t0;
        last = t0;
        if (t1 - window >= 5000) {
            droid_log("gtasa: %.1f fps, frame %.1f ms average (%s)", frames * 1000.0 / (t1 - window),
                      frames ? draw_total / frames : 0, ready ? "playing" : "not initialized yet");
            arm_profile_dump(t1 - window, 10);
            static int reports;
            int droid_tool(const char *name); // game_settings.c
            if (droid_tool("ThreadScope") && reports++ % 4 == 0) { // (the debug tool: every 20 s)
                droid_log("gtasa: ThreadScope: what every thread is doing");
                arm_threads_report();
            }
            // implOnDrawFrame's three gates in 2.11's libGame.so: suspended (implOnPause / Resume),
            // OS_CanGameRender (set at the end of implOnSurfaceChanged), initialized
            static int probed;
            if (!probed++ && game->bias && 0xd19300 < game->size) {
                const volatile uint8_t *g = (const volatile uint8_t *)(uintptr_t)(game->bias + 0xd19000);
                droid_log("gtasa: frame gates: suspended %d, can render %d, initialized %d", g[0x2f1] & 1,
                          g[0x140] & 1, g[0x2f8] & 1);
            }
            window = t1;
            frames = 0;
            draw_total = 0;
        }
    }
    return NULL;
}

void gtasa_run(so_module *mod, gtasa_poll_fn poll) {
    game = mod;
    poll_input = poll;
    gn.initial_setup = native("implOnInitialSetup");
    gn.rockstar_setup = native("implOnRockstarSetup");
    gn.rockstar_initial = native("implOnRockstarInitialComplete");
    gn.activity_created = native("implOnActivityCreated");
    gn.surface_created = native("implOnSurfaceCreated");
    gn.surface_changed = native("implOnSurfaceChanged");
    gn.resume = native("implOnResume");
    gn.is_initialized = native("implIsInitialized");
    gn.draw_frame = native("implOnDrawFrame");
    gn.pad_connected = native("implOnGamepadConnected");
    gn.pad_down = native("implOnGamepadButtonDown");
    gn.pad_up = native("implOnGamepadButtonUp");
    gn.pad_axes = native("implOnGamepadAxesChanged");
    gn.touch_start = native("implOnTouchStart");
    gn.touch_move = native("implOnTouchMove");
    gn.touch_end = native("implOnTouchEnd");
    gs.gate_complete = native("implOnRockstarGateComplete");
    gs.sign_in_complete = native("implOnRockstarSignInComplete");
    gs.sign_out_complete = native("implOnRockstarSignOutComplete");
    gs.cloud_disabled_complete = native("implOnRockstarCloudDisabledComplete");
    gs.deletion_complete = native("implOnRockstarAccountDeletionComplete");
    gs.http_error = native("implOnHttpRequestError");
    gs.playlist_open_complete = native("implOnPlaylistOpenComplete");
    if (!gn.initial_setup || !gn.activity_created || !gn.surface_changed || !gn.is_initialized || !gn.draw_frame ||
        !gn.pad_down || !gn.pad_up || !gn.pad_axes)
        return;
    env = arm_jni_env();
    clazz = strdup("com/rockstargames/oswrapper/GameNative");
    // System.loadLibrary("Game"): its JNI_OnLoad first
    if (so_sym(mod, "JNI_OnLoad")) java_jni_onload(mod);

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20); // Mesa compiles shaders on it
    pthread_t t, dog;
    pthread_create(&dog, NULL, watchdog, NULL);
    pthread_create(&t, &a, game_thread, NULL);
    pthread_join(t, NULL);
}
