#ifndef UNITY_HOST_H
#define UNITY_HOST_H

#include "android_shim.h"

typedef void (*unity_input_sink)(const droid_input_event *);
typedef void (*unity_poll_fn)(unity_input_sink sink);

// Plays UnityPlayer's Java side for the APK's Unity libraries (in lib_dir).
// Returns when the game quits, or a negative step number if startup failed.
int unity_run(const char *lib_dir, unity_poll_fn poll);

#endif
