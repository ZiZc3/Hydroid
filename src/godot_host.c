// godot_host.c — the Java side of a Godot 3 Android export, played by us:
// what org.godotengine.godot.Godot / GodotView / GodotLib do on a phone.
// The engine's natives (Java_org_godotengine_godot_GodotLib_*) are called on
// one thread that owns the GL context, as GLSurfaceView's render thread does:
// initialize, setup, newcontext, resize, then step() every frame.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "FalsoJNI.h"
#include "game_settings.h"
#include "android_shim.h"
#include "arm_cpu.h"
#include "godot_host.h"
#include "native_activity.h"

#define LIB "Java_org_godotengine_godot_GodotLib_"

typedef void *(*egl_get_display_fn)(void *);
typedef unsigned (*egl_initialize_fn)(void *, int *, int *);
typedef unsigned (*egl_choose_config_fn)(void *, const int *, void **, int, int *);
typedef void *(*egl_create_surface_fn)(void *, void *, void *, const int *);
typedef void *(*egl_create_context_fn)(void *, void *, void *, const int *);
typedef unsigned (*egl_make_current_fn)(void *, void *, void *, void *);
typedef unsigned (*egl_swap_fn)(void *, void *);

static struct {
    void (*initialize)(void *, void *, void *, void *, void *, uint8_t);
    void (*setup)(void *, void *, void *);
    void (*resize)(void *, void *, int32_t, int32_t);
    void (*newcontext)(void *, void *, uint8_t);
    void (*step)(void *, void *);
    void (*focusin)(void *, void *);
    void (*key)(void *, void *, int32_t, int32_t, uint8_t);
    void (*joybutton)(void *, void *, int32_t, int32_t, uint8_t);
    void (*joyaxis)(void *, void *, int32_t, int32_t, float);
    void (*joyconnection)(void *, void *, int32_t, uint8_t, void *);
} lib;

static void *env, *clazz;
static godot_poll_fn poll_input;
static so_module *game;

int godot_detect(so_module *mod) { return so_sym(mod, LIB "step") != NULL; }

static void *native(const char *name) {
    char full[128];
    snprintf(full, sizeof(full), LIB "%s", name);
    void *p = so_sym(game, full);
    if (!p) droid_log("godot: %s not found", full);
    return p && game->arm64 ? arm_callback((uint64_t)p) : p;
}

// ---- DualSense -> Godot ------------------------------------------------------------

// Android keycode -> Godot joypad button (SDL order, as Godot's Java input handler maps it)
static int joy_button(int keycode) {
    switch (keycode) {
    case 96: return 0;   // A / Cross
    case 97: return 1;   // B / Circle
    case 99: return 2;   // X / Square
    case 100: return 3;  // Y / Triangle
    case 102: return 4;  // L1
    case 103: return 5;  // R1
    case 106: return 8;  // L3
    case 107: return 9;  // R3
    case 109: return 10; // select (touchpad)
    case 108: return 11; // start (Options)
    case 19: return 12;  // D-pad up
    case 20: return 13;
    case 21: return 14;
    case 22: return 15;
    }
    return -1;
}

static float last_axis[8];

static void axis(int index, float v) {
    if (v == last_axis[index]) return;
    last_axis[index] = v;
    lib.joyaxis(env, clazz, 0, index, v);
}

static void godot_input(const droid_input_event *e) {
    if (e->type == 1) { // key
        uint8_t down = e->action == 0;
        int b = joy_button(e->keycode);
        if (b >= 0) lib.joybutton(env, clazz, 0, b, down);
        // keyboard too: arrows for the D-pad, Enter for Cross. (Not Android's
        // Back key for Options: Godot quits the game on Back.)
        int key = e->keycode >= 19 && e->keycode <= 22 ? e->keycode : e->keycode == 96 ? 66 : 0;
        if (key) lib.key(env, clazz, key, key == 66 ? '\n' : 0, down);
        return;
    }
    axis(0, e->axes[0]);   // left stick
    axis(1, e->axes[1]);
    axis(2, e->axes[11]);  // right stick
    axis(3, e->axes[14]);
    axis(6, e->axes[17]);  // L2
    axis(7, e->axes[18]);  // R2
}

// ---- the render thread -----------------------------------------------------------

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static double start_ms;

void thread_place_role(int role, const char *why);
static void *render_thread(void *arg) {
    (void)arg;
    void arm_want_big_stack(void);
    arm_want_big_stack();
    thread_place_role(0, "Godot"); // Godot's frames, all on this thread: the main core
    // EGL, as GodotView's config chooser asks for it (GLES3, RGB888, depth 24, stencil 8)
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
    const int attribs[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3025, 24, 0x3026, 8,
                            0x3040, 0x40, 0x3038 };
    void *config = NULL;
    int n = 0;
    choose(dpy, attribs, &config, 1, &n);
    void *surface = create_surface(dpy, config, activity_window(), NULL);
    const int ctx_attribs[] = { 0x3098, 3, 0x3038 };
    void *ctx = create_context(dpy, config, NULL, ctx_attribs);
    unsigned cur = make_current(dpy, surface, surface, ctx);
    droid_log("godot: EGL display %p config %p surface %p context %p current %u", dpy, config,
              surface, ctx, cur);

    lib.newcontext(env, clazz, 1);
    lib.resize(env, clazz, 1920, 1080);
    double window_start = now_ms(), step_total = 0;
    arm_profile_dump(0, 0); // start the profile window
    int window_frames = 0;
    for (uint64_t frame = 0;; frame++) {
        if (frame == 2) {
            // Main::start ran in step 1: the input singleton exists now.
            lib.joyconnection(env, clazz, 0, 1, jni->NewStringUTF(&jni, "PS5 DualSense"));
            lib.focusin(env, clazz);
            droid_log("godot: main loop running");
        }
        if (frame >= 2) poll_input(godot_input);
        double t0 = now_ms();
        lib.step(env, clazz);
        double t1 = now_ms();
        swap(dpy, surface);
        step_total += t1 - t0;
        window_frames++;
        double t2 = now_ms();
        if (frame == 2) droid_log("godot: first game frame %.1f s after start", (t2 - start_ms) / 1000);
        if (t2 - window_start >= 5000) {
            droid_log("godot: %.1f fps, step %.1f ms avg (game logic + GL calls in ARM code)",
                      window_frames * 1000.0 / (t2 - window_start), step_total / window_frames);
            arm_profile_dump(t2 - window_start, 10);
            window_start = t2;
            step_total = 0;
            window_frames = 0;
        }
        if (frame == 0) droid_log("godot: step 0 done (Main::setup2)");
        if (frame == 1) droid_log("godot: step 1 done (Main::start)");
    }
    return NULL;
}

void godot_run(so_module *mod, godot_poll_fn poll) {
    game = mod;
    poll_input = poll;
    start_ms = now_ms();
    lib.initialize = native("initialize");
    lib.setup = native("setup");
    lib.resize = native("resize");
    lib.newcontext = native("newcontext");
    lib.step = native("step");
    lib.focusin = native("focusin");
    lib.key = native("key");
    lib.joybutton = native("joybutton");
    lib.joyaxis = native("joyaxis");
    lib.joyconnection = native("joyconnectionchanged");
    if (!lib.initialize || !lib.setup || !lib.step) return;

    env = mod->arm64 ? arm_jni_env() : (void *)&jni;
    clazz = (void *)strdup("org/godotengine/godot/GodotLib");
    void *activity = strdup("android/app/Activity");
    void *godot = strdup("org/godotengine/godot/Godot");
    void *assets = strdup("android/content/res/AssetManager");
    char godot_dir[300];
    mkdir(data_path(godot_dir, sizeof(godot_dir), "godot"), 0777);

    // Godot.onCreate -> GodotLib.initialize + setup (no command line)
    droid_log("godot: initialize");
    lib.initialize(env, clazz, activity, godot, assets, 0);
    droid_log("godot: setup");
    lib.setup(env, clazz, NULL);
    droid_log("godot: setup done, starting the render thread");

    // Mesa compiles shaders on this thread: give it a big host stack.
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20);
    pthread_t t;
    pthread_create(&t, &a, render_thread, NULL);
    pthread_join(t, NULL);
}
