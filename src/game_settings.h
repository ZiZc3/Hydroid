#ifndef GAME_SETTINGS_H
#define GAME_SETTINGS_H

#include <stddef.h>

// The game being run: its own folder (its libraries, saves, caches) and its settings,
// (game_settings.c). Without a game folder: /app0/data.

#define RENDERER_AUTO 0   // Vulkan when the game has it, else GLES
#define RENDERER_VULKAN 1
#define RENDERER_GLES 2
#define RENDERER_ANGLE 3  // OpenGL ES on ANGLE (on Vulkan), not ps5-opengl

#define UNITY_FPS_AUTO 0
#define UNITY_FPS_GAME 1
#define UNITY_FPS_30 2
#define UNITY_FPS_60 3

typedef struct {
    int renderer;     // RENDERER_*
    int refresh_hz;   // 60, 120, or 0: auto (Vulkan 120 Hz, OpenGL ES 60)
    int fps_counter;  // the stats overlay: 0 off, 1 the frame rate, 2 the full panel (overlay_stats.h)
    int cursor;       // the controller's cursor (left / right stick, R2)
    int unity_fps30;  // Unity's frame rate: 0 auto (60 where the game leaves it), 1 the game's
                      // own, 2 always 30, 3 always 60 (UNITY_FPS_*)
    int hide_astc;    // hide ASTC from a GLES game (Unity unpacks it itself)
    int debug_log;    // the game's debug log lines too
    int offline;      // no internet for the game (it hears "no network" at once)
    int cores;        // the main and render threads on cores of their own (thread_place.c)
    int bigfiles;     // files written past 2 MB moved to /app0/data (big_files.c); 0: they stay
    int writers;      // write-behind threads taking files: 1 (one stream to the disk) or 2
    int exact_atomics; // the ARM exclusive monitor kept exact (slower atomics): miHoYo's Noelle GC needs
                       // it (Genshin Impact); on by itself for libyuanshen.so
    int motion;        // the DualSense's motion as the phone's sensors (tilt, shake): 0 auto (the games
                       // known to use it, sensors.c), 1 on, 2 off
    int view;         // VIEW_*: the screen the game is given (Auto: portrait for portrait-only games)
} game_settings;

#define VIEW_AUTO 0
#define VIEW_WIDE 1  // the TV's 1920x1080
#define VIEW_PHONE 2 // a phone's 1080x1920, shown in the middle of the TV

// The screen the game is told it has: 1920x1080, or 1080x1920 in the phone view (view_decide)
extern int droid_game_w, droid_game_h;
static inline int phone_view(void) { return droid_game_h > droid_game_w; }
// Where the game's picture goes on a sw x sh screen: its full height, in the middle
static inline void view_rect(int sw, int sh, int *x, int *y, int *w, int *h) {
    int pw = (int)((long)sh * droid_game_w / droid_game_h);
    if (pw > sw) pw = sw;
    *x = (sw - pw) / 2, *y = 0, *w = pw, *h = sh;
}
// A point of the game's screen on the TV's 1920x1080 (the same point outside the phone view)
static inline void view_to_tv(float gx, float gy, float *tx, float *ty) {
    if (!phone_view()) { *tx = gx, *ty = gy; return; }
    int x, y, w, h;
    view_rect(1920, 1080, &x, &y, &w, &h);
    *tx = x + gx * w / droid_game_w, *ty = y + gy * h / droid_game_h;
}
// ...and back: a point of the TV on the game's screen (the Keymap Editor: placed on the TV's picture)
static inline void view_from_tv(float tx, float ty, float *gx, float *gy) {
    if (!phone_view()) { *gx = tx, *gy = ty; return; }
    int x, y, w, h;
    view_rect(1920, 1080, &x, &y, &w, &h);
    *gx = (tx - x) * droid_game_w / w, *gy = (ty - y) * droid_game_h / h;
}
// The view, by hand for each game (app_main.c's PHONE_VIEW_GAMES, before the game starts): the
// portrait games tested in the phone view. Every other game keeps the TV's 1920x1080
void view_decide(const char *apk, const unsigned char *manifest, size_t manifest_len);

extern game_settings settings;        // the running game's
// the game draws with OpenGL ES (ps5-opengl or ANGLE), not Vulkan, even when it has Vulkan
static inline int renderer_gles(void) { return settings.renderer == RENDERER_GLES || settings.renderer == RENDERER_ANGLE; }
extern char game_dir[256];
// A fix for some games only (every other game keeps what worked for it): the running game's
// package starts with one of the list's ("com.imangi.,com.halfbrick.jetpackjoyride")
int droid_game_in(const char *list);            // its folder, no trailing '/'

// "<game_dir>/<rel>" into out
const char *game_path(char *out, size_t n, const char *rel);
// the game's own files (files/, cache/, prefs, downloads): the fast folder when there's room
extern char game_data_dir[300];
const char *data_path(char *out, size_t n, const char *rel);
void game_data_prepare(void); // at its launch, after game_settings_load: chooses, moves once
void fast_data_path(const char *dir, char *out, size_t n);

// game_dir = dir (made if missing), settings from dir/settings.txt (defaults if none;
// the old switch files there - vulkan, hz120, fps30, noastc, debug-log - still count)
void game_settings_load(const char *dir);
int game_settings_save(void); // into game_dir/settings.txt; 0 ok
// the game's own data in dir removed, as Android's "Clear storage": its saves, downloads and
// caches (everything: the unpacked APKs too); our Options and the game's notes stay
long game_clear(const char *dir, int everything);
// The game gone from the PS5: its file (games/ or game.*), its folder, its Vulkan copy. 0: done
int game_delete(const char *file, const char *dir);
// Its saves backed up / restored (game_backup.c): Hydroid-Backups/<game> on a USB drive, else on
// the console's /data. The saves' size (0: none); a backup there (when: its date); 0 done
long long game_saves_bytes(const char *dir);
int game_backup_info(const char *dir, char *when, size_t wn);
int game_backup(const char *dir);
int game_restore(const char *dir);

// The debug tools switched on (their names, a line each, "#" a comment): tools.txt in the
// game's folder, APP0/data/tools.txt for every game, or DROID_TOOLS="NetSpy JavaMap" (PC).
// XTool, ThreadScope, FileWatch, MemTrace, FrameMeter, NetSpy, JavaMap, ShotBox, HeapGuard,
// RockTrace (XTool's built-in Rockstar list: texts, help messages, files - arm_trace.c).
// ShotBox takes its seconds after the line's name ("ShotBox 10 30 60"): droid_tool_args.
int droid_tool(const char *name);
const char *droid_tool_args(const char *name); // the rest of its line, "" if none

// The app's own settings, for every game (APP0/data/app_settings.txt)
typedef struct {
    int fps_cap;    // every game's frame rate cap: 30, 1 (60: the default; as saved before), 120, or 0 off
    int vibration;  // the menus' DualSense rumble; on by default
    int ui_sounds;  // the menus' sounds; on by default
    int text_large; // the menus' text a size up (a couch far from the TV); off by default
    int stats;      // the in-game stats overlay, every game: 0 off, 1 the frame rate (default), 2 the full panel
    int theme;      // the Quick menu's look: 0 the default (dark green), 1 dark (neutral grey)
} app_settings_t;
extern app_settings_t app_settings;
// the FPS cap in frames a second (0: off)
static inline int app_fps_cap_hz(void) { return app_settings.fps_cap == 1 ? 60 : app_settings.fps_cap; }
void app_settings_load(void);
int app_settings_save(void);

#endif
