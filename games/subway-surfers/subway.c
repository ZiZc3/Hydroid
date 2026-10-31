// subway.c — what the Subway Surfers app adds to the engine for this game.
//
// The engine runs the game's own arm64 code and answers its calls to Android. Subway Surfers
// needs one Java answer of its own, and three engine settings (game.json "engine"):
//
//   fastJit    the game's code translated in bigger blocks: almost no stutter while running
//   phoneView  a portrait phone screen (1080x1920) in the middle of the TV, black at the sides
//   etcNative  its ETC textures decoded by the engine instead of by Unity: faster loading
//
// Tested with Subway Surfers 3.70.3.

#include "java_layer.h"

// The game's save data helper (com.sybogames.chili.migration.KilooPlatformAndroidBridge)
// asks where its user data lives: the game's own files folder.
static jvalue userdata_path(jobject self, const jvalue *a) {
    (void)self;
    (void)a;
    char p[600];
    return L(jstr(data_path(p, sizeof(p), "files")));
}

// Its line in the engine's table of Java answers (class, method, signature, answer):
//
//   { "com/sybogames/chili/migration/KilooPlatformAndroidBridge", "getUserdataPath",
//     "()Ljava/lang/String;", userdata_path },
