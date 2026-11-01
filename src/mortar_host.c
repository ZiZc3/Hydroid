// mortar_host.c — the Java side of a Halfbrick Mortar game, played by us: what
// com.halfbrick.mortar.MortarGameActivity / GameManager / MortarGameView do on a phone. The GL
// view's render thread runs GameManager.Render every frame; its first frames walk SystemInit's
// states (Uninitialized: the device properties, on a thread of their own; DoDevicePropertiesInit:
// waits for them; FinializeInit: initFileSystem -> InitFileManager(apk, files/, cache/, external,
// false), SystemInit(w, h, language), the licence check), then GameInit once, then step() every
// frame (false: the game ended). Touches: touchEvent(action, time, id, x / w, y / h, pressure,
// size), as TouchInputHandler sends them. (Fruit Ninja 2.8.9, Fruit Ninja Classic 2.4.6:
// read from their smali, Apktool.)

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "FalsoJNI.h"
#include "android_shim.h"
#include "keymap.h"
#include "arm_cpu.h"
#include "arm_modules.h"
#include "game_settings.h"
#include "java_layer.h"
#include "mortar_host.h"
#include "native_activity.h"

#define LIB "Java_com_halfbrick_mortar_NativeGameLib_native_1"
#define SCREEN_W 1920.0f
#define SCREEN_H 1080.0f

typedef void *(*egl_get_display_fn)(void *);
typedef unsigned (*egl_initialize_fn)(void *, int *, int *);
typedef unsigned (*egl_choose_config_fn)(void *, const int *, void **, int, int *);
typedef void *(*egl_create_surface_fn)(void *, void *, void *, const int *);
typedef void *(*egl_create_context_fn)(void *, void *, void *, const int *);
typedef unsigned (*egl_make_current_fn)(void *, void *, void *, void *);
typedef unsigned (*egl_swap_fn)(void *, void *);

static struct {
    void (*device_properties)(void *, void *);
    void (*file_manager)(void *, void *, void *, void *, void *, void *, uint8_t);
    void (*system_init)(void *, void *, int32_t, int32_t, void *);
    void (*set_licensed)(void *, void *, uint8_t);
    void (*game_init)(void *, void *);
    uint8_t (*step)(void *, void *);
    void (*touch)(void *, void *, int32_t, int64_t, int32_t, float, float, float, float);
    void (*key)(void *, void *, int32_t, uint8_t, uint8_t, int32_t);
    void (*motion)(void *, void *, int32_t, int32_t, float, float); // (device, axis, x, y)
    void (*focus)(void *, void *);
    int32_t (*gles_version)(void *, void *);
} mo;

static void *env, *cls;
static so_module *game;
static mortar_poll_fn poll_input;

int mortar_detect(so_module *mod) { return so_sym(mod, LIB "step") != NULL; }

static void *native(const char *name) {
    char full[128];
    snprintf(full, sizeof(full), "%s%s", LIB, name);
    void *p = so_sym(game, full);
    if (!p) droid_log("mortar: %s not found", full);
    return p ? arm_callback((uint64_t)p) : NULL;
}

extern volatile int droid_mortar_hbid_wanted; // java_layer.c: RequestHBID() was called
void *droid_mortar_hbid(void);                  // the id, a Java string
static void (*hbid_result)(void *, void *, void *); // the game's GooglePlayServicesUtility.OnHBIDResult
extern volatile int droid_mortar_signin_wanted; // GooglePlaySignIn.SignIn() was called
static void (*signin_started)(void *, void *), (*signin_failed)(void *, void *);
static void (*manual_started)(void *, void *), (*manual_failed)(void *, void *);
// Java requests answered on the next frame (java_layer.c): 1 GooglePlaySnapshotManager.Open,
// 2 GooglePlayAchievementsManager.Load - each failed, as without Google Play Games
extern volatile int droid_mortar_answer_kind[32], droid_mortar_answer_id[32], droid_mortar_answers;
void *droid_mortar_snapshot_failed(void);
void *droid_mortar_achievements_failed(void);
static void (*snapshot_opened)(void *, void *, int32_t, void *);   // the game's OnSnapshotOpened(id, result)
static void (*achievements_loaded)(void *, void *, int32_t, void *); // its OnLoadCompleted(id, result)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// ---- touches: the touchpad (2 fingers), the cursor (Cross held: a swipe as the stick moves) ----
// ids: 0-1 the touchpad's fingers, 2 the cursor. MotionEvent's actions: DOWN 0, UP 1, MOVE 2
static struct { int down; float x, y; } finger[3 + KEYMAP_FINGERS]; // 3..: the keymap's (keymap.c)

static void finger_set(int id, int down, float x, float y) {
    int action = -1;
    if (down && !finger[id].down) action = 0;
    else if (down && (x != finger[id].x || y != finger[id].y)) action = 2;
    else if (!down && finger[id].down) action = 1;
    if (action >= 0 && mo.touch)
        mo.touch(env, cls, action, (int64_t)now_ms(), id, x / SCREEN_W, y / SCREEN_H, 1.0f, 0.1f);
    finger[id].down = down;
    finger[id].x = x;
    finger[id].y = y;
}

extern float droid_cursor_x, droid_cursor_y; // unity_host.c
extern volatile int droid_cursor_shown;
static float stick_x, stick_y;

static void mo_km_finger(int id, int down, float x, float y);
static double used_at; // MORTAR_PAD_GAMES: the cursor hides 3 s after it was last moved or tapped with
static int cursor_hides;
static void cursor_tick(double dt) {
    keymap_tick(mo_km_finger); // (a keymap's joystick, each frame)
    if (cursor_hides) {
        const double t = now_ms();
        if (finger[2].down) used_at = t;
        droid_cursor_shown = t - used_at < 3000.0;
    }
    float m = sqrtf(stick_x * stick_x + stick_y * stick_y);
    if (m < 0.15f) return;
    if (m > 1) m = 1;
    float speed = (finger[2].down ? 2600.0f : 1400.0f) * powf((m - 0.15f) / 0.85f, 1.6f) * (float)dt; // a slice is fast
    droid_cursor_x += stick_x / m * speed;
    droid_cursor_y += stick_y / m * speed;
    if (droid_cursor_x < 0) droid_cursor_x = 0;
    if (droid_cursor_y < 0) droid_cursor_y = 0;
    if (droid_cursor_x > SCREEN_W - 1) droid_cursor_x = SCREEN_W - 1;
    if (droid_cursor_y > SCREEN_H - 1) droid_cursor_y = SCREEN_H - 1;
    droid_cursor_shown = 1;
    if (finger[2].down) finger_set(2, 1, droid_cursor_x, droid_cursor_y);
}

static void mo_km_finger(int id, int down, float x, float y) { finger_set(3 + id, down, x, y); }

// Jetpack Joyride: Hydroid's buttons as before (Cross / the touchpad's click tap at the cursor,
// Circle / Options back), plus, as in the other games: the cursor shown from the start and moved
// by the right stick, R2 taps too (as Cross), hidden 3 s after its last use.
#define CURSOR_RIGHT_GAMES "com.halfbrick.jetpack,com.halfbrick.fruitninjafree"
static int tap_held[3]; // what holds the cursor's finger: Cross, the touchpad's click, R2
static void cursor_tap(int which, int down) {
    tap_held[which] = down;
    droid_cursor_shown = 1;
    used_at = now_ms();
    finger_set(2, tap_held[0] || tap_held[1] || tap_held[2], droid_cursor_x, droid_cursor_y);
}
static int cursor_right(const droid_input_event *e) { // the sticks / R2 (the buttons: mortar_input)
    static int ready;
    if (!droid_game_in(CURSOR_RIGHT_GAMES)) return 0;
    if (!ready) {
        ready = 1;
        droid_cursor_x = SCREEN_W * 0.5f, droid_cursor_y = SCREEN_H * 0.5f;
        cursor_hides = 1;
        used_at = now_ms(); // (shown at the start)
        droid_log("mortar: the cursor on the right stick, R2 / Cross taps (CURSOR_RIGHT_GAMES)");
    }
    if (e->type != 2) return 0; // a button, the touchpad: as before (2: the sticks / triggers)
    stick_x = e->axes[11], stick_y = e->axes[14]; // the right stick: the cursor (cursor_tick)
    if (stick_x * stick_x + stick_y * stick_y > 0.02f) used_at = now_ms();
    const int r2 = e->axes[18] > 0.5f;
    if (r2 != tap_held[2]) cursor_tap(2, r2);
    return 1;
}

static void mortar_input(const droid_input_event *e) {
    if (cursor_right(e)) return; // (Jetpack: the right stick and R2)
    keymap_sync(); // (the Quick menu's editor may have changed it)
    if (e->type == 1 && keymap_key(e->keycode, e->action == 0, mo_km_finger)) return; // a mapped button: its spot
    if (e->type == DROID_EVENT_TOUCH) { // the touchpad is the touchscreen
        int n = (int)e->axes[0];
        for (int f = 0; f < 2; f++) {
            int down = f < n;
            finger_set(f, down, down ? e->axes[1 + f * 3] * SCREEN_W : finger[f].x,
                       down ? e->axes[2 + f * 3] * SCREEN_H : finger[f].y);
        }
        return;
    }
    if (e->type == 1) { // a button
        int down = e->action == 0;
        switch (e->keycode) {
        case 96: case 109: // Cross, the touchpad's click: the finger down at the cursor (held: the stick slices)
            cursor_tap(e->keycode == 109, down);
            break;
        case 97: case 108: // Circle, Options: Back (KEYCODE_BACK 4)
            if (mo.key) mo.key(env, cls, 4, (uint8_t)down, 0, 0);
            break;
        }
        return;
    }
    keymap_stick(e->axes[0], e->axes[1]); // a keymap's joysticks: the left stick's, the right one's
    keymap_stick2(e->axes[11], e->axes[14]);
    keymap_triggers(e->axes[17], e->axes[18], mo_km_finger); // L2 / R2 on their spots, if mapped
    const int use_right = keymap_has_stick() && !keymap_has_stick2(), none = keymap_has_stick() && keymap_has_stick2();
    stick_x = none ? 0 : use_right ? e->axes[11] : e->axes[0];
    stick_y = none ? 0 : use_right ? e->axes[14] : e->axes[1];
}

// ---- the render thread (MortarGameView's GLThread) -----------------------------------------
void thread_place_role(int role, const char *why);
void arm_want_big_stack(void);
int droid_tool(const char *name);

// a frame that doesn't come back: every thread's state, from a thread of its own
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
            droid_log("mortar: frame %llu has not come back for 5 s: what every thread is doing",
                      (unsigned long long)done + 1);
            arm_threads_report();
        }
        last = done;
    }
    return NULL;
}

static volatile int props_done;
static void *props_thread(void *arg) { // GameManager's DevicePropertiesInitTask (an AsyncTask)
    (void)arg;
    if (mo.device_properties) mo.device_properties(env, cls);
    __atomic_store_n(&props_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *render_thread(void *arg) {
    const char *apk_path = arg;
    arm_want_big_stack();
    thread_place_role(0, "Mortar"); // every frame on this thread: the main core
    const double start = now_ms();
    // EGL as GLSurfaceViewPatched asks it: GLES 2, RGB888, depth 24 here, stencil 8
    egl_get_display_fn get_display = (egl_get_display_fn)droid_resolve("eglGetDisplay");
    egl_initialize_fn initialize = (egl_initialize_fn)droid_resolve("eglInitialize");
    egl_choose_config_fn choose = (egl_choose_config_fn)droid_resolve("eglChooseConfig");
    egl_create_surface_fn create_surface = (egl_create_surface_fn)droid_resolve("eglCreateWindowSurface");
    egl_create_context_fn create_context = (egl_create_context_fn)droid_resolve("eglCreateContext");
    egl_make_current_fn make_current = (egl_make_current_fn)droid_resolve("eglMakeCurrent");
    egl_swap_fn swap = (egl_swap_fn)droid_resolve("eglSwapBuffers");
    void *dpy = get_display(NULL);
    int major = 0, minor = 0;
    initialize(dpy, &major, &minor);
    const int attribs[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3025, 24, 0x3026, 8, 0x3040, 0x4, 0x3038 };
    void *config = NULL;
    int n = 0;
    choose(dpy, attribs, &config, 1, &n);
    void *surface = create_surface(dpy, config, activity_window(), NULL);
    const int ctx_attribs[] = { 0x3098, 2, 0x3038 };
    void *ctx = create_context(dpy, config, NULL, ctx_attribs);
    unsigned cur = make_current(dpy, surface, surface, ctx);
    unsigned (*swap_interval)(void *, int) = (unsigned (*)(void *, int))droid_resolve("eglSwapInterval");
    if (swap_interval) swap_interval(dpy, 1);
    droid_log("mortar: EGL surface %p context %p current %u (GLES %d asked)", surface, ctx, cur,
              mo.gles_version ? mo.gles_version(env, cls) : -1);

    // SystemInit's states: Uninitialized - the device properties, on their own thread
    pthread_t pt;
    pthread_create(&pt, NULL, props_thread, NULL);
    // DoDevicePropertiesInit: frames go by until they are done (none drawn here: we wait)
    while (!__atomic_load_n(&props_done, __ATOMIC_ACQUIRE)) {
        struct timespec w = { 0, 5000000 };
        nanosleep(&w, NULL);
    }
    pthread_join(pt, NULL);
    droid_log("mortar: device properties done (%.1f s)", (now_ms() - start) / 1000);
    // FinializeInit: initFileSystem (getFilesDir()/, getCacheDir()/, the external storage /),
    // SystemInit(width, height, language), the licence check (Market check: licensed)
    char files[300], cache[300], ext[300];
    game_path(files, sizeof(files) - 1, "files");
    mkdir(files, 0777);
    strcat(files, "/");
    game_path(cache, sizeof(cache) - 1, "cache");
    mkdir(cache, 0777);
    strcat(cache, "/");
    game_path(ext, sizeof(ext) - 1, "sdcard"); // the external storage: a folder of the game's
    mkdir(ext, 0777);
    strcat(ext, "/");
    droid_log("mortar: InitFileManager(%s, %s, %s, %s)", apk_path, files, cache, ext);
    mo.file_manager(env, cls, jni->NewStringUTF(&jni, apk_path), jni->NewStringUTF(&jni, files),
                    jni->NewStringUTF(&jni, cache), jni->NewStringUTF(&jni, ext), 0);
    mo.system_init(env, cls, (int32_t)SCREEN_W, (int32_t)SCREEN_H, jni->NewStringUTF(&jni, "en"));
    if (mo.set_licensed) mo.set_licensed(env, cls, 1);
    droid_log("mortar: SystemInit 1920x1080 done (%.1f s); GameInit", (now_ms() - start) / 1000);
    mo.game_init(env, cls);
    void *hb = so_sym(game, "Java_com_halfbrick_mortar_GooglePlayServicesUtility_OnHBIDResult");
    hbid_result = hb ? arm_callback((uint64_t)hb) : NULL;
    void *ss = so_sym(game, "Java_com_halfbrick_mortar_GooglePlaySignIn_OnSignInStarted");
    void *sf = so_sym(game, "Java_com_halfbrick_mortar_GooglePlaySignIn_OnSignInFailed");
    signin_started = ss ? arm_callback((uint64_t)ss) : NULL;
    signin_failed = sf ? arm_callback((uint64_t)sf) : NULL;
    void *ms = so_sym(game, "Java_com_halfbrick_mortar_GooglePlaySignIn_OnManualSignInStarted");
    void *mf = so_sym(game, "Java_com_halfbrick_mortar_GooglePlaySignIn_OnManualSignInFailed");
    manual_started = ms ? arm_callback((uint64_t)ms) : NULL;
    manual_failed = mf ? arm_callback((uint64_t)mf) : NULL;
    void *so = so_sym(game, "Java_com_halfbrick_mortar_GooglePlaySnapshotManager_OnSnapshotOpened");
    snapshot_opened = so ? arm_callback((uint64_t)so) : NULL;
    void *al = so_sym(game, "Java_com_halfbrick_mortar_GooglePlayAchievementsManager_OnLoadCompleted");
    achievements_loaded = al ? arm_callback((uint64_t)al) : NULL;
    droid_log("mortar: Java answers ready: HBID %s, sign-in %s, manual sign-in %s", hbid_result ? "yes" : "no",
              signin_failed ? "yes" : "no", manual_failed ? "yes" : "no");
    if (mo.focus) mo.focus(env, cls); // the window has the focus (onWindowFocusChanged)
    droid_log("mortar: running (%.1f s after start)", (now_ms() - start) / 1000);

    double last_end = now_ms();
    double last = now_ms(), window = last, step_total = 0, swap_total = 0, worst = 0;
    int frames = 0, slow = 0;
    for (uint64_t frame = 0;; frame++) {
        const double t0 = now_ms();
        poll_input(mortar_input);
        cursor_tick((t0 - last) / 1000.0);
        last = t0;
        if (droid_mortar_hbid_wanted && hbid_result) { // Java's answer to RequestHBID, as on a phone
            droid_mortar_hbid_wanted = 0;
            hbid_result(env, cls, droid_mortar_hbid());
            droid_log("mortar: OnHBIDResult given (frame %llu)", (unsigned long long)frame);
        }
        if (droid_mortar_answers) { // Java's answers to the game's requests (failed)
            const int n = droid_mortar_answers < 32 ? droid_mortar_answers : 32;
            int kind[32], id[32];
            for (int i = 0; i < n; i++) kind[i] = droid_mortar_answer_kind[i], id[i] = droid_mortar_answer_id[i];
            droid_mortar_answers = 0;
            for (int i = 0; i < n; i++) {
                if (kind[i] == 1 && snapshot_opened) snapshot_opened(env, cls, id[i], droid_mortar_snapshot_failed());
                else if (kind[i] == 2 && achievements_loaded)
                    achievements_loaded(env, cls, id[i], droid_mortar_achievements_failed());
                else continue;
                droid_log("mortar: request %d (%s) answered: failed (frame %llu)", id[i],
                          kind[i] == 1 ? "snapshot open" : "achievements load", (unsigned long long)frame);
            }
        }
        if (droid_mortar_signin_wanted) { // no Google Play Games: started, then failed
            const int w = droid_mortar_signin_wanted;
            droid_mortar_signin_wanted = 0;
            if ((w & 1) && signin_failed) {
                if (signin_started) signin_started(env, cls);
                signin_failed(env, cls);
                droid_log("mortar: OnSignInStarted + OnSignInFailed given (frame %llu)", (unsigned long long)frame);
            }
            if ((w & 2) && manual_failed) {
                if (manual_started) manual_started(env, cls);
                manual_failed(env, cls);
                droid_log("mortar: OnManualSignInStarted + OnManualSignInFailed given (frame %llu)", (unsigned long long)frame);
            }
        }
        frames_begun++;
        const uint8_t going = mo.step(env, cls);
        frames_done++;
        if (!going) {
            droid_log("mortar: step() said the game ended (frame %llu)", (unsigned long long)frame);
            break;
        }
        const double t1 = now_ms();
        swap(dpy, surface);
        const double t2 = now_ms();
        swap_total += t2 - t1;
        const double whole = t2 - last_end;
        if (frame > 0 && whole > worst) worst = whole;
        if (frame > 0 && whole > 25) slow++;
        // 60 a second, as a phone's GLSurfaceView (the swap may not wait for the refresh: 100 fps
        // on the PC, the game's clock ran ahead)
        const double spent = now_ms() - t0;
        if (spent < 16.4) {
            struct timespec w = { 0, (long)((16.4 - spent) * 1e6) };
            nanosleep(&w, NULL);
        }
        last_end = now_ms();
        step_total += t1 - t0;
        frames++;
        if (frame == 0) {
            droid_log("mortar: first frame %.1f s after start", (t1 - start) / 1000);
            void arm_prof_start(void); // arm_cpu.cpp: ArmProf, when tools.txt has it
            arm_prof_start();
        }
        if (t1 - window >= 5000) {
            droid_log("mortar: %.1f fps, step %.1f ms average, swap %.1f ms, worst frame %.0f ms, %d over 25 ms",
                      frames * 1000.0 / (t1 - window), step_total / frames, swap_total / frames, worst, slow);
            swap_total = worst = 0;
            slow = 0;
            arm_profile_dump(t1 - window, 10);
            void arm_translate_report(void); // arm_cpu.cpp: which threads translated new code (the stutters)
            arm_translate_report();
            if (droid_tool("ThreadScope")) arm_threads_report();
            window = t1;
            frames = 0;
            step_total = 0;
        }
    }
    return NULL;
}

void mortar_run(so_module *mod, const char *apk_path, mortar_poll_fn poll) {
    game = mod;
    poll_input = poll;
    mo.device_properties = native("InitDeviceProperties");
    mo.file_manager = native("InitFileManager");
    mo.system_init = native("SystemInit");
    mo.set_licensed = native("SetAppLicensed");
    mo.game_init = native("GameInit");
    mo.step = native("step");
    mo.touch = native("touchEvent");
    mo.key = native("keyEvent");
    mo.motion = native("motionEvent");
    mo.focus = native("onFocusRetrieved");
    mo.gles_version = native("GLESVersion");
    if (!mo.file_manager || !mo.system_init || !mo.game_init || !mo.step) return;
    env = arm_jni_env();
    cls = strdup("com/halfbrick/mortar/NativeGameLib");
    // the working folder Android gives an app: "/" (Mortar's file manager drops the paths' first
    // "/" and opens them relative: from the game's cache folder, ours, they were cache/mnt/...)
    void *linux_abi_resolve(const char *name);
    int (*cd)(const char *) = (int (*)(const char *))linux_abi_resolve("chdir");
    if (cd) cd("/");
    void arm_jit_bench(void); // arm_cpu.cpp: the code cache's memory types, measured
    if (droid_tool("JitBench")) arm_jit_bench();
    if (so_sym(mod, "JNI_OnLoad")) java_jni_onload(mod);

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20);
    pthread_t t, dog;
    pthread_create(&dog, NULL, watchdog, NULL);
    pthread_create(&t, &a, render_thread, strdup(apk_path));
    pthread_join(t, NULL);
}
