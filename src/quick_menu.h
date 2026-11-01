#ifndef QUICK_MENU_H
#define QUICK_MENU_H

#include <stddef.h>
#include <stdint.h>

// The in-game Quick menu (quick_menu.c): hold Options in a game and it pauses: its picture
// frozen behind a frosted blur, its sound silent, and a flat card: Resume, the stats overlay,
// Keymap Editor (the keymap editor), Back to Home. The swap / present (gles_bridge.c, vulkan_bridge.c,
// pc/egl_pc.c) runs it: the game's last frame given once, then frames of ours until it's shut.
// Drawn on the CPU (Outfit, as the stats panel): the same pictures for GL, ANGLE and Vulkan.

extern volatile int droid_game_paused; // the audio loops send silence, the pad's events stop

// the game being played (app_main.c, at its start): the card's header
void quick_menu_set_game(const char *name, const char *id, const uint8_t *icon_rgba, int icon_size);
void quick_menu_request(void); // pad_poll: Options held
int quick_menu_wanted(void);   // a swap: open it now? (also a PC test's DROID_QM_TEST=<s>)
int quick_menu_resumed(void);  // pad_poll: once after a resume (the buttons still held are swallowed)

// The game's frame (its rows top first unless bottom_up; BGRA or RGBA) and the screen's size.
// 0: no menu (no memory, no font): the game goes on.
int quick_menu_begin(const uint8_t *px, int w, int h, size_t stride, int bgra, int bottom_up, int screen_w, int screen_h);
// One frame: the pad read, the animations on, paced to 60. 1 while it's open, 0 when it's shut.
int quick_menu_frame(void);
// The pictures of this frame. The backdrop: opaque RGBA, w x h (stretched over the screen). The
// card: premultiplied RGBA at the screen's pixels, at (x, y) on it, its alpha in *alpha (0..1);
// a version that changes when the pixels did.
const uint8_t *quick_menu_backdrop(int *w, int *h, unsigned *version);
const uint8_t *quick_menu_card(int *x, int *y, int *w, int *h, float *alpha, unsigned *version);
// The card made opaque over the backdrop (for copies that can't blend: Vulkan), in the screen's
// byte order, into dst (w x h, its rows dst_stride bytes apart)
void quick_menu_card_opaque(uint8_t *dst, size_t dst_stride, int bgra);
void quick_menu_card_max(int *w, int *h); // the biggest card it shows (a buffer for every one of them)
// After the last frame: the resume (the game goes on), or Close Game (the app closes: no return)
void quick_menu_end(void);
// The app closes, why in the log ("back": Close Game; "nolib": it couldn't start, no 64-bit ARM
// libraries)
void quick_menu_relaunch(const char *what);
// ... "error": the game stopped (it couldn't start, or it crashed); msg in the log
void quick_menu_relaunch_msg(const char *what, const char *msg);

#endif
