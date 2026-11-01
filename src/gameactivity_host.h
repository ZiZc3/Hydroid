// gameactivity_host.h — games on Google's GameActivity (the Android Game SDK), King's engine first
#ifndef GAMEACTIVITY_HOST_H
#define GAMEACTIVITY_HOST_H

#include "android_shim.h"
#include "soloader.h"

int gameactivity_detect(so_module *mod);
// poll: our input (the touchpad / PC mouse as touches), as the other hosts get it
void gameactivity_run(so_module *mod, void (*poll)(void (*)(const droid_input_event *)));

#endif
