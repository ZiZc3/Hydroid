#ifndef GAME_FILE_H
#define GAME_FILE_H

#include <stddef.h>
#include <stdint.h>

// The game file: the .apk / .xapk / .apks / .apkm in the app's /app0/game folder, and what it says
// about itself (package, name, version, icon). The game gets its own folder for its files and
// settings (game_settings.h).

#define LIB_MAX 64

typedef struct {
    char path[256];   // the file
    char id[128];     // its package, or its file name
    char name[128];   // its name
    char engine[32];  // Unity / Godot / ... once known ("" if not yet)
    char dir[256];    // its folder
    uint8_t *icon;    // RGBA, icon_size x icon_size; NULL: none found
    int icon_size;
    long long bytes;
} lib_game;

// An attribute of a binary AndroidManifest.xml (the first tag with it): its text, or *num
int axml_attr(const uint8_t *x, size_t len, const char *name, char *out, size_t n, long *num);
// a <meta-data> value of the manifest by its name (text and number); 0 if not there
int axml_meta(const uint8_t *x, size_t len, const char *key, char *out, size_t n, long *num);

int library_scan_in(const char *folder, lib_game *games, int max); // the game files of a folder (/app0/game)
// the folder a game file gets (data/games/<from its file name>), and the version in it (0: not found)
void library_dir_for(const char *path, char *dir, size_t n);
long long library_version_code(const char *path);
void library_free(lib_game *games, int n);
// a PNG / JPEG as RGBA, LIBRARY_ICON x LIBRARY_ICON; NULL when it isn't one
#define LIBRARY_ICON 256
uint8_t *library_icon_decode(const uint8_t *data, size_t len);
// any picture (screenshots), fitted into max_w x max_h, its size in *w, *h
uint8_t *library_image_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);

#endif
