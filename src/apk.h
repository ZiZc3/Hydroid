#ifndef APK_H
#define APK_H

#include <stddef.h>
#include <stdint.h>

// The game's APK (a zip): its libraries and assets are read from it directly.
int apk_open(const char *path);   // 0 = ok
int apk_is_open(void);

// Each archive opened (a split APK = base + config.* APKs), in order.
int apk_archive_count(void);
const char *apk_archive_path(int i);

// Copies the entry (e.g. "lib/arm64-v8a/libgodot_android.so") to dest, unless
// a file of the same size is already there. 0 = ok.
int apk_extract(const char *name, const char *dest);
// The same, a chunk at a time (an 800 MB OBB must not go through memory). 0 = ok.
int apk_extract_stream(const char *name, const char *dest);
// An entry in memory (malloc'd, free it), its size in *size; NULL: not there.
void *apk_load(const char *name, size_t *size);

// Calls fn for each entry whose name starts with prefix.
void apk_each(const char *prefix, void (*fn)(const char *name, void *ctx), void *ctx);

// AAssetManager_* / AAsset_* / AAssetDir_* over assets/ in the APK.
void *apk_resolve(const char *name);

#endif
