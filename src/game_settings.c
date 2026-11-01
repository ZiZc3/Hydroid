// game_settings.c — the running game's folder and settings (game_settings.h).

#include "game_settings.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>

#include "android_shim.h"

void droid_log(const char *fmt, ...);

game_settings settings = { RENDERER_AUTO, 0, 1, 1, 0, 0, 0, 0 };
#ifdef DROID_PC
#define CORES_DEFAULT 0 // the PC: 20 CPUs, Mesa's threads out of reach (42 fps placed, 48 not)
#else
#define CORES_DEFAULT 1
#endif
static const game_settings defaults = { RENDERER_AUTO, 0, 1, 1, 0, 0, 0, 0, CORES_DEFAULT, 1, 1, 0, VIEW_AUTO };
int droid_game_w = 1920, droid_game_h = 1080;
char game_dir[256] = APP0 "/data";

const char *game_path(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s/%s", game_dir, rel);
    return out;
}

// The game's keymap to read: its folder's touchmap.txt (made in the Keymap Editor), else the one
// a game app ships (/app0/touchmap.txt: the same 1920x1080 game points on every TV).
FILE *touchmap_open_read(void) {
    char path[300];
    FILE *f = fopen(game_path(path, sizeof(path), "touchmap.txt"), "r");
    if (!f) f = fopen(APP0 "/touchmap.txt", "r");
    return f;
}

// What the game itself writes (its files, cache, prefs, downloads) lives in game_data_dir: the
// title's download data (/download0/games/<game>) when it has room, else the game's folder.
// File work there costs ~0.02 ms; in /app0 or /data ~8 ms a created file, ~15 ms a folder
// moved (FsBench) - Unity's cache made Vector's menus freeze for seconds. Our own files (the
// unpacked APK, settings, tools) stay in the game's folder.
char game_data_dir[300] = APP0 "/data";
const char *data_path(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s/%s", game_data_dir, rel);
    return out;
}

static int exists(const char *rel) {
    char p[512];
    struct stat st;
    return stat(game_path(p, sizeof(p), rel), &st) == 0;
}

static void make_dirs(const char *dir) {
    char p[256];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(p, 0777);
        *s = '/';
    }
    mkdir(p, 0777);
}

static const struct { const char *key; int *value; } keys[] = {
    { "renderer", &settings.renderer },     { "refresh_hz", &settings.refresh_hz },
    { "fps_counter", &settings.fps_counter }, { "cursor", &settings.cursor },
    { "unity_fps30", &settings.unity_fps30 }, { "hide_astc", &settings.hide_astc },
    { "debug_log", &settings.debug_log }, { "offline", &settings.offline },
    { "cores", &settings.cores },
    { "bigfiles", &settings.bigfiles },
    { "writers", &settings.writers },
    { "atomics", &settings.exact_atomics },
    { "motion", &settings.motion },
    { "view", &settings.view },
};

void game_settings_load(const char *dir) {
    snprintf(game_dir, sizeof(game_dir), "%s", dir);
    make_dirs(game_dir);
    settings = defaults; // not the last game's
    app_settings_load(); // the FPS cap & co., for every game
    // the old switch files, then settings.txt over them
    if (exists("vulkan")) settings.renderer = RENDERER_VULKAN;
    if (exists("hz120")) settings.refresh_hz = 120;
    if (exists("fps30")) settings.unity_fps30 = 1;
    if (exists("noastc")) settings.hide_astc = 1;
    if (exists("debug-log")) settings.debug_log = 1;
    char p[512];
    FILE *f = fopen(game_path(p, sizeof(p), "settings.txt"), "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char key[64];
            int v;
            if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
                if (!strcmp(key, keys[i].key)) *keys[i].value = v;
        }
        fclose(f);
    }
    if (settings.refresh_hz != 60 && settings.refresh_hz != 120) settings.refresh_hz = 0; // auto
    droid_log("game: folder %s, renderer %s, screen %s, fps counter %d, cursor %d", game_dir,
              settings.renderer == RENDERER_VULKAN ? "Vulkan" : settings.renderer == RENDERER_GLES ? "GLES"
              : settings.renderer == RENDERER_ANGLE ? "ANGLE" : "auto",
              settings.refresh_hz == 120 ? "120 Hz" : settings.refresh_hz == 60 ? "60 Hz" : "auto (60 Hz)",
              settings.fps_counter, settings.cursor);
    if (settings.offline) droid_log("game: offline mode (no internet for the game)");
}

int game_settings_save(void) {
    char p[512];
    FILE *f = fopen(game_path(p, sizeof(p), "settings.txt"), "w");
    if (!f) return -1;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) fprintf(f, "%s=%d\n", keys[i].key, *keys[i].value);
    fclose(f);
    return 0;
}

// ---- the app's settings (all games) ----------------------------------------------------

app_settings_t app_settings = { 1, 1, 1, 0, 1, 0, 0 };
#define APP_SETTINGS APP0 "/data/app_settings.txt"

void app_settings_load(void) {
    app_settings = (app_settings_t){ 1, 1, 1, 0, 1, 0, 0 };
    FILE *f = fopen(APP_SETTINGS, "r");
    if (f) {
        char line[128];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "fps_cap=%d", &v) == 1)
                app_settings.fps_cap = v == 30 || v == 120 ? v : v == 60 ? 1 : v != 0; // 30 / 60 (1) / 120 / off (0)
            if (sscanf(line, "vibration=%d", &v) == 1) app_settings.vibration = v != 0;
            if (sscanf(line, "ui_sounds=%d", &v) == 1) app_settings.ui_sounds = v != 0;
            if (sscanf(line, "text_large=%d", &v) == 1) app_settings.text_large = v != 0;
            if (sscanf(line, "stats=%d", &v) == 1) app_settings.stats = v < 0 ? 0 : v > 2 ? 2 : v;
            if (sscanf(line, "theme=%d", &v) == 1) app_settings.theme = v != 0;
        }
        fclose(f);
    }
    extern int droid_single_game; // app_main.c: a one-game app shows no stats overlay
    if (droid_single_game) app_settings.stats = 0;
    droid_log("app: FPS cap %d", app_fps_cap_hz());
}

int app_settings_save(void) {
    mkdir(APP0 "/data", 0777);
    FILE *f = fopen(APP_SETTINGS, "w");
    if (!f) return -1;
    fprintf(f, "fps_cap=%d\nvibration=%d\nui_sounds=%d\ntext_large=%d\nstats=%d\ntheme=%d\n", app_settings.fps_cap,
            app_settings.vibration, app_settings.ui_sounds, app_settings.text_large, app_settings.stats, app_settings.theme);
    fclose(f);
    return 0;
}

// ---- clearing a game's data ("Clear saves" / "Clear everything") ------------------------
// Like Android's "Clear storage": leftovers from older builds of ours (Shadow Fight 2's save
// hashes made with an early device id) keep a game stuck; this starts it fresh.

static long remove_tree(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    long n = 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[600];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            n += remove_tree(sub);
        }
        if (d) closedir(d);
        rmdir(path);
    } else if (unlink(path) == 0) {
        n++;
    }
    return n;
}

long game_clear(const char *dir, int everything) {
    // ours, always kept: the Options, the game's name/icon/engine notes
    static const char *const ours[] = { "settings.txt", "game-info.txt", "icon.rgba", "engine.txt", "tools.txt", NULL };
    // the unpacked APKs: kept unless everything goes (the next start unpacks them again)
    static const char *const unpacked[] = { "apk", "apk-lib", NULL };
    // only a game's own folder (data/games/<game>): the old single game uses data/ itself,
    // which holds every game's folder and the app's settings
    const char *root = APP0 "/data/games/";
    if (strncmp(dir, root, strlen(root)) != 0 || !dir[strlen(root)] || strstr(dir, "/..")) {
        droid_log("game: %s is not a game's own folder: not cleared", dir);
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) return -1;
    long n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        int keep = 0;
        for (int i = 0; ours[i] && !keep; i++) keep = !strcmp(name, ours[i]);
        for (int i = 0; unpacked[i] && !keep && !everything; i++) keep = !strcmp(name, unpacked[i]);
        if (keep) continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        n += remove_tree(path);
    }
    closedir(d);
    char fast[300]; // its data in the fast folder: all of it is the game's
    fast_data_path(dir, fast, sizeof(fast));
    struct stat fst;
    if (stat(fast, &fst) == 0) {
        n += remove_tree(fast);
        mkdir(fast, 0777);
    }
    droid_log("game: %s cleared (%s): %ld file(s) removed", dir, everything ? "everything" : "saves", n);
    return n;
}

int game_delete(const char *file, const char *dir) {
    // only the app's own game files: game/<file>, or the old single game.* of APP0
    const char *lib = APP0 "/games/";
    int ok_file = (!strncmp(file, lib, strlen(lib)) && file[strlen(lib)] && !strchr(file + strlen(lib), '/')) ||
                  !strncmp(file, APP0 "/game.", strlen(APP0 "/game."));
    if (!ok_file || strstr(file, "/..")) {
        droid_log("game: %s is not a library file: not deleted", file);
        return -1;
    }
    if (remove(file) != 0) {
        droid_log("game: %s could not be deleted (errno %d)", file, errno);
        return -1;
    }
    const char *root = APP0 "/data/games/";
    if (!strncmp(dir, root, strlen(root)) && dir[strlen(root)] && !strstr(dir, "/..")) {
        long n = remove_tree(dir);
        char fast[300];
        fast_data_path(dir, fast, sizeof(fast));
        n += remove_tree(fast);
        // its Vulkan copy (app_main.c): data/vulkan-first/<folder name without .apk/.xapk/...>.apk
        char name[256], copy[400];
        snprintf(name, sizeof(name), "%s", dir + strlen(root));
        char *dot = strrchr(name, '.');
        if (dot && (!strcasecmp(dot, ".apk") || !strcasecmp(dot, ".xapk") || !strcasecmp(dot, ".apks") ||
                    !strcasecmp(dot, ".apkm")))
            *dot = 0;
        snprintf(copy, sizeof(copy), APP0 "/data/vulkan-first/%s.apk", name);
        remove(copy);
        droid_log("game: %s deleted, its folder too (%ld files)", file, n);
    } else {
        droid_log("game: %s deleted", file);
    }
    return 0;
}


// ---- the fast data folder ------------------------------------------------------------------
// (PC tests: DROID_FAST_ROOT=<a folder> stands in for it)
void big_files_setup(const char *fast, const char *game_dir);
static const char *fast_root(void) {
    const char *r = getenv("DROID_FAST_ROOT");
    return r && *r ? r : "/download0";
}
// our files: they stay in the game's folder (the rest moves to the data folder once)
static const char *const ours_kept[] = { "apk", "apk-lib", "settings.txt", "game-info.txt", "icon.rgba", "engine.txt",
                                         "tools.txt", "obb", "data-moved.txt", "xtool.txt", "xtool-names.txt",
                                         "javamap.txt", "touchmap.txt", "maps.txt", "big", "big-files.txt", NULL };
static int kept(const char *name) {
    for (int i = 0; ours_kept[i]; i++)
        if (!strcmp(name, ours_kept[i])) return 1;
    return !strncmp(name, "shot_", 5); // ShotBox's pictures
}

// room for games there: the title's download data is as big as param.json says (16 GB now;
// 256 MB before - an app installed with the old one). Tried once (512 MB written), a marker kept
static int fast_area_ok(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
    ok = 0;
    struct stat st;
    char room[300], probe[300];
    snprintf(room, sizeof(room), "%s/android-room.txt", fast_root());
    snprintf(probe, sizeof(probe), "%s/android-probe.bin", fast_root());
    if (stat(fast_root(), &st) != 0 || !S_ISDIR(st.st_mode)) return ok;
    if (stat(room, &st) == 0) return ok = 1;
    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        droid_log("data: %s not writable (errno %d): the games keep their data in their folders", fast_root(), errno);
        return ok;
    }
    static char chunk[1 << 20];
    int good = 1;
    for (int i = 0; i < 512 && good; i++) good = write(fd, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk);
    close(fd);
    unlink(probe);
    if (!good) {
        droid_log("data: %s has under 512 MB (the app was installed with downloadDataSize 256): the games keep "
                  "their data in their folders - reinstall the app for the fast one", fast_root());
        return ok;
    }
    FILE *f = fopen(room, "w");
    if (f) {
        fputs("512 MB written: room for the games data\n", f);
        fclose(f);
    }
    return ok = 1;
}

static long copy_tree(const char *a, const char *b) { // files copied, -1 on a failure
    struct stat st;
    if (stat(a, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        mkdir(b, 0777);
        DIR *d = opendir(a);
        if (!d) return -1;
        long n = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char x[600], y[600];
            snprintf(x, sizeof(x), "%s/%s", a, e->d_name);
            snprintf(y, sizeof(y), "%s/%s", b, e->d_name);
            long k = copy_tree(x, y);
            if (k < 0) {
                closedir(d);
                return -1;
            }
            n += k;
        }
        closedir(d);
        return n;
    }
    int in = open(a, O_RDONLY), out = in >= 0 ? open(b, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    static char buf[1 << 20];
    ssize_t r = 0;
    int bad = in < 0 || out < 0;
    while (!bad && (r = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)r) != r) bad = 1;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return bad || r < 0 ? -1 : 1;
}

static void *remove_moved(void *arg) { // the old copies, in the background (slow deletes there)
    char *dir = arg;
    DIR *d = opendir(dir);
    long n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || kept(e->d_name)) continue;
        char x[600];
        snprintf(x, sizeof(x), "%s/%s", dir, e->d_name);
        n += remove_tree(x);
    }
    if (d) closedir(d);
    droid_log("data: %ld old file(s) removed from %s", n, dir);
    free(dir);
    return NULL;
}

void fast_data_path(const char *dir, char *out, size_t n) { // a game's folder -> its fast one
    const char *slash = strrchr(dir, '/');
    snprintf(out, n, "%s/games/%s", fast_root(), slash ? slash + 1 : dir);
}

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void game_data_prepare(void) {
    snprintf(game_data_dir, sizeof(game_data_dir), "%s", game_dir);
    if (getenv("DROID_SLOW_DATA") || !fast_area_ok()) return;
    char fast[300], mark[400];
    fast_data_path(game_dir, fast, sizeof(fast));
    make_dirs(fast);
    game_path(mark, sizeof(mark), "data-moved.txt");
    struct stat st;
    if (stat(mark, &st) != 0) { // the first time: what the game wrote so far, moved over
        double t0 = now_secs();
        DIR *d = opendir(game_dir);
        long files = 0;
        int failed = 0;
        struct dirent *e;
        while (d && !failed && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || kept(e->d_name)) continue;
            char x[600], y[600];
            snprintf(x, sizeof(x), "%s/%s", game_dir, e->d_name);
            snprintf(y, sizeof(y), "%s/%s", fast, e->d_name);
            long k = copy_tree(x, y);
            if (k < 0) failed = 1;
            else files += k;
        }
        if (d) closedir(d);
        if (failed) {
            droid_log("data: moving %s to %s failed (errno %d): its data stays in its folder", game_dir, fast, errno);
            remove_tree(fast);
            return;
        }
        FILE *f = fopen(mark, "w");
        if (f) {
            fprintf(f, "%s\n", fast);
            fclose(f);
        }
        droid_log("data: %ld file(s) moved to %s in %.1f s", files, fast, now_secs() - t0);
        pthread_t t;
        if (pthread_create(&t, NULL, remove_moved, strdup(game_dir)) == 0) pthread_detach(t);
    }
    snprintf(game_data_dir, sizeof(game_data_dir), "%s", fast);
    droid_log("data: the game's own files in %s (fast)", game_data_dir);
    big_files_setup(fast, game_dir); // big_files.c: its big files on the disk that writes them fast
}

// ---- the debug tools' switch: tools.txt (the game's folder, then APP0/data), DROID_TOOLS ----
#define TOOL_LINES 32
static struct { char name[24]; char args[104]; } tool_lines[TOOL_LINES];
static int tool_count = -1;
static void tools_read(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f) && tool_count < TOOL_LINES) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char name[24] = "";
        int used = 0;
        if (sscanf(line, " %23s%n", name, &used) != 1) continue;
        char *rest = line + used;
        while (*rest == ' ' || *rest == '\t') rest++;
        rest[strcspn(rest, "\r\n")] = 0;
        snprintf(tool_lines[tool_count].name, sizeof(tool_lines[0].name), "%s", name);
        snprintf(tool_lines[tool_count].args, sizeof(tool_lines[0].args), "%s", rest);
        tool_count++;
    }
    fclose(f);
}
static void tools_load(void) {
    // once per game folder: a tool asked before the game was chosen (the menus' flip rate)
    // read none, and every tool stayed off for the game
    static char loaded_for[256];
    if (tool_count >= 0 && !strcmp(loaded_for, game_dir)) return;
    snprintf(loaded_for, sizeof(loaded_for), "%s", game_dir);
    tool_count = 0;
    char p[512];
    if (game_dir[0]) tools_read(game_path(p, sizeof(p), "tools.txt"));
    tools_read(APP0 "/data/tools.txt");
    const char *env = getenv("DROID_TOOLS");
    for (const char *e = env; e && *e && tool_count < TOOL_LINES;) {
        while (*e == ' ' || *e == ',') e++;
        size_t n = strcspn(e, " ,");
        if (!n) break;
        snprintf(tool_lines[tool_count].name, sizeof(tool_lines[0].name), "%.*s", (int)n, e);
        tool_lines[tool_count].args[0] = 0;
        tool_count++;
        e += n;
    }
    for (int i = 0; i < tool_count; i++) droid_log("tools: %s on%s%s", tool_lines[i].name,
                                                     tool_lines[i].args[0] ? ": " : "", tool_lines[i].args);
}
int droid_tool(const char *name) {
    tools_load();
    for (int i = 0; i < tool_count; i++)
        if (!strcasecmp(tool_lines[i].name, name)) return 1;
    return 0;
}
const char *droid_tool_args(const char *name) {
    tools_load();
    for (int i = 0; i < tool_count; i++)
        if (!strcasecmp(tool_lines[i].name, name)) return tool_lines[i].args;
    return "";
}

int droid_game_in(const char *list) {
    extern const char *java_package; // jni_impl.c
    if (!java_package || !list) return 0;
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ",");
        if (n && !strncmp(java_package, p, n)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}
