// unity_host.c — the Java side of a Unity Android player (UnityPlayerActivity /
// UnityPlayer), played by us in the order UnityPlayer runs it:
//   System.load(libmain) -> JNI_OnLoad; NativeLoader.load(libdir) (libmain
//   loads libunity + libil2cpp, they RegisterNatives); initJni(context);
//   then on the "UnityMain" thread: nativeRecreateGfxState(0, surface),
//   nativeSendSurfaceChangedEvent, nativeResume, nativeFocusChanged(true),
//   nativeRender() every frame (Unity makes its own EGL context inside).

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "FalsoJNI.h"
#include "game_settings.h"
#include "mapper.h"
#include "android_shim.h"
#include "keymap.h"
#include "arm_cpu.h"
#include "arm_modules.h"
#include "java_layer.h"
#include "jni_impl.h"
#include "unity_host.h"

static struct {
    jboolean (*load)(void *, void *, void *);
    // initJni(Context) - Unity 6: initJni(Context, int contextType), 0 an Activity, 1 a
    // GameActivity (only the Context passed, Unity 6 read garbage: "Unknown context type"
    // without end - Vector, Dead Trigger 2) - and some builds initJni(Context, int, String),
    // the process name (Shadow Fight 3: without it, a garbage "string" went into what Unity
    // builds from it - the PS5 then failed to decrypt its files, the PC crashed). All go
    // always: an older one doesn't read what it doesn't take.
    void (*init_jni)(void *, void *, void *, int, void *);
    void (*recreate_gfx)(void *, void *, jint, void *);
    void (*surface_changed)(void *, void *);
    void (*resume)(void *, void *);
    void (*focus)(void *, void *, jboolean);
    jboolean (*render)(void *, void *);
} u;

static void *env, *player, *activity, *surface;
static unity_poll_fn poll_input;

static void *native(const char *name) {
    void *p = jni_find_native(name);
    if (!p) droid_log("unity: native %s was not registered", name);
    return p;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// ---- the DualSense into Unity (nativeInjectEvent with our MotionEvent / KeyEvent) -----
// A touch game sees touches: the touchpad is the touchscreen, and the touch map puts
// the stick and buttons on the game's on-screen controls. A game that reads gamepads
// also gets the DualSense as one (device 2).

#define SCREEN_W ((float)droid_game_w) // the game's screen: 1080x1920 in the phone view
#define SCREEN_H ((float)droid_game_h)
#define SRC_TOUCH 0x1002
#define SRC_GAMEPAD 0x401
#define SRC_JOYSTICK 0x1000010

// The touch map: where the stick drags and each button taps (1920x1080). A file
// The game folder's touchmap.txt replaces it ("stick X Y RADIUS", "button KEYCODE X Y").
// Default: The Endless Tunnel - joystick bottom left, "open" bottom right, menu, help.
static struct { float x, y, r; } map_stick = { 339, 921, 110 };
static struct { int keycode; float x, y; } map_buttons[16] = {
    { 96, 1605, 915 },  // Cross    -> the "open" button
    { 108, 120, 90 },   // Options  -> menu
    { 100, 1785, 99 },  // Triangle -> help
    { 97, 1605, 915 },  // Circle   -> "open" too
};
static int map_button_count = 4;

static void load_touch_map(void) {
    FILE *touchmap_open_read(void); // game_settings.c: the game's, else the app's shipped one
    FILE *f = touchmap_open_read();
    if (!f) {
        // the built-in map is The Endless Tunnel's; other games read the pad themselves (a
        // D-pad press that also dragged the stick was two moves in Subway Surfers: a swipe)
        extern const char *java_package; // jni_impl.c
        if (strcmp(java_package, "com.KomoriGameDev.TheEndlessTunnel") != 0) {
            map_stick.r = 0;
            map_button_count = 0;
            droid_log("unity: touch map: none (the game reads the pad itself)");
            return;
        }
        droid_log("unity: touch map: built-in (stick %.0f,%.0f r %.0f, %d buttons)", map_stick.x, map_stick.y,
                  map_stick.r, map_button_count);
        return;
    }
    char line[128];
    int buttons = 0;
    map_stick.r = 0; // (the file says all of it: a joystick only if it has one)
    while (fgets(line, sizeof(line), f)) {
        float x, y, r;
        int k;
        if (sscanf(line, "stick %f %f %f", &x, &y, &r) == 3) {
            map_stick.x = x;
            map_stick.y = y;
            map_stick.r = r;
        } else if (sscanf(line, "button %d %f %f", &k, &x, &y) == 3 && buttons < 16) {
            map_buttons[buttons].keycode = k;
            map_buttons[buttons].x = x;
            map_buttons[buttons].y = y;
            buttons++;
        }
    }
    fclose(f);
    map_button_count = buttons;
    droid_log("unity: touch map from touchmap.txt (stick %.0f,%.0f r %.0f, %d buttons)", map_stick.x, map_stick.y,
              map_stick.r, map_button_count);
}

// Touch pointers: slot 0-1 touchpad fingers, 2 the stick, 3.. the buttons.
#define SLOTS 22 // (21: the right stick's joystick, keymap.c)
// Each finger's pointer id as a phone gives it: the lowest one free when it goes down (the first
// finger on the screen 0), not its slot (the cursor was 8: Dragon Ball Legends' menus only took
// finger 0 and ignored every tap on the console; the PC's mouse is slot 0)
static struct { int down; float x, y; int pid; } slot[SLOTS];
static int64_t touch_down_ns;

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

typedef jboolean (*inject_fn)(void *env, void *self, jobject event, jint display);
static inject_fn inject;

// Sent events are freed once 256 newer ones went (Unity reads one while it's injected;
// the ring keeps it alive well past that). Before: every event stayed forever.
static void send(jobject ev) {
    if (!inject) inject = (inject_fn)jni_find_native("nativeInjectEvent");
    static int logged;
    if (inject) {
        jboolean r = inject(env, player, ev, 0);
        if (logged++ < 10 || getenv("DROID_INPUT_LOG")) droid_log("unity: input event sent (%s)", r ? "taken" : "not taken");
    }
    static jobject ring[256];
    static unsigned at;
    if (ring[at % 256]) java_free_event(ring[at % 256]);
    ring[at % 256] = ev;
    at++;
}

// slot s goes down / up / moves to (x, y): one MotionEvent with every finger down
static void touch(int s, int down, float x, float y) {
    int was = slot[s].down;
    if (!was && !down) return;
    if (was && down && slot[s].x == x && slot[s].y == y) return;
    int64_t t = now_ns();
    int others = 0;
    for (int i = 0; i < SLOTS; i++) others += i != s && slot[i].down;
    if (!was && !others) touch_down_ns = t;
    if (!was) { // its id: the lowest one no other finger down has
        int pid = 0;
        for (int taken = 1; taken;) {
            taken = 0;
            for (int i = 0; i < SLOTS; i++)
                if (i != s && slot[i].down && slot[i].pid == pid) taken = 1;
            if (taken) pid++;
        }
        slot[s].pid = pid;
    }
    slot[s].x = x;
    slot[s].y = y;
    slot[s].down = 1; // in the event either way (an UP event still lists the finger)
    int ids[SLOTS], index = 0, n = 0;
    float xs[SLOTS], ys[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        if (!slot[i].down) continue;
        if (i == s) index = n;
        ids[n] = slot[i].pid;
        xs[n] = slot[i].x;
        ys[n] = slot[i].y;
        n++;
    }
    int action;
    if (!was) action = others ? 5 | (index << 8) : 0;       // POINTER_DOWN / DOWN
    else if (!down) action = others ? 6 | (index << 8) : 1; // POINTER_UP / UP
    else action = 2;                                         // MOVE
    static int taps_logged; // the first taps (downs and ups, not moves): where they went
    if (action != 2 && taps_logged++ < 40)
        droid_log("unity: tap %s, finger %d (pointer %d) at %.0f,%.0f (%d down, the game's screen %dx%d)",
                  (action & 0xff) == 0 || (action & 0xff) == 5 ? "down" : "up", s, slot[s].pid, x, y, n, droid_game_w, droid_game_h);
    send(java_motion_event(action, SRC_TOUCH, 1, n, ids, xs, ys, NULL, t, touch_down_ns));
    slot[s].down = down;
}

// ---- the cursor (as Ruffle Flash PS5 has it): the right stick glides it, R2 touches
// there (held: drags). Drawn by gles_bridge.c while it's in use.
#define CURSOR_SLOT 8
#define CURSOR_DEADZONE 0.14
#define CURSOR_CURVE 2.2
#define CURSOR_MAX_SPEED 1700.0 // px/s at full tilt
float droid_cursor_x = 960.0f, droid_cursor_y = 540.0f; // (inside either screen)
volatile int droid_cursor_shown;
static float cursor_sx, cursor_sy; // the right stick, -1..1
static int cursor_pressing;
static int64_t cursor_last_ns, cursor_used_ns;

// ---- the control mapper: the DualSense placed on the game's own on-screen controls ------
// Hold the touchpad click 1 s: the mapper opens over the running game (its markers and the
// cursor shown, nothing sent to the game). The cursor (either stick) on a game button, then
// a DualSense button: that button taps there (the same button again on its marker: gone).
// L3 on the game's joystick: the left stick and the D-pad drag it (D-pad up / down there:
// its reach). Hold the touchpad click again: saved as the game's touchmap.txt.
droid_marker droid_markers[DROID_MARKERS];
volatile int droid_marker_count;
volatile int droid_mapper_on;

static void key_colour(int keycode, float rgb[3]) {
    float r = 1, g = 1, b = 1; // shoulders, Options: white
    switch (keycode) {
    case 96: r = 0.35f, g = 0.55f, b = 1.0f; break; // Cross
    case 97: r = 1.0f, g = 0.3f, b = 0.35f; break;  // Circle
    case 100: r = 0.2f, g = 0.9f, b = 0.6f; break;  // Triangle
    case 99: r = 1.0f, g = 0.45f, b = 0.85f; break; // Square
    case 107: r = 0.7f, g = 0.7f, b = 0.7f; break;  // R3
    }
    rgb[0] = r, rgb[1] = g, rgb[2] = b;
}

static void markers_make(void) {
    int n = 0;
    for (int i = 0; i < map_button_count && n < DROID_MARKERS; i++) {
        droid_markers[n].x = map_buttons[i].x;
        droid_markers[n].y = map_buttons[i].y;
        droid_markers[n].size = 40;
        key_colour(map_buttons[i].keycode, droid_markers[n].rgb);
        n++;
    }
    if (map_stick.r > 0 && n + 5 <= DROID_MARKERS) { // the joystick: its centre, its reach
        static const float dx[5] = { 0, 1, -1, 0, 0 }, dy[5] = { 0, 0, 0, 1, -1 };
        for (int k = 0; k < 5; k++, n++) {
            droid_markers[n].x = map_stick.x + dx[k] * map_stick.r;
            droid_markers[n].y = map_stick.y + dy[k] * map_stick.r;
            droid_markers[n].size = k ? 18 : 44;
            droid_markers[n].rgb[0] = 1.0f, droid_markers[n].rgb[1] = 0.6f, droid_markers[n].rgb[2] = 0.1f;
        }
    }
    droid_marker_count = n;
}

static void touch_map_save(void) {
    char path[300];
    FILE *f = fopen(game_path(path, sizeof(path), "touchmap.txt"), "w");
    if (!f) {
        droid_log("mapper: touchmap.txt not written");
        return;
    }
    fprintf(f, "# the control mapper's (hold the touchpad click 1 s in the game to change it)\n");
    if (map_stick.r > 0) fprintf(f, "stick %.0f %.0f %.0f\n", map_stick.x, map_stick.y, map_stick.r);
    for (int i = 0; i < map_button_count; i++)
        fprintf(f, "button %d %.0f %.0f\n", map_buttons[i].keycode, map_buttons[i].x, map_buttons[i].y);
    fclose(f);
    droid_log("mapper: saved (stick %s, %d buttons)", map_stick.r > 0 ? "yes" : "no", map_button_count);
}

// a game reading the pad itself: pad mode or touch mode (unity_input, R3 switches)
static int touch_mode = 1; // the cursor first (a terms page needs a click); R3: pad mode
static int pad_only_game(void) { return map_stick.r <= 0 && map_button_count == 0; }
// Games with their own full controller mode (menus too): the pad from the start, no cursor of ours.
// Subway Surfers: its ControllerUI (a cursor of its own, Submit/Cancel), found when a pad is there.
#define PAD_FIRST_GAMES "com.kiloo.subwaysurf,com.yodo1.crossyroad" // Crossy Road: InControl (its Android TV / PS5 profiles)
// The same, but some of the game's screens don't take the pad (Shadow Fight 2: InControl + its
// GamePadController, but its Play Games "Warning" popup needs a tap): R3 brings our cursor
// (right stick, R2 / touchpad click taps); the D-pad goes back to the game's pad mode.
#define PAD_FIRST_CURSOR_GAMES "com.nekki.shadowfight"
// The cursor only: no pad mode (R3 / the D-pad don't switch to it). Temple Run 2: its Gamepad.cs
// mode ran the player on his own (user, 2026-11-01); played with the cursor, touchpad and tilt.
#define CURSOR_ONLY_GAMES "com.imangi.templerun2"

static void cursor_tick(void) {
    if (!droid_mapper_on && (!settings.cursor || (pad_only_game() && !touch_mode))) { // no cursor (settings, pad mode)
        droid_cursor_shown = 0;
        return;
    }
    int64_t t = now_ns();
    double dt = cursor_last_ns ? (t - cursor_last_ns) / 1e9 : 0;
    if (dt > 0.05) dt = 0.05;
    cursor_last_ns = t;
    double m = sqrt(cursor_sx * cursor_sx + cursor_sy * cursor_sy);
    if (m > 1) m = 1;
    if (m > CURSOR_DEADZONE) {
        double scaled = pow((m - CURSOR_DEADZONE) / (1 - CURSOR_DEADZONE), CURSOR_CURVE);
        double speed = CURSOR_MAX_SPEED * scaled * dt;
        droid_cursor_x += (float)(cursor_sx / m * speed);
        droid_cursor_y += (float)(cursor_sy / m * speed);
        if (droid_cursor_x < 0) droid_cursor_x = 0;
        if (droid_cursor_y < 0) droid_cursor_y = 0;
        if (droid_cursor_x > SCREEN_W - 1) droid_cursor_x = SCREEN_W - 1;
        if (droid_cursor_y > SCREEN_H - 1) droid_cursor_y = SCREEN_H - 1;
        cursor_used_ns = t;
        if (cursor_pressing) touch(CURSOR_SLOT, 1, droid_cursor_x, droid_cursor_y); // a drag
    }
    if (cursor_pressing) cursor_used_ns = t;
    droid_cursor_shown = droid_mapper_on || (cursor_used_ns && t - cursor_used_ns < 3000000000LL);
}

static int cursor_r2, cursor_click; // what holds the cursor's finger: R2, the touchpad's click
static void cursor_press(int down) {
    if (down == cursor_pressing) return;
    cursor_pressing = down;
    cursor_used_ns = now_ns();
    touch(CURSOR_SLOT, down, droid_cursor_x, droid_cursor_y);
}

// The D-pad on the on-screen joystick, as a thumb does it: down at the centre in one
// frame, then out to the edge over the next ones (a floating joystick places itself
// where the touch first lands, so a down and an edge at once only moved it).
static float dpad_x, dpad_y;
static float lstick_x, lstick_y; // the left stick, when the game has an on-screen joystick
static void dpad_tick(void) {
    static int frames_down;
    if (map_stick.r <= 0 || droid_mapper_on) return; // no on-screen stick (or the mapper is open)
    float sm = sqrtf(lstick_x * lstick_x + lstick_y * lstick_y);
    float dx = dpad_x, dy = dpad_y, reach_max = 1.0f;
    if (sm > 0.22f) { // the left stick: analog, as far out as it's tilted
        dx = lstick_x, dy = lstick_y;
        reach_max = sm > 1 ? 1 : sm;
    }
    if (!dx && !dy) {
        touch(2, 0, slot[2].x, slot[2].y);
        frames_down = 0;
        return;
    }
    if (!slot[2].down) {
        touch(2, 1, map_stick.x, map_stick.y);
        frames_down = 0;
        return;
    }
    frames_down++;
    float m = sqrtf(dx * dx + dy * dy);
    float reach = frames_down >= 3 ? reach_max : reach_max * frames_down / 3.0f; // 3 frames to the edge
    touch(2, 1, map_stick.x + dx / m * map_stick.r * reach, map_stick.y + dy / m * map_stick.r * reach);
}

// A game that reads the pad itself (no touch map: Among Us, with Rewired) sees one control
// at a time. With the pad's sticks both a gamepad and the cursor's touches, its HUD switched
// between controller and touch with every move (Among Us: "New controller connected" 19
// times, buttons gone, the X not answering). Pad mode: the gamepad only. Touch mode: the
// cursor only (stick + R2). R3 switches; the touchpad is the touchscreen in both.

// every finger lifted (the mapper opening / closing; no touch left held by mistake)
static void touches_release(void) {
    for (int i = 0; i < SLOTS; i++)
        if (slot[i].down) touch(i, 0, slot[i].x, slot[i].y);
    cursor_pressing = 0;
    dpad_x = dpad_y = lstick_x = lstick_y = 0;
}

static void mapper_toggle(void) {
    touches_release();
    if (!droid_mapper_on) {
        markers_make();
        droid_mapper_on = 1;
        droid_log("mapper: open (the cursor on a game button + a DualSense button; L3: the joystick)");
    } else {
        droid_mapper_on = 0;
        touch_map_save();
    }
}

static void mapper_button(int keycode) {
    float x = droid_cursor_x, y = droid_cursor_y;
    if (keycode == 106) { // L3: the joystick here (again on it: no joystick)
        float dx = x - map_stick.x, dy = y - map_stick.y;
        if (map_stick.r > 0 && dx * dx + dy * dy < 50 * 50) map_stick.r = 0;
        else {
            map_stick.x = x, map_stick.y = y;
            if (map_stick.r <= 0) map_stick.r = 120;
        }
    } else if ((keycode == 19 || keycode == 20) && map_stick.r > 0) { // D-pad up / down: its reach
        map_stick.r += keycode == 19 ? 15 : -15;
        if (map_stick.r < 40) map_stick.r = 40;
        if (map_stick.r > 400) map_stick.r = 400;
    } else if (keycode == 21 || keycode == 22) {
        return; // the D-pad drives the joystick
    } else {
        int at = -1;
        for (int i = 0; i < map_button_count; i++)
            if (map_buttons[i].keycode == keycode) at = i;
        if (at >= 0) {
            float dx = x - map_buttons[at].x, dy = y - map_buttons[at].y;
            if (dx * dx + dy * dy < 40 * 40) { // on its own marker: gone
                for (int i = at; i + 1 < map_button_count; i++) map_buttons[i] = map_buttons[i + 1];
                map_button_count--;
                droid_log("mapper: button %d removed", keycode);
                markers_make();
                return;
            }
        } else if (map_button_count < 16) {
            at = map_button_count++;
        } else {
            return;
        }
        map_buttons[at].keycode = keycode;
        map_buttons[at].x = x;
        map_buttons[at].y = y;
        droid_log("mapper: button %d at %.0f,%.0f", keycode, x, y);
    }
    markers_make();
}

// keymap.c's fingers here: its buttons on the slots of Unity's own map (the same file, the same order;
// L2 / R2 come through it), the right stick's joystick on slot 21 (its left one: Unity's own)
static void unity_km_finger(int id, int down, float x, float y) {
    if (id == KEYMAP_STICK2_ID) touch(21, down, x, y);
    else if (id >= 1) touch(id - 1 < 5 ? 3 + (id - 1) : 4 + (id - 1), down, x, y);
}

// Games with keyboard controls of their own (built for Google Play Games on PC): the DualSense as
// those keys, sent as a keyboard's (Unity's Input.GetKey). The touchpad stays the touchscreen,
// the right stick moves the cursor and the touchpad's click taps there (menus without keys).
// Vector (com.nekki.vector 2.10.2, read from its binding table KCEIJIDJAED..ctor):
//   W jump / up, S slide / down, A left, D right, Q trick, Esc back / pause, R restart,
//   Left Ctrl checkpoint, E confirm. Android key codes: W 51, S 47, A 29, D 32, Q 45, E 33,
//   R 46, Esc 111, Ctrl left 113.
#define PAD_KEYBOARD_GAMES "com.nekki.vector"
#define SRC_KEYBOARD 0x101
enum { KB_W, KB_S, KB_A, KB_D, KB_Q, KB_E, KB_R, KB_ESC, KB_CTRL, KB_COUNT };
static const int kb_code[KB_COUNT] = { 51, 47, 29, 32, 45, 33, 46, 111, 113 };
static unsigned kb_held[KB_COUNT]; // which inputs hold each key (bits)
static void kb_set(int k, unsigned bit, int down, int64_t t) {
    const unsigned was = kb_held[k];
    kb_held[k] = down ? was | bit : was & ~bit;
    if (!was == !kb_held[k]) return; // no change for the game
    send(java_key_event(kb_held[k] ? 0 : 1, kb_code[k], SRC_KEYBOARD, 1, 0, t, t));
}
// Slingshot games with no controller play of their own (Angry Birds 2: its gamepad code is a
// rumbler only; phones aim by touch, its Play Games PC build by mouse). The user's layout
// (2026-11-01): the right stick puts the cursor on the bird, the LEFT STICK grabs the bird at
// the cursor and pulls (direction and power), R2 lets go (the shot); the left stick back to the
// middle without R2 brings the finger back to the bird (the game cancels). Cross taps at the
// cursor (menus), Square taps the middle of the screen (a bird's power in flight), Circle /
// Options are Android's back. The touchpad stays the touchscreen, its click a tap at the cursor.
#define SLINGSHOT_GAMES "com.rovio.baba"
#define SLING_SLOT 20
static void sling_key(int keycode, int64_t t) { // a key pressed and let go (Android's back: 4)
    send(java_key_event(0, keycode, SRC_GAMEPAD, 2, 0, t, t));
    send(java_key_event(1, keycode, SRC_GAMEPAD, 2, 0, t, t));
}
static void slingshot_input(const droid_input_event *e, int64_t t) {
    static int told, grabbed, r2_down, square_down, need_center;
    static float gx, gy; // where the bird was grabbed
    if (!told++) {
        droid_cursor_x = SCREEN_W * 0.15f, droid_cursor_y = SCREEN_H * 0.62f; // (where the slingshot usually is)
        droid_log("unity: slingshot controls (SLINGSHOT_GAMES): left stick pulls, R2 shoots, right stick the cursor");
    }
    if (e->type == 1) {
        const int down = e->action == 0;
        switch (e->keycode) {
        case 96: cursor_press(down); break; // Cross: a tap at the cursor
        case 99: // Square: a tap in the middle (the bird's power)
            if (down != square_down) touch(SLING_SLOT - 1, down, SCREEN_W * 0.5f, SCREEN_H * 0.5f);
            square_down = down;
            break;
        case 97: case 108: if (down) sling_key(4, t); break; // Circle / Options: back
        default: break;
        }
        return;
    }
    const float lx = e->axes[0], ly = e->axes[1], r2 = e->axes[18];
    cursor_sx = e->axes[11]; // the right stick: the cursor
    cursor_sy = e->axes[14];
    const float m = sqrtf(lx * lx + ly * ly);
    const float reach = SCREEN_H * 0.30f; // a full pull
    if (need_center && m < 0.1f) need_center = 0; // (after a shot: the stick back in the middle first)
    if (!grabbed && !need_center && m > 0.2f && !r2_down) { // the left stick moved: the bird grabbed at the cursor
        grabbed = 1;
        gx = droid_cursor_x, gy = droid_cursor_y;
        touch(SLING_SLOT, 1, gx, gy);
    }
    if (grabbed) touch(SLING_SLOT, 1, gx + lx * reach, gy + ly * reach); // the pull follows the stick
    const int r2_now = r2 > 0.5f;
    if (r2_now && !r2_down && grabbed) { // R2: the shot
        touch(SLING_SLOT, 0, gx + lx * reach, gy + ly * reach);
        grabbed = 0;
        need_center = 1;
        static int shots;
        if (shots++ < 5) droid_log("unity: slingshot: shot at a pull of %.2f", m);
    }
    r2_down = r2_now;
    if (grabbed && m < 0.1f) { // the stick let go without R2: back on the bird, the finger up (cancelled)
        touch(SLING_SLOT, 0, gx, gy);
        grabbed = 0;
    }
}

static void pad_keyboard(const droid_input_event *e, int64_t t) {
    static int told;
    if (!told++) droid_log("unity: the pad as the game's keyboard keys (PAD_KEYBOARD_GAMES)");
    if (e->type == 1) {
        const int down = e->action == 0;
        switch (e->keycode) { // a button: its keys (bit 1 << button so two inputs can hold one key)
        case 19: kb_set(KB_W, 1u << 0, down, t); break;   // D-pad up: jump
        case 20: kb_set(KB_S, 1u << 1, down, t); break;   // D-pad down: slide
        case 21: kb_set(KB_A, 1u << 2, down, t); break;   // D-pad left
        case 22: kb_set(KB_D, 1u << 3, down, t); break;   // D-pad right
        case 96: kb_set(KB_W, 1u << 4, down, t); kb_set(KB_E, 1u << 4, down, t); break; // Cross: jump + confirm
        case 99: kb_set(KB_Q, 1u << 5, down, t); break;   // Square: trick
        case 103: kb_set(KB_Q, 1u << 6, down, t); break;  // R1: trick
        case 97: kb_set(KB_ESC, 1u << 7, down, t); break; // Circle: back
        case 108: kb_set(KB_ESC, 1u << 8, down, t); break; // Options: pause
        case 100: kb_set(KB_R, 1u << 9, down, t); break;  // Triangle: restart
        case 102: kb_set(KB_CTRL, 1u << 10, down, t); break; // L1: checkpoint
        default: break;
        }
        return;
    }
    // the left stick: W A S D past half way (bit 1 << 16); the right stick: the cursor
    const float lx = e->axes[0], ly = e->axes[1];
    kb_set(KB_W, 1u << 16, ly < -0.5f, t);
    kb_set(KB_S, 1u << 16, ly > 0.5f, t);
    kb_set(KB_A, 1u << 16, lx < -0.5f, t);
    kb_set(KB_D, 1u << 16, lx > 0.5f, t);
    cursor_sx = e->axes[11];
    cursor_sy = e->axes[14];
}

// Swipe runners with no controller play of their own (Sonic Dash, Sonic Boom: phones swipe up
// to jump, down to roll, left / right to change lanes, tap for the rest). The DualSense:
// D-pad / left stick flick = a swipe that way; Cross = up (jump), Square = down (roll),
// L1 / R1 = left / right; Triangle = a tap in the middle of the screen; Circle / Options =
// Android's back. Menus: the right stick moves the cursor, R2 taps there; the touchpad stays
// the touchscreen (its click a tap at the cursor).
#define SWIPE_GAMES "com.sega.sonicdash,com.sega.sonicboomandroid"
#define SWIPE_SLOT 18
#define SWIPE_FRAMES 4 // down, 3 moves out, up
static int swipe_dir = -1, swipe_frame; // 0 up 1 down 2 left 3 right
static int swipe_queued = -1;
static void swipe_start(int dir) {
    if (swipe_dir >= 0) { swipe_queued = dir; return; } // one at a time (a quick second one waits)
    swipe_dir = dir, swipe_frame = 0;
    static int told;
    static const char *names[] = { "up", "down", "left", "right" };
    if (told++ < 8) droid_log("unity: swipe %s (SWIPE_GAMES)", names[dir]);
}
static void swipe_tick(void) {
    if (swipe_dir < 0) return;
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.6f, len = SCREEN_W * 0.30f;
    const float dx = swipe_dir == 2 ? -1.f : swipe_dir == 3 ? 1.f : 0.f;
    const float dy = swipe_dir == 0 ? -1.f : swipe_dir == 1 ? 1.f : 0.f;
    if (swipe_frame < SWIPE_FRAMES) {
        const float k = (float)swipe_frame / (SWIPE_FRAMES - 1);
        touch(SWIPE_SLOT, 1, cx + dx * len * k, cy + dy * len * k);
        swipe_frame++;
    } else {
        touch(SWIPE_SLOT, 0, cx + dx * len, cy + dy * len);
        swipe_dir = swipe_queued, swipe_frame = 0, swipe_queued = -1;
    }
}
static void swipe_input(const droid_input_event *e, int64_t t) {
    static int told, r2_down, tri_down, flicked;
    if (!told++) droid_log("unity: swipe controls (SWIPE_GAMES): D-pad / left stick / Cross / Square swipe, R2 taps at the cursor");
    if (e->type == 1) {
        const int down = e->action == 0;
        switch (e->keycode) {
        case 19: case 96: if (down) swipe_start(0); break;  // D-pad up / Cross: jump
        case 20: case 99: if (down) swipe_start(1); break;  // D-pad down / Square: roll
        case 21: case 102: if (down) swipe_start(2); break; // D-pad left / L1
        case 22: case 103: if (down) swipe_start(3); break; // D-pad right / R1
        case 100: // Triangle: a tap in the middle
            if (down != tri_down) touch(SWIPE_SLOT + 1, down, SCREEN_W * 0.5f, SCREEN_H * 0.5f);
            tri_down = down;
            break;
        case 97: case 108: if (down) sling_key(4, t); break; // Circle / Options: back
        default: break;
        }
        return;
    }
    const float lx = e->axes[0], ly = e->axes[1], r2 = e->axes[18];
    const float m = sqrtf(lx * lx + ly * ly);
    if (flicked && m < 0.3f) flicked = 0; // back in the middle: the next flick
    if (!flicked && m > 0.6f) {
        flicked = 1;
        swipe_start(fabsf(lx) > fabsf(ly) ? (lx < 0 ? 2 : 3) : (ly < 0 ? 0 : 1));
    }
    cursor_sx = e->axes[11]; // the right stick: the cursor
    cursor_sy = e->axes[14];
    const int r2_now = r2 > 0.5f;
    if (r2_now != r2_down) {
        cursor_r2 = r2_now;
        cursor_press(cursor_r2 || cursor_click);
    }
    r2_down = r2_now;
}

static void unity_input(const droid_input_event *e) {
    static int ready;
    static int map_version;
    if (ready && map_version != droid_keymap_version) { // the Quick menu's editor saved: its map now
        map_version = droid_keymap_version;
        for (int s = 2; s < SLOTS; s++) touch(s, 0, slot[s].x, slot[s].y); // (the old one's fingers up)
        load_touch_map();
    }
    if (!ready) {
        ready = 1;
        map_version = droid_keymap_version;
        load_touch_map();
        if (pad_only_game() && droid_game_in(PAD_FIRST_CURSOR_GAMES)) {
            touch_mode = 0; // the game's own controller mode first; R3: our cursor (its popups)
            droid_log("unity: the game's own controller mode first (R3: the cursor for popups) (PAD_FIRST_CURSOR_GAMES)");
        } else if (pad_only_game() && droid_game_in(PAD_FIRST_GAMES)) {
            touch_mode = 0;
            settings.cursor = 0; // (no R3 to a cursor either: the game's own controller mode does it all)
            droid_log("unity: the game's own controller mode: pad only, no cursor (PAD_FIRST_GAMES)");
        } else if (pad_only_game() && settings.cursor) {
            droid_log("unity: the game reads the pad: touch mode (R3: pad mode, the gamepad only)");
        }
        if (touch_mode && settings.cursor && !droid_mapper_on)
            cursor_used_ns = now_ns(); // the cursor shown at the start (then hidden 3 s after its last use)
    }
    int split = pad_only_game() && settings.cursor;
    int64_t t = now_ns();
    if (e->type == DROID_EVENT_TOUCH) { // the touchpad is the touchscreen
        int n = (int)e->axes[0];
        for (int f = 0; f < 2; f++) {
            if (f < n) touch(f, 1, e->axes[1 + f * 3] * SCREEN_W, e->axes[2 + f * 3] * SCREEN_H);
            else touch(f, 0, slot[f].x, slot[f].y);
        }
        return;
    }
    if (e->type == 1 && e->keycode == 109 && settings.cursor && !droid_mapper_on &&
        !(split && !touch_mode)) { // the touchpad's click: a tap at the cursor (two quick ones: a double tap)
        cursor_click = e->action == 0;
        cursor_press(cursor_click || cursor_r2);
        return;
    }
    if (e->type == 1 && e->keycode == 109) { // the touchpad click (no cursor): held 1 s, the mapper
        static int64_t down_at;
        if (e->action == 0) {
            down_at = t;
            return; // its own press waits: a short one goes to the game on release
        }
        extern int droid_single_game; // app_main.c: a one-game app has no mapper
        if (down_at && t - down_at >= 800000000LL && !droid_single_game) {
            mapper_toggle();
        } else if (!droid_mapper_on) {
            send(java_key_event(0, 109, SRC_GAMEPAD, 2, 0, down_at, down_at));
            send(java_key_event(1, 109, SRC_GAMEPAD, 2, 0, down_at, t));
        }
        down_at = 0;
        return;
    }
    if (droid_mapper_on) { // the mapper: nothing to the game
        if (e->type == 1) {
            if (e->action == 0) mapper_button(e->keycode);
            return;
        }
        float lx = e->axes[0], ly = e->axes[1], rx = e->axes[11], ry = e->axes[14];
        int left_used = lx * lx + ly * ly >= rx * rx + ry * ry;
        cursor_sx = left_used ? lx : rx;
        cursor_sy = left_used ? ly : ry;
        return;
    }
    if (droid_game_in(SLINGSHOT_GAMES)) { // slingshot play on the DualSense (no controller play of its own)
        slingshot_input(e, t);
        return;
    }
    if (droid_game_in(SWIPE_GAMES)) { // swipe runners: the pad as swipes
        swipe_input(e, t);
        return;
    }
    if (droid_game_in(PAD_KEYBOARD_GAMES)) { // the game's own keyboard controls (its PC build's)
        pad_keyboard(e, t);
        return;
    }
    if (e->type == 1) { // a button: the gamepad key, and its tap on the touch map
        if (split && touch_mode && droid_game_in(CURSOR_ONLY_GAMES)) return; // (no pad mode: no gamepad keys)
        if (split && e->keycode == 107) { // R3: pad mode <-> touch mode
            if (e->action == 0) {
                touch_mode = !touch_mode;
                if (!touch_mode) cursor_press(0);
                cursor_sx = cursor_sy = 0;
                droid_log("unity: %s", touch_mode ? "touch mode: the cursor (R3: back to the pad)" : "pad mode");
            }
            return;
        }
        // the D-pad in touch mode (the cursor never uses it): pad mode, the press goes on to the
        // game (Subway Surfers: steered by the D-pad, nothing reached it until R3)
        if (split && touch_mode && e->action == 0 && e->keycode >= 19 && e->keycode <= 22) {
            touch_mode = 0;
            cursor_press(0);
            cursor_sx = cursor_sy = 0;
            droid_log("unity: pad mode (the D-pad; R3: back to the cursor)");
        }
        if (split && touch_mode) return; // the cursor's turn: no gamepad buttons
        send(java_key_event(e->action, e->keycode, SRC_GAMEPAD, 2, 0, t, t));
        // the D-pad pushes the on-screen joystick (8 directions)
        static int up, down, left, right;
        int *d = e->keycode == 19 ? &up : e->keycode == 20 ? &down : e->keycode == 21 ? &left
                 : e->keycode == 22 ? &right : NULL;
        if (d && map_stick.r > 0) { // where the thumb should go; dpad_tick moves it there over a few frames
            *d = e->action == 0;
            dpad_x = (float)(right - left);
            dpad_y = (float)(down - up);
            return;
        }
        for (int i = 0; i < map_button_count; i++)
            if (map_buttons[i].keycode == e->keycode)
                touch(i < 5 ? 3 + i : 4 + i, e->action == 0, map_buttons[i].x, map_buttons[i].y); // (slot 8 is the cursor's)
        return;
    }
    // sticks: the gamepad's axes; with an on-screen joystick the left stick drags it and the
    // right one glides the cursor, else either stick glides the cursor (the left one first)
    if (!(split && touch_mode)) send(java_motion_event(2, SRC_JOYSTICK, 2, 1, NULL, NULL, NULL, e->axes, t, t));
    if (split && !touch_mode) { // the pad's turn: no cursor
        cursor_sx = cursor_sy = 0;
        return;
    }
    float lx = e->axes[0], ly = e->axes[1], rx = e->axes[11], ry = e->axes[14];
    if (map_stick.r > 0) {
        lstick_x = lx, lstick_y = ly;
        cursor_sx = rx, cursor_sy = ry;
    } else {
        int left_used = lx * lx + ly * ly >= rx * rx + ry * ry;
        cursor_sx = left_used ? lx : rx;
        cursor_sy = left_used ? ly : ry;
    }
    keymap_sync();
    keymap_stick2(rx, ry); // the right stick's joystick and L2 / R2's spots (keymap.c: touchmap.txt's)
    keymap_triggers(e->axes[17], e->axes[18], unity_km_finger);
    if (keymap_has_stick2()) cursor_sx = map_stick.r > 0 ? 0 : lx, cursor_sy = map_stick.r > 0 ? 0 : ly;
    if (settings.cursor) { // R2: touch at the cursor
        cursor_r2 = !keymap_has_key(KEY_R2) && e->axes[18] > 0.5f; // (R2 mapped: its spot)
        cursor_press(cursor_r2 || cursor_click);
    }
}

// ---- 60 fps for a game that didn't choose: Unity's targetFrameRate -1 means 30 on a
// phone. Set through IL2CPP's own icall lookup, on Unity's main thread, only while the
// game leaves it at -1. The game's Options (unity_fps30): the game's own, or always 30 or
// 60 - set again when the game sets its own (looked at every second). A game too slow for 60
// gets an even 30 from the pacing (vulkan_bridge.c).
static void unity_fps_target(uint64_t frame) {
    static uint64_t get_fn, set_fn;
    static int tried, mode = -1, told;
    if (mode < 0) {
        mode = settings.unity_fps30;
        if (mode == UNITY_FPS_GAME) droid_log("unity: the game's settings keep its own frame rate");
    }
    if (mode == UNITY_FPS_GAME || frame < 120 || frame % 60 != 0) return;
    if (!tried) {
        tried = 1;
        so_module *il2cpp = NULL;
        for (int i = 0; i < arm_module_count(); i++) {
            const char *p = arm_module_path(i);
            if (p && strstr(p, "libil2cpp.so")) il2cpp = arm_module(i);
        }
        uint64_t resolve = il2cpp ? (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_resolve_icall") : 0;
        if (resolve) {
            int ok = 0;
            uint64_t a = (uint64_t)(uintptr_t)"UnityEngine.Application::get_targetFrameRate()";
            get_fn = arm_call(resolve, &a, 1, &ok);
            a = (uint64_t)(uintptr_t)"UnityEngine.Application::set_targetFrameRate(System.Int32)";
            set_fn = arm_call(resolve, &a, 1, &ok);
        }
        droid_log("unity: targetFrameRate icalls: get %#llx, set %#llx", (unsigned long long)get_fn,
                  (unsigned long long)set_fn);
    }
    if (!get_fn || !set_fn) return;
    int ok = 0;
    int now = (int)arm_call(get_fn, NULL, 0, &ok);
    if (mode == UNITY_FPS_30 || mode == UNITY_FPS_60) {
        uint64_t rate = mode == UNITY_FPS_30 ? 30 : 60;
        if (now != (int)rate) {
            arm_call(set_fn, &rate, 1, &ok);
            if (told++ < 6) droid_log("unity: the game asked %d fps: set to %d (its Options)", now, (int)rate);
        }
    } else if (now == -1) {
        extern int droid_vblank_hz; // the screen's (60 or 120)
        uint64_t rate = droid_vblank_hz > 0 ? (uint64_t)droid_vblank_hz : 60;
        arm_call(set_fn, &rate, 1, &ok);
        droid_log("unity: the game left its frame rate to Unity (30): set to %d", (int)rate);
    }
}

// GC report (per game, read-only): Crossy Road's menus stop for 55-190 ms per collection. Its
// il2cpp GC's mode (incremental?), time slice, heap size and use, next to the GC line.
#define GC_REPORT_GAMES "com.yodo1.crossyroad"
static void gc_heap_report(void) {
    static int state; // 0 not set up, 1 on, -1 off
    static uint64_t is_inc, slice, heap, used;
    if (state < 0) return;
    if (!state) {
        state = -1;
        if (!droid_game_in(GC_REPORT_GAMES)) return;
        so_module *il2cpp = NULL;
        for (int i = 0; i < arm_module_count(); i++) {
            const char *p = arm_module_path(i);
            if (p && strstr(p, "libil2cpp.so")) il2cpp = arm_module(i);
        }
        if (!il2cpp) return;
        is_inc = (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_gc_is_incremental");
        slice = (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_gc_get_max_time_slice_ns");
        heap = (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_gc_get_heap_size");
        used = (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_gc_get_used_size");
        if (!is_inc || !slice || !heap || !used) return;
        state = 1;
    }
    int ok = 0;
    const int inc = (int)(arm_call(is_inc, NULL, 0, &ok) & 0xff);
    const long long ns = (long long)arm_call(slice, NULL, 0, &ok);
    const unsigned long long h = arm_call(heap, NULL, 0, &ok), u = arm_call(used, NULL, 0, &ok);
    droid_log("unity: GC heap %llu MB, used %llu MB, incremental %s, time slice %.1f ms", h >> 20, u >> 20,
              inc ? "on" : "off", ns / 1e6);
}

// A game object hidden by name (per game): Temple Run 1's "Global Challenges CTA" - an invisible
// layer its menu shows over PLAY when its Global Challenges have no live dates (on a phone they come
// from Remote Config; every live one has ended, so the phone shows none). Deactivated whenever the
// menu turns it on (looked for twice a second: GameObject.Find sees active objects only).
extern const char *java_package; // jni_impl.c
static void unity_hide_objects(uint64_t frame) {
    static const char *name;
    static int state; // 0 not set up, 1 on, -1 off
    static uint64_t find_fn, set_active_fn, new_string;
    static int hidden;
    if (state < 0 || frame < 60 || frame % 30 != 0) return;
    if (!state) {
        state = -1;
        if (!java_package || strcmp(java_package, "com.imangi.templerun") != 0) return;
        name = "Global Challenges CTA";
        so_module *il2cpp = NULL;
        for (int i = 0; i < arm_module_count(); i++) {
            const char *p = arm_module_path(i);
            if (p && strstr(p, "libil2cpp.so")) il2cpp = arm_module(i);
        }
        uint64_t resolve = il2cpp ? (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_resolve_icall") : 0;
        new_string = il2cpp ? (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_string_new") : 0;
        if (!resolve || !new_string) return;
        int ok = 0;
        uint64_t a = (uint64_t)(uintptr_t)"UnityEngine.GameObject::Find(System.String)";
        find_fn = arm_call(resolve, &a, 1, &ok);
        a = (uint64_t)(uintptr_t)"UnityEngine.GameObject::SetActive(System.Boolean)";
        set_active_fn = arm_call(resolve, &a, 1, &ok);
        droid_log("unity: hiding \"%s\" (icalls: find %#llx, SetActive %#llx)", name, (unsigned long long)find_fn,
                  (unsigned long long)set_active_fn);
        if (!find_fn || !set_active_fn) return;
        state = 1;
    }
    int ok = 0;
    uint64_t a = (uint64_t)(uintptr_t)name;
    const uint64_t str = arm_call(new_string, &a, 1, &ok);
    if (!ok || !str) return;
    const uint64_t obj = arm_call(find_fn, &str, 1, &ok);
    if (!ok || !obj) return;
    const uint64_t args[2] = { obj, 0 };
    arm_call(set_active_fn, args, 2, &ok);
    if (hidden++ < 5) droid_log("unity: \"%s\" hidden (it blocked the menu)", name);
}

// Unity's stack trace for each log line: off (Application.SetStackTraceLogType(type, None)).
// Each trace runs Unity's native unwinder, which reads /proc/self/maps and opens every
// library again: Among Us logs a line every frame ("Waiting for time stamp") - 400 maps and
// 700 library opens a second, slow files on the PS5 (7 fps). The game's debug log option
// keeps them (the traces then show in our log).
// Unity 2019 builds can strip the Set icall (the game never calls it) but keep Get:
// "b impl" -> impl = adrp x8, table; add x8, x8, :lo12:table; ldr w0, [x8, w0, uxtw #2]; ret.
// Writing that table is what Set does. Vector 2 only so far: its unwinder (libunwindstack)
// crashed re-reading /proc/self/maps for a log line's trace.
#define STACK_TABLE_GAMES "com.nekki.vector2"
static int *stack_type_table(uint64_t get) {
    if (!get) return NULL;
    const uint32_t *p = (const uint32_t *)(uintptr_t)get;
    if ((p[0] & 0xfc000000) == 0x14000000) { // b
        int64_t off = (int64_t)(p[0] & 0x3ffffff) << 2;
        if (off & (1 << 27)) off -= 1 << 28;
        p = (const uint32_t *)((uintptr_t)p + off);
    }
    if ((p[0] & 0x9f00001f) != 0x90000008 || (p[1] & 0xffc003ff) != 0x91000108 || p[2] != 0xb8605900 ||
        p[3] != 0xd65f03c0)
        return NULL;
    int64_t imm = ((p[0] >> 29) & 3) | (((p[0] >> 5) & 0x7ffff) << 2);
    if (imm & (1 << 20)) imm -= 1 << 21;
    const uintptr_t page = ((uintptr_t)p & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
    return (int *)(page + ((p[1] >> 10) & 0xfff));
}

static void unity_stack_traces_off(void) {
    static int told, missing;
    if (missing || settings.debug_log) return;
    so_module *il2cpp = NULL;
    for (int i = 0; i < arm_module_count(); i++) {
        const char *p = arm_module_path(i);
        if (p && strstr(p, "libil2cpp.so")) il2cpp = arm_module(i);
    }
    uint64_t resolve = il2cpp ? (uint64_t)(uintptr_t)so_sym(il2cpp, "il2cpp_resolve_icall") : 0;
    if (!resolve) return;
    int ok = 0;
    uint64_t a = (uint64_t)(uintptr_t)"UnityEngine.Application::SetStackTraceLogType(UnityEngine.LogType,UnityEngine.StackTraceLogType)";
    uint64_t set = arm_call(resolve, &a, 1, &ok);
    if (!set && droid_game_in(STACK_TABLE_GAMES)) {
        uint64_t g = (uint64_t)(uintptr_t)"UnityEngine.Application::GetStackTraceLogType";
        int *table = stack_type_table(arm_call(resolve, &g, 1, &ok));
        if (table) {
            for (int type = 0; type <= 4; type++) table[type] = 0; // StackTraceLogType.None
            missing = 1;
            droid_log("unity: stack traces of log lines off (Set stripped: Get's table at %p)", (void *)table);
            return;
        }
    }
    if (!set) {
        missing = 1;
        droid_log("unity: no SetStackTraceLogType: log lines keep their stack traces");
        return;
    }
    for (uint64_t type = 0; type <= 4; type++) { // Error, Assert, Warning, Log, Exception
        uint64_t args[2] = { type, 0 };         // StackTraceLogType.None
        arm_call(set, args, 2, &ok);
    }
    if (!told++) droid_log("unity: stack traces of log lines off (each one re-read every library: slow)");
}

int sceKernelUsleep(uint32_t us);

// Frames finished so far; the watchdog reports every thread when it stalls.
static volatile uint64_t frames_done;

// Every 50 ms: a frame late by 300 ms is a hitch - what every thread is doing then is
// logged (the first 10 hitches), and a long stall as before (5 s, then every 30 s).
static void *watchdog(void *arg) {
    (void)arg;
    uint64_t seen = 0;
    int ticks = 0, hitch_logged = 0, hitches = 0, runs = 0;
    for (;;) {
        sceKernelUsleep(50000);
        uint64_t now = frames_done;
        // every 20 s for the first 5 minutes: what every thread is doing, even while frames come
        // (a game stuck behind its loading screen still draws it)
        if (++runs % 400 == 0 && runs <= 6000) {
            droid_log("unity: %d s in (%llu frames): what every thread is doing:", runs / 20, (unsigned long long)now);
            arm_threads_report();
            void java_busy_report(void); // java_layer.c: a thread stuck in one of our Java answers
            java_busy_report();
        }
        if (now != seen) {
            seen = now;
            ticks = 0;
            hitch_logged = 0;
            continue;
        }
        ticks++;
        if (ticks == 6 && !hitch_logged && now > 30 && hitches < 10) { // 300 ms
            hitch_logged = 1;
            hitches++;
            droid_log("unity: hitch %d - no frame for 300 ms (after %llu frames): what every thread is doing:",
                      hitches, (unsigned long long)now);
            arm_threads_report();
        }
        int stalled = ticks / 20; // seconds
        if (ticks % 20 == 0 && (stalled == 5 || (stalled > 5 && stalled % 30 == 0))) {
            droid_log("unity: no frame for %d s (after %llu frames): what every thread is doing:", stalled,
                      (unsigned long long)now);
            arm_threads_report();
            if (stalled == 5) {
                void arm_host_backtraces(void); // arm_cpu.cpp (the PC runner: host call stacks)
                arm_host_backtraces();
            }
        }
    }
    return NULL;
}

void thread_place_role(int role, const char *why);
void big_report(void);
int video_player_active(void);                          // video_player.c
void video_player_input(const droid_input_event *e);
int fs_slow_report(long long *ms); // linux_abi.c: slow file calls
static void *unity_main(void *arg) {
    (void)arg;
    pthread_t w;
    pthread_create(&w, NULL, watchdog, NULL);
    void arm_want_big_stack(void); // arm_cpu.cpp: an 8 MB ARM stack, as a phone's main thread
    arm_want_big_stack();
    thread_place_role(0, "UnityMain"); // its own core (thread_place.c)
    droid_log("unity: UnityMain thread: surface, resume, focus");
    if (u.recreate_gfx) u.recreate_gfx(env, player, 0, surface);
    if (u.surface_changed) u.surface_changed(env, player);
    if (u.resume) u.resume(env, player);
    if (u.focus) u.focus(env, player, JNI_TRUE);
    droid_log("unity: rendering");
    double start = now_ms(), window = start, last = start, slowest = 0;
    int frames = 0, hitches = 0, over34 = 0;
    // the frame's real work: nativeRender's time less its waits (the frame pacer's)
    double work_sum = 0, work_max = 0;
    for (uint64_t frame = 0;; frame++) {
        // a full-screen movie (video_player.c): Unity paused while it plays, as Android's
        // UnityPlayer.pause(); the pad and touches go to it (skip)
        while (video_player_active()) {
            poll_input(video_player_input);
            sceKernelUsleep(16000);
            last = now_ms(); // (not a hitch)
        }
        poll_input(unity_input);
        cursor_tick();
        swipe_tick();
        dpad_tick();
        keymap_tick(unity_km_finger); // (the right stick's joystick)
        unity_fps_target(frame);
        unity_hide_objects(frame);
        if (frame >= 1 && frame % 600 == 1) unity_stack_traces_off(); // again later: a game may set its own
        // DROID_TEST_INPUT=1 (PC tests): the stick right for 2 s from frame 400, then Cross
        static int test_input = -1;
        if (test_input < 0) test_input = getenv("DROID_TEST_INPUT") != NULL;
        if (test_input && frame >= 400 && frame <= 460) {
            droid_input_event te;
            memset(&te, 0, sizeof(te));
            te.type = 1; // the D-pad right, held
            te.keycode = 22;
            te.action = frame < 460 ? 0 : 1;
            if (frame == 400 || frame == 460) unity_input(&te);
        }
        if (test_input && frame >= 500 && frame <= 560) { // the cursor: right stick, then R2
            droid_input_event te;
            memset(&te, 0, sizeof(te));
            te.type = 2;
            te.axes[0] = frame < 530 ? 1.0f : 0.0f;
            te.axes[18] = frame >= 540 && frame < 546 ? 1.0f : 0.0f;
            unity_input(&te);
            if (frame == 531) droid_log("unity: test cursor at %.0f,%.0f", droid_cursor_x, droid_cursor_y);
        }
        if (test_input && (frame == 470 || frame == 476)) {
            droid_input_event te;
            memset(&te, 0, sizeof(te));
            te.type = 1;
            te.keycode = 96;
            te.action = frame == 470 ? 0 : 1;
            unity_input(&te);
        }
        uint64_t c0 = __builtin_ia32_rdtsc(), b0 = arm_thread_blocked_cycles();
        double r0 = now_ms();
        jboolean rendered = u.render(env, player);
        double r_ms = now_ms() - r0;
        uint64_t dc = __builtin_ia32_rdtsc() - c0, db = arm_thread_blocked_cycles() - b0;
        double work = dc ? r_ms * (double)(dc > db ? dc - db : 0) / (double)dc : r_ms;
        work_sum += work;
        if (work > work_max) work_max = work;
        if (!rendered) {
            droid_log("unity: nativeRender returned false (quit) after %llu frames",
                      (unsigned long long)frame);
            return NULL;
        }
        frames++;
        frames_done++;
        double t = now_ms();
        if (frame == 0) {
            droid_log("unity: first frame %.1f s after start", (t - start) / 1000);
            void arm_bench_calls(void); // arm_imports.c: what a call out of ARM code costs here
            arm_bench_calls();
            void arm_prof_start(void); // arm_cpu.cpp: ArmProf, when tools.txt has it
            arm_prof_start();
        }
        // frame times: the slowest one, and how many were visible hitches
        double dt = t - last;
        last = t;
        if (frame > 0) {
            if (dt > slowest) slowest = dt;
            if (dt > 50) hitches++;
            if (dt > 34) over34++;
        }
        if (t - window >= 5000) {
            uint64_t arm_translated_take(void), arm_exclusive_take(void); // arm_cpu.cpp
            uint64_t arm_gc_stops_take(double *slowest_ms);
            uint64_t arm_gc_collections_take(double *stopped_ms, double *longest_ms);
            double gc_stopped = 0, gc_longest = 0;
            unsigned long long gc_runs = (unsigned long long)arm_gc_collections_take(&gc_stopped, &gc_longest);
            void arm_translate_report(void);
            double gc_slowest = 0;
            unsigned long long gc_stops = (unsigned long long)arm_gc_stops_take(&gc_slowest);
            droid_log("unity: %.1f fps, slowest frame %.0f ms, %d frames over 34 ms, %d over 50 ms (hitches); "
                      "work per frame %.1f ms average, %.1f ms most; %llu K new ARM instructions translated; "
                      "%llu K atomics called out; GC %llu collections, game stopped %.0f ms (longest %.0f ms), %llu thread stops "
                      "(slowest thread %.0f ms)",
                      frames * 1000.0 / (t - window), slowest, over34, hitches, frames ? work_sum / frames : 0,
                      work_max, (unsigned long long)(arm_translated_take() / 1000),
                      (unsigned long long)(arm_exclusive_take() / 1000), gc_runs, gc_stopped, gc_longest, gc_stops, gc_slowest);
            if (gc_runs) { // (its own line: the one above is cut at the log's line length)
                droid_log("unity: GC %llu collections in 5 s, the game stopped %.0f ms (longest %.0f ms), "
                          "%llu thread stops (slowest %.0f ms)", gc_runs, gc_stopped, gc_longest, gc_stops, gc_slowest);
                gc_heap_report();
            }
            arm_translate_report();
            big_report(); // big_files.c: files moved to the fast disk
            long long slow_ms;
            int slow_n = fs_slow_report(&slow_ms); // linux_abi.c: file calls of 100 ms and more
            if (slow_n) droid_log("files: %d slow file call(s), %lld ms waited in all", slow_n, slow_ms);
            work_sum = work_max = 0;
            slowest = 0;
            hitches = over34 = 0;
            arm_profile_dump(t - window, 10);
            window = t;
            frames = 0;
        }
    }
}

// merged: Unity's player built into the game's own library (Genshin Impact: libyuanshen.so,
// libunity and il2cpp in one, no libmain): its JNI_OnLoad registers UnityPlayer's natives
static int unity_start(so_module *merged, const char *lib_dir, unity_poll_fn poll) {
    poll_input = poll;
    env = arm_jni_env();
    activity = strdup("com/unity3d/player/UnityPlayerActivity");
    player = strdup("com/unity3d/player/UnityPlayer");
    surface = strdup("android/view/Surface");

    so_module *libmain = merged;
    if (merged) { // System.loadLibrary of it: its JNI_OnLoad registers NativeLoader (libmain's part)
        droid_log("unity: Unity's player in the game's own library (%zu MB, no libmain / libunity)", merged->size >> 20);
    } else { // System.load(nativeLibraryDir + "/libmain.so")
        libmain = arm_module_load("libmain.so");
        if (!libmain) return -1;
    }
    if (!so_sym(libmain, "JNI_OnLoad")) return -2;
    java_jni_onload(libmain); // once: System.load of it later doesn't run it again
    if (merged) // Java-loaded libraries (System.loadLibrary on a phone) whose JNI_OnLoad keeps the
        // JavaVM: Genshin Impact's voice chat, libgmesdk ("_javaVM =NULL"). Not every one: the
        // HoYo network SDK's never returned this early
        for (int i = 0; i < arm_module_count(); i++)
            if (arm_module(i) != merged && strstr(arm_module_path(i), "libgmesdk.so") &&
                so_sym(arm_module(i), "JNI_OnLoad")) {
                droid_log("unity: JNI_OnLoad of %s", arm_module_path(i));
                java_jni_onload(arm_module(i));
            }

    // NativeLoader.load(nativeLibraryDir): libmain loads libunity (+ il2cpp); a merged library
    // has them already, its load starts them
    u.load = native("load");
    if (!u.load) return -3;
    void *loader_class = strdup("com/unity3d/player/NativeLoader");
    jboolean loaded = u.load(env, loader_class, jni->NewStringUTF(&jni, lib_dir));
    // (a stopped JIT returns 0 here: the BAD access report is above in the log)
    droid_log("unity: NativeLoader.load = %d", loaded);
    if (!loaded) return -4;

    u.init_jni = native("initJni");
    u.recreate_gfx = native("nativeRecreateGfxState");
    u.surface_changed = native("nativeSendSurfaceChangedEvent");
    u.resume = native("nativeResume");
    u.focus = native("nativeFocusChanged");
    u.render = native("nativeRender");
    if (!u.init_jni || !u.render) return -5;

    droid_log("unity: initJni");
    extern int fjni_trace;
    const char *trace = getenv("DROID_JNI_TRACE"); // PC runner: trace this many calls from here on
    fjni_trace = trace ? atoi(trace) : 3000; // every JNI call Unity makes here goes to the log
    extern const char *java_package; // jni_impl.c: the process name is the package's
    void *process = jni->NewStringUTF(&jni, java_package);
    u.init_jni(env, player, activity, 0, process); // 0: an Activity (UnityPlayerActivity)
    if (!trace) fjni_trace = 0;
    droid_log("unity: initJni done");

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20); // Mesa shader compiles happen on it
    pthread_t t;
    pthread_create(&t, &a, unity_main, NULL);
    pthread_join(t, NULL);
    return 0;
}

int unity_run(const char *lib_dir, unity_poll_fn poll) { return unity_start(NULL, lib_dir, poll); }
int unity_run_merged(so_module *m, const char *lib_dir, unity_poll_fn poll) { return unity_start(m, lib_dir, poll); }

// a library with Unity's player inside: JNI_OnLoad and UnityPlayer's class name in it
int unity_is_merged(so_module *m) {
    if (!m || !so_sym(m, "JNI_OnLoad") || m->size < (32u << 20)) return 0;
    static const char want[] = "com/unity3d/player/UnityPlayer";
    const char *p = m->base, *end = (const char *)m->base + m->size - (sizeof(want) - 1); // (no memmem here)
    while (p < end && (p = memchr(p, 'c', (size_t)(end - p)))) {
        if (!memcmp(p, want, sizeof(want) - 1)) return 1;
        p++;
    }
    return 0;
}

