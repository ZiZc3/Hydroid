#ifndef ANDROID_SHIM_H
#define ANDROID_SHIM_H

#include <stdint.h>

// The title's folder: /app0 on the PS5; the PC test runner sets its own.
#ifndef APP0
#define APP0 "/app0"
#endif

#define DROID_AXES 48

// Our AInputEvent: the game only sees it through the AInputEvent_* calls.
typedef struct {
    int32_t type;    // AINPUT_EVENT_TYPE_KEY 1, _MOTION 2
    int32_t source;  // AINPUT_SOURCE_GAMEPAD / _JOYSTICK
    int32_t action;  // AKEY_EVENT_ACTION_DOWN 0 / _UP 1, AMOTION_EVENT_ACTION_MOVE 2
    int32_t keycode;
    float axes[DROID_AXES];
} droid_input_event;

// type: DualSense touchpad fingers; axes[0] count, then x, y (0..1), id per finger
#define DROID_EVENT_TOUCH 100

void *droid_resolve(const char *name);
char *droid_strcasestr(const char *h, const char *n); // bionic_shim.c (the console's libc has none)
void droid_log(const char *fmt, ...);
void droid_log_flush(void); // app_main.c: the lines so far into android.log now
// The DualSense's motion as a phone's sensors (sensors.c): Android's frame (a phone in landscape,
// the screen towards you), m/s2 and rad/s; app_main.c's pad_poll puts the newest
typedef struct { float accel[3], gyro[3]; int64_t ns; } droid_motion_sample;
void droid_motion_put(const droid_motion_sample *m);
int droid_motion_wanted(void); // this game gets them (Controls > Motion)
// The Quick menu's pause (quick_menu.c): the game's sound keeps going (its engine never sees a
// stall), what goes to the speakers is silence. An audio loop calls this on each block it sends.
void droid_audio_mute_if_paused(int16_t *stereo, int frames);

// Behaviour only some games get (app_main.c sets it from the APK before its libraries load).
// Every other game keeps the old, working behaviour - a fix for one engine is gated to it
#define QUIRK_MIHOYO 1   // libyuanshen.so: Genshin Impact (Unity 2017 merged, Noelle GC, Combo SDK)
#define QUIRK_ROCKSTAR 2 // libGame.so: Rockstar's oswrapper ports (GTA San Andreas 2.11)
#define QUIRK_UNREAL 4   // libUE4.so / libUnreal.so: Unreal Engine (Wuthering Waves: packer, OBB, GameActivity)
extern int droid_quirks;

#include <stddef.h>
// An address for a new mapping, out of RADV's GPU window (linux_abi.c)
void *droid_va_hint(size_t bytes);

// app_main.c: the startup timeline ("startup: <s> <step>" from the game's pick to its first frame)
void droid_startup_begin(void);
void droid_startup(const char *fmt, ...);
void droid_startup_frame(const char *how);

#endif
