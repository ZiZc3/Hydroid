#ifndef GTASA_HOST_H
#define GTASA_HOST_H

#include "android_shim.h"
#include "soloader.h"

typedef void (*gtasa_input_sink)(const droid_input_event *);
typedef void (*gtasa_poll_fn)(gtasa_input_sink sink);

// A Rockstar oswrapper port (GTA San Andreas 2.11: libGame.so exports
// Java_com_rockstargames_oswrapper_GameNative_implOnDrawFrame).
int gtasa_detect(so_module *mod);

// Plays its Java side: setup, activity, surface, resume, then a frame every loop on one
// thread. poll feeds the DualSense in. Never returns unless the natives are missing.
void gtasa_run(so_module *mod, gtasa_poll_fn poll);

// GamePlatformServices' requests the engine waits on (java_layer.c), completed on the game
// thread between frames: GTASA_GATE's id is the gate's, GTASA_HTTP's the request's
enum { GTASA_INITIAL, GTASA_GATE, GTASA_SIGN_IN, GTASA_SIGN_OUT, GTASA_CLOUD_DISABLED, GTASA_DELETION, GTASA_HTTP,
       GTASA_PLAYLIST };
void gtasa_post(int kind, int id);

#endif
