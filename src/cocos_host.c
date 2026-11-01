// cocos_host.c — the Java side of a cocos2d-x 2.x Android game, played by us: what
// org.cocos2dx.lib.Cocos2dxActivity / Cocos2dxHelper / Cocos2dxGLSurfaceView / Cocos2dxRenderer
// do on a phone. Cocos2dxHelper.init hands the engine the APK's path (it reads its assets from
// the zip itself), the GL view's render thread makes the context and calls nativeInit(w, h),
// then nativeRender every frame; touches go in as nativeTouches*, Back as nativeKeyDown(4).
// (Geometry Dash: RobTop's cocos2d-x 2.2 fork.)

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FalsoJNI.h"
#include "android_shim.h"
#include "keymap.h"
#include "arm_cpu.h"
#include "arm_modules.h"
#include "cocos_host.h"
#include "java_layer.h"
#include "native_activity.h"

#define HELPER "Java_org_cocos2dx_lib_Cocos2dxHelper_"
#define RENDERER "Java_org_cocos2dx_lib_Cocos2dxRenderer_"
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
    void (*set_apk_path)(void *, void *, void *);
    void (*init)(void *, void *, int32_t, int32_t);
    void (*render)(void *, void *);
    void (*surface_changed)(void *, void *, int32_t, int32_t);
    void (*on_resume)(void *, void *);
    void (*touch_begin)(void *, void *, int32_t, float, float);
    void (*touch_end)(void *, void *, int32_t, float, float);
    void (*touch_move)(void *, void *, void *, void *, void *);
    uint8_t (*key_down)(void *, void *, int32_t);
    // nativeKeyboardEvent(keyCode, isDown, isRepeat, timestamp s, shift, ctrl, alt)
    void (*keyboard_event)(void *, void *, int32_t, uint8_t, uint8_t, double, uint8_t, uint8_t, uint8_t);
} cc;

static void *env, *renderer;
static so_module *game;
static cocos_poll_fn poll_input;

int cocos_detect(so_module *mod) { return so_sym(mod, RENDERER "nativeRender") != NULL; }

static void *native(const char *prefix, const char *name) {
    char full[128];
    snprintf(full, sizeof(full), "%s%s", prefix, name);
    void *p = so_sym(game, full);
    if (!p) droid_log("cocos: %s not found", full);
    return p ? arm_callback((uint64_t)p) : NULL;
}

// ---- touches: the touchpad (2 fingers), the cursor's tap, the jump tap -------------------
// pointer ids: 0-1 the touchpad's fingers, 2 the cursor (Cross), 3 the jump (R1 / R2 / Square)
static struct { int down; float x, y; } finger[4 + KEYMAP_FINGERS]; // 4..: the keymap's (keymap.c)

static void finger_set(int id, int down, float x, float y) {
    if (down && !finger[id].down) {
        cc.touch_begin(env, renderer, id, x, y);
    } else if (down && (x != finger[id].x || y != finger[id].y) && cc.touch_move) {
        jintArray ids = jni->NewIntArray(&jni, 1);
        jfloatArray xs = jni->NewFloatArray(&jni, 1), ys = jni->NewFloatArray(&jni, 1);
        jint i = id;
        jni->SetIntArrayRegion(&jni, ids, 0, 1, &i);
        jni->SetFloatArrayRegion(&jni, xs, 0, 1, &x);
        jni->SetFloatArrayRegion(&jni, ys, 0, 1, &y);
        cc.touch_move(env, renderer, ids, xs, ys);
        jni->DeleteLocalRef(&jni, ids);
        jni->DeleteLocalRef(&jni, xs);
        jni->DeleteLocalRef(&jni, ys);
    } else if (!down && finger[id].down) {
        cc.touch_end(env, renderer, id, x, y);
    }
    finger[id].down = down;
    finger[id].x = x;
    finger[id].y = y;
}

// the cursor: the left stick glides it (drawn by the GL overlay), Cross taps there
extern float droid_cursor_x, droid_cursor_y; // unity_host.c
extern volatile int droid_cursor_shown;
static float stick_x, stick_y;

static void cc_km_finger(int id, int down, float x, float y);
static void cursor_tick(double dt) {
    keymap_tick(cc_km_finger); // (a keymap's joystick, each frame)
    float m = sqrtf(stick_x * stick_x + stick_y * stick_y);
    if (m < 0.15f) return;
    if (m > 1) m = 1;
    float speed = 1400.0f * powf((m - 0.15f) / 0.85f, 1.6f) * (float)dt;
    droid_cursor_x += stick_x / m * speed;
    droid_cursor_y += stick_y / m * speed;
    if (droid_cursor_x < 0) droid_cursor_x = 0;
    if (droid_cursor_y < 0) droid_cursor_y = 0;
    if (droid_cursor_x > SCREEN_W - 1) droid_cursor_x = SCREEN_W - 1;
    if (droid_cursor_y > SCREEN_H - 1) droid_cursor_y = SCREEN_H - 1;
    droid_cursor_shown = 1;
    if (finger[2].down) finger_set(2, 1, droid_cursor_x, droid_cursor_y); // a drag (sliders)
}

static void cc_km_finger(int id, int down, float x, float y) { finger_set(4 + id, down, x, y); }

// ---- the game's own controller code (per game: PAD_KEYS_GAMES) ---------------------------
// Geometry Dash's C++ has a whole controller mode (button glyphs, menus by D-pad: its PC build's),
// but its Android Java never passes gamepad buttons on (Cocos2dxGLSurfaceView.onKeyDown sends
// isGamepadButton() keys to the system), its nativeKeyboardEvent turns only keyboard keys into its
// (Windows) key codes, and PlatformToolbox::isControllerConnected says no (controller_connected_patch).
// Here the pad's buttons go straight to CCKeyboardDispatcher::dispatchKeyboardMSG as its controller
// codes (CCKeyboardDispatcher::keyToString: Controller_A 1001 ... Controller_Right 1027); the left
// stick as the D-pad, the triggers as LT / RT. No cursor.
#define PAD_KEYS_GAMES "com.robtopx."
static struct {
    void *(*shared_director)(void);
    void *(*keyboard_dispatcher)(void *director);
    uint8_t (*dispatch)(void *dispatcher, int32_t key, uint8_t down, uint8_t repeat, double t);
} gd;
static int pad_keys_on(void) {
    static int on = -1;
    if (on < 0) {
        extern int droid_game_in(const char *list); // game_settings.c
        on = 0;
        if (droid_game_in(PAD_KEYS_GAMES)) {
            void *a = so_sym(game, "_ZN7cocos2d10CCDirector14sharedDirectorEv");
            void *b = so_sym(game, "_ZN7cocos2d10CCDirector21getKeyboardDispatcherEv");
            void *c = so_sym(game, "_ZN7cocos2d20CCKeyboardDispatcher19dispatchKeyboardMSGENS_12enumKeyCodesEbbd");
            if (a && b && c) {
                gd.shared_director = arm_callback((uint64_t)(uintptr_t)a);
                gd.keyboard_dispatcher = arm_callback((uint64_t)(uintptr_t)b);
                gd.dispatch = arm_callback((uint64_t)(uintptr_t)c);
                on = 1;
            }
            droid_log("cocos: the pad as the game's controller: %s", on ? "its controller codes (dispatchKeyboardMSG)" : "not found");
        }
    }
    return on;
}
// the pad's Android key code -> the game's controller code (0: none)
static int controller_code(int key) {
    switch (key) {
    case 96: return 1001;  // Cross: A
    case 97: return 1003;  // Circle: B
    case 100: return 1005; // Triangle: Y
    case 99: return 1007;  // Square: X
    case 108: return 1009; // Options: Start
    case 109: return 1011; // the touchpad's click: Back
    case 103: return 1013; // R1: RB
    case 102: return 1015; // L1: LB
    case 105: return 1017; // R2: RT
    case 104: return 1019; // L2: LT
    case 19: return 1021;  // D-pad up
    case 20: return 1023;  // down
    case 21: return 1025;  // left
    case 22: return 1027;  // right
    default: return 0;
    }
}
// Geometry Dash's own keybinds (GJKeybindManager::loadDefaults): jump = Space, W, controller A
// (1001), RB (1013); D-pad left / right move in platformer levels only; D-pad up is bound to
// nothing, so in a level the D-pad did nothing (the user, 2026-11-01). D-pad up is added to
// jump (action 10001) through the game's own bindKey, before the first key goes in (its saved
// binds are loaded by then); menus keep the D-pad as before.
static void gd_bind_dpad_up(void) {
    static int done;
    if (done) return;
    done = 1;
    void *s = so_sym(game, "_ZN16GJKeybindManager11sharedStateEv");
    void *b = so_sym(game, "_ZN16GJKeybindManager7bindKeyEN7cocos2d12enumKeyCodesEbbbi");
    if (!s || !b) {
        droid_log("cocos: GJKeybindManager not found: D-pad up not bound");
        return;
    }
    void *(*shared)(void) = arm_callback((uint64_t)(uintptr_t)s);
    void (*bind)(void *, int32_t, uint8_t, uint8_t, uint8_t, int32_t) = arm_callback((uint64_t)(uintptr_t)b);
    void *m = shared();
    if (m) bind(m, 1021, 0, 0, 0, 10001);
    droid_log("cocos: D-pad up (1021) bound to jump (10001) in the game's keybinds: %s", m ? "done" : "no manager");
}
static void pad_key(int key, int down) {
    static uint8_t held[256];
    if (key <= 0 || key > 255 || held[key] == down) return;
    held[key] = (uint8_t)down;
    gd_bind_dpad_up();
    const int code = controller_code(key);
    static int told;
    if (told++ < 24) droid_log("cocos: pad key %d %s -> controller code %d", key, down ? "down" : "up", code);
    if (!code) return;
    void *dispatcher = gd.keyboard_dispatcher(gd.shared_director());
    if (!dispatcher) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    gd.dispatch(dispatcher, code, (uint8_t)down, 0, t.tv_sec + t.tv_nsec / 1e9);
}
// a stick's axis as two keys (on past 0.5, off under 0.3)
static void axis_keys(float v, int neg, int pos) {
    static uint8_t on[256];
    for (int k = 0; k < 2; k++) {
        const int code = k ? pos : neg;
        const float a = k ? v : -v;
        if (!code) continue;
        if (!on[code] && a > 0.5f) on[code] = 1, pad_key(code, 1);
        else if (on[code] && a < 0.3f) on[code] = 0, pad_key(code, 0);
    }
}
static void pad_keys_input(const droid_input_event *e) {
    droid_cursor_shown = 0;
    if (e->type == 1) { // a button: its Android key code (Cross 96 A, Circle 97 B, Options 108 Start ...)
        pad_key(e->keycode, e->action == 0);
        return;
    }
    axis_keys(e->axes[0], 21, 22);          // left stick: D-pad left / right
    axis_keys(e->axes[1], 19, 20);          //             up / down
    axis_keys(e->axes[17] * 2 - 1, 0, 104); // L2
    axis_keys(e->axes[18] * 2 - 1, 0, 105); // R2
}

static void cocos_input(const droid_input_event *e) {
    if (pad_keys_on() && e->type != DROID_EVENT_TOUCH) { // (the touchpad stays the touchscreen)
        pad_keys_input(e);
        return;
    }
    keymap_sync(); // (the Quick menu's editor may have changed it)
    if (e->type == 1 && keymap_key(e->keycode, e->action == 0, cc_km_finger)) return; // a mapped button: its spot
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
        case 96: case 109: { // Cross, the touchpad's click: a tap at the cursor (either holds it)
            static int held[2];
            held[e->keycode == 109] = down;
            droid_cursor_shown = 1;
            finger_set(2, held[0] || held[1], droid_cursor_x, droid_cursor_y);
            break;
        }
        case 99: case 102: case 103: case 105: case 19: // Square, L1, R1, R2, D-pad up: the jump
            finger_set(3, down, SCREEN_W * 0.5f, SCREEN_H * 0.6f);
            break;
        case 97: case 108: // Circle, Options: Back (the pause menu in a level)
            if (down && cc.key_down) cc.key_down(env, renderer, 4);
            break;
        }
        return;
    }
    keymap_stick(e->axes[0], e->axes[1]); // a keymap's joysticks: the left stick's, the right one's
    keymap_stick2(e->axes[11], e->axes[14]);
    keymap_triggers(e->axes[17], e->axes[18], cc_km_finger); // L2 / R2 on their spots, if mapped
    // the cursor: a stick no joystick took
    const int use_right = keymap_has_stick() && !keymap_has_stick2(), none = keymap_has_stick() && keymap_has_stick2();
    stick_x = none ? 0 : use_right ? e->axes[11] : e->axes[0];
    stick_y = none ? 0 : use_right ? e->axes[14] : e->axes[1];
    // R2 as an axis too (the triggers come as axes here): the jump - only when R2 itself changes
    // (a stick's move comes with R2 at 0: it let go of a jump held on Square)
    static int r2_was;
    int r2 = !keymap_has_key(KEY_R2) && e->axes[18] > 0.5f; // (R2 mapped: its spot, not the jump)
    if (r2 != r2_was) {
        r2_was = r2;
        finger_set(3, r2, SCREEN_W * 0.5f, SCREEN_H * 0.6f);
    }
}

// ---- the render thread (Cocos2dxGLSurfaceView's) -----------------------------------------

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

extern double cocos_anim_interval; // java_layer.c: Cocos2dxRenderer.setAnimationInterval (1/60)
void thread_place_role(int role, const char *why);
void arm_want_big_stack(void);

int droid_tool(const char *name); // game_settings.c
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
            droid_log("cocos: frame %llu has not come back for 5 s: what every thread is doing",
                      (unsigned long long)done + 1);
            arm_threads_report();
        } else if (droid_tool("ThreadScope")) { // (the debug tool: every 5 s anyway)
            droid_log("cocos: ThreadScope: what every thread is doing");
            arm_threads_report();
        }
        last = done;
    }
    return NULL;
}

static void *render_thread(void *arg) {
    (void)arg;
    arm_want_big_stack();
    thread_place_role(0, "cocos2d-x"); // every frame on this thread: the main core
    const double start = now_ms();
    // EGL as Cocos2dxGLSurfaceView asks it: GLES 2, RGB565 or better, depth 16 (24 here), stencil 8
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
    // a GLSurfaceView swaps once a refresh (interval 1): without it 160 fps on the PC
    unsigned (*swap_interval)(void *, int) = (unsigned (*)(void *, int))droid_resolve("eglSwapInterval");
    if (swap_interval) swap_interval(dpy, 1);
    droid_log("cocos: EGL surface %p context %p current %u", surface, ctx, cur);

    // Cocos2dxRenderer.onSurfaceCreated: nativeInit(w, h); onSurfaceChanged
    droid_log("cocos: nativeInit 1920x1080");
    cc.init(env, renderer, 1920, 1080);
    if (cc.surface_changed) cc.surface_changed(env, renderer, 1920, 1080);
    if (cc.on_resume) cc.on_resume(env, renderer);
    droid_log("cocos: running (%.1f s after start)", (now_ms() - start) / 1000);

    double last = now_ms(), window = last, render_total = 0;
    int frames = 0;
    for (uint64_t frame = 0;; frame++) {
        const double t0 = now_ms();
        poll_input(cocos_input);
        cursor_tick((t0 - last) / 1000.0);
        last = t0;
        frames_begun++;
        cc.render(env, renderer);
        frames_done++;
        const double t1 = now_ms();
        swap(dpy, surface);
        // the frame time the game asked (the swap may not wait for the refresh: 160 fps on the PC)
        const double spent = now_ms() - t0;
        const double want = cocos_anim_interval * 1000 - 0.25; // what the game asked (setAnimationInterval)
        if (spent < want) {
            struct timespec w = { 0, (long)((want - spent) * 1e6) };
            nanosleep(&w, NULL);
        }
        render_total += t1 - t0;
        frames++;
        if (frame == 0) {
            droid_log("cocos: first frame %.1f s after start", (t1 - start) / 1000);
            void arm_prof_start(void); // arm_cpu.cpp: ArmProf, when tools.txt has it
            arm_prof_start();
        }
        if (t1 - window >= 5000) {
            droid_log("cocos: %.1f fps, frame %.1f ms average", frames * 1000.0 / (t1 - window), render_total / frames);
            arm_profile_dump(t1 - window, 10);
            window = t1;
            frames = 0;
            render_total = 0;
        }
    }
    return NULL;
}

// Geometry Dash on Android: PlatformToolbox::isControllerConnected is "mov w0, #0; ret" (its PC build's
// controller mode never on). With the pad as its controller (PAD_KEYS_GAMES): "mov w0, #1; ret".
static void controller_connected_patch(void) {
    extern int droid_game_in(const char *list); // game_settings.c
    if (!droid_game_in(PAD_KEYS_GAMES)) return;
    uint32_t *f = (uint32_t *)so_sym(game, "_ZN15PlatformToolbox21isControllerConnectedEv");
    if (!f || f[0] != 0x52800000 || f[1] != 0xd65f03c0) {
        droid_log("cocos: isControllerConnected not as expected (%p): left as it is", (void *)f);
        return;
    }
    f[0] = 0x52800020; // mov w0, #1
    void arm_code_changed(uint64_t addr, uint64_t len); // arm_cpu.cpp: its translation dropped
    arm_code_changed((uint64_t)(uintptr_t)f, 8);
    droid_log("cocos: isControllerConnected answers yes (the game's controller mode)");
}

void cocos_run(so_module *mod, const char *apk_path, cocos_poll_fn poll) {
    game = mod;
    poll_input = poll;
    controller_connected_patch();
    cc.set_apk_path = native(HELPER, "nativeSetApkPath");
    cc.init = native(RENDERER, "nativeInit");
    cc.render = native(RENDERER, "nativeRender");
    cc.surface_changed = native(RENDERER, "nativeOnSurfaceChanged");
    cc.on_resume = native(RENDERER, "nativeOnResume");
    cc.touch_begin = native(RENDERER, "nativeTouchesBegin");
    cc.touch_end = native(RENDERER, "nativeTouchesEnd");
    cc.touch_move = native(RENDERER, "nativeTouchesMove");
    cc.key_down = native(RENDERER, "nativeKeyDown");
    cc.keyboard_event = native(RENDERER, "nativeKeyboardEvent");
    if (!cc.init || !cc.render || !cc.touch_begin || !cc.touch_end) return;
    env = arm_jni_env();
    renderer = strdup("org/cocos2dx/lib/Cocos2dxRenderer");
    // System.loadLibrary("fmod") comes first in RobTop's activity (FMOD keeps its JavaVM there;
    // without it FMOD never set up any sound: Geometry Dash was silent), then "cocos2dcpp"
    so_module *fmod = arm_module_load("libfmod.so");
    if (fmod && so_sym(fmod, "JNI_OnLoad")) {
        droid_log("cocos: libfmod.so JNI_OnLoad (System.loadLibrary(\"fmod\"))");
        java_jni_onload(fmod);
    }
    if (so_sym(mod, "JNI_OnLoad")) java_jni_onload(mod);
    // Cocos2dxHelper.init: nativeSetApkPath(the APK file the assets are read from)
    if (cc.set_apk_path) {
        droid_log("cocos: APK path %s", apk_path);
        cc.set_apk_path(env, strdup("org/cocos2dx/lib/Cocos2dxHelper"), jni->NewStringUTF(&jni, apk_path));
    }

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20); // Mesa compiles shaders on it
    pthread_t t, dog;
    pthread_create(&dog, NULL, watchdog, NULL);
    pthread_create(&t, &a, render_thread, NULL);
    pthread_join(t, NULL);
}
