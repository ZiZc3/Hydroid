#ifndef MORTAR_HOST_H
#define MORTAR_HOST_H

#include "android_shim.h"
#include "soloader.h"

typedef void (*mortar_input_sink)(const droid_input_event *);
typedef void (*mortar_poll_fn)(mortar_input_sink sink);

// Halfbrick's Mortar engine (libmortargame.so, Java_com_halfbrick_mortar_NativeGameLib_*:
// Fruit Ninja 2.x, Fruit Ninja Classic 2.x, Jetpack Joyride).
int mortar_detect(so_module *mod);

// Plays Mortar's Java side (MortarGameActivity / GameManager / MortarGameView): the init steps in
// GameManager's order on a GL thread, then step() every frame. Never returns unless the natives
// are missing.
void mortar_run(so_module *mod, const char *apk_path, mortar_poll_fn poll);

#endif
