#ifndef KEYMAP_H
#define KEYMAP_H

// A game's keymap (keymap.c): DualSense buttons that tap the game's on-screen buttons, and the
// left stick on its on-screen joystick. The game folder's touchmap.txt ("stick X Y R", "button
// KEYCODE X Y", "stick2 X Y R" the right stick's, 1920x1080): made in the Quick menu's Keymap Editor,
// read by every host. L2 / R2 (triggers: axes, not keys) are the keycodes 104 / 105, as Android's.

#define KEYMAP_MAX 16
#define KEY_L2 104
#define KEY_R2 105
typedef struct {
    float stick_x, stick_y, stick_r;    // the left stick's joystick (r 0: none)
    float stick2_x, stick2_y, stick2_r; // the right stick's (a camera's, an aim's)
    int count;
    struct { int keycode; float x, y; } button[KEYMAP_MAX];
} keymap_t;

extern volatile int droid_keymap_version; // goes up at each save (a host reads it again)

int keymap_read(keymap_t *k);        // from the game's touchmap.txt; 0: no file (k emptied)
int keymap_write(const keymap_t *k); // into it (the version goes up); 0 ok

// For the hosts without their own (GameActivity, cocos2d-x, Mortar): fingers through f, ids from 0
// (0 the left joystick, 1.. the buttons, KEYMAP_STICK2_ID the right joystick: the host puts them past its own)
typedef void (*keymap_finger_fn)(int id, int down, float x, float y);
void keymap_sync(void);                          // read again if it changed (call before the others)
int keymap_has_stick(void);
int keymap_has_stick2(void);
int keymap_has_key(int keycode);
void keymap_triggers(float l2, float r2, keymap_finger_fn f); // L2 / R2 (0..1) on their spots, if mapped
int keymap_key(int keycode, int down, keymap_finger_fn f); // 1: the keymap took the button
void keymap_stick(float lx, float ly);         // the left stick, as it moves
void keymap_stick2(float rx, float ry);        // the right one
void keymap_tick(keymap_finger_fn f);          // each frame: the fingers on the joysticks (a no-op without them)
#define KEYMAP_STICK2_ID (1 + KEYMAP_MAX)
#define KEYMAP_FINGERS (2 + KEYMAP_MAX)

#endif
