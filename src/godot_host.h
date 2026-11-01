#ifndef GODOT_HOST_H
#define GODOT_HOST_H

#include "android_shim.h"
#include "soloader.h"

typedef void (*godot_input_sink)(const droid_input_event *);
typedef void (*godot_poll_fn)(godot_input_sink sink);

// A Godot 3 Android engine library (exports Java_org_godotengine_godot_GodotLib_step).
int godot_detect(so_module *mod);

// Plays Godot's Java side: initialize, setup, then a render thread with the GL
// context calling step() every frame. poll feeds the DualSense in. Never returns
// unless the natives are missing.
void godot_run(so_module *mod, godot_poll_fn poll);

#endif
