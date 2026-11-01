// game_backup.c — a game's saves backed up and restored (game_settings.h). Its own data (the game's
// folder in data/games and its fast folder): what Clear saves would remove, less its caches, temp
// files and its unpacked APKs, into Hydroid-Backups/<game>/ on a USB drive when one is in
// (/mnt/usb0), else on the console's own /data: outside Hydroid's folder, so a deleted game or
// Hydroid itself leaves it. A new backup goes to <game>.new first, then takes the old one's place.

#include "game_settings.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "android_shim.h"

// not saves: ours (Options, the game's notes), the unpacked APKs, caches and temp files
static const char *const skip[] = { "settings.txt", "launcher.txt", "icon.rgba", "engine.txt", "tools.txt",
                                    "orientation.txt", "apk", "apk-lib", "il2cpp", "big", "cache", "code_cache",
                                    "tmp", "vulkan-first", NULL };
static int skipped(const char *name) {
    for (int i = 0; skip[i]; i++)
        if (!strcmp(name, skip[i])) return 1;
    return 0;
}

static int game_root_ok(const char *dir) { // only a game's own folder (data/games/<game>)
    const char *root = APP0 "/data/games/";
    return !strncmp(dir, root, strlen(root)) && dir[strlen(root)] && !strstr(dir, "/..");
}
static const char *game_name(const char *dir) {
    const char *s = strrchr(dir, '/');
    return s ? s + 1 : dir;
}

// the backups' folder: a USB drive's, else the console's
static void backups_root(char *out, size_t n) {
    struct stat st;
    if (stat("/mnt/usb0", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, n, "/mnt/usb0/Hydroid-Backups");
        mkdir(out, 0777);
        if (stat(out, &st) == 0) return;
    }
    snprintf(out, n, "/data/Hydroid-Backups");
    mkdir(out, 0777);
}
static void backup_dir(const char *dir, char *out, size_t n) {
    char root[200];
    backups_root(root, sizeof(root));
    snprintf(out, n, "%s/%s", root, game_name(dir));
}

// ---- trees: their size, a copy, a removal
static long long tree_bytes(const char *path, int top) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    long long n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && skipped(e->d_name))) continue;
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        n += tree_bytes(p, 0);
    }
    closedir(d);
    return n;
}

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return -1;
    }
    static char buf[1 << 20];
    int ok = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) { ok = -1; break; }
        if (r == 0) break;
        for (ssize_t at = 0; at < r;) {
            ssize_t w = write(out, buf + at, (size_t)(r - at));
            if (w <= 0) { ok = -1; break; }
            at += w;
        }
        if (ok) break;
    }
    close(in);
    if (close(out) != 0) ok = -1;
    return ok;
}

static long copy_tree(const char *from, const char *to, int top, int *failed) {
    struct stat st;
    if (lstat(from, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) return 0;
        if (copy_file(from, to) != 0) {
            droid_log("backup: %s couldn't be copied (errno %d)", from, errno);
            *failed = 1;
            return 0;
        }
        return 1;
    }
    mkdir(to, 0777);
    DIR *d = opendir(from);
    if (!d) return 0;
    long n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && !*failed) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && skipped(e->d_name))) continue;
        char a[700], b[700];
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        n += copy_tree(a, b, 0, failed);
    }
    closedir(d);
    return n;
}

static void remove_all(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[700];
            snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
            remove_all(p);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

// ---- the game's saves
long long game_saves_bytes(const char *dir) {
    if (!game_root_ok(dir)) return 0;
    char fast[300];
    fast_data_path(dir, fast, sizeof(fast));
    return tree_bytes(dir, 1) + tree_bytes(fast, 1);
}

int game_backup_info(const char *dir, char *when, size_t wn) {
    char b[300], stamp[340];
    backup_dir(dir, b, sizeof(b));
    snprintf(stamp, sizeof(stamp), "%s/backup.txt", b);
    FILE *f = fopen(stamp, "r");
    if (!f) return 0;
    char line[120] = "";
    if (fgets(line, sizeof(line), f)) line[strcspn(line, "\r\n")] = 0;
    fclose(f);
    if (when && wn) snprintf(when, wn, "%s", line);
    return 1;
}

int game_backup(const char *dir) {
    if (!game_root_ok(dir)) return -1;
    char b[300], tmp[310], fast[300], part[340];
    backup_dir(dir, b, sizeof(b));
    snprintf(tmp, sizeof(tmp), "%s.new", b);
    remove_all(tmp);
    mkdir(tmp, 0777);
    fast_data_path(dir, fast, sizeof(fast));
    int failed = 0;
    snprintf(part, sizeof(part), "%s/app", tmp);
    long n = copy_tree(dir, part, 1, &failed);
    snprintf(part, sizeof(part), "%s/fast", tmp);
    struct stat st;
    if (!failed && stat(fast, &st) == 0) n += copy_tree(fast, part, 1, &failed);
    if (failed) {
        remove_all(tmp);
        droid_log("backup: %s not backed up (a copy failed): the old backup kept", game_name(dir));
        return -1;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(part, sizeof(part), "%s/backup.txt", tmp);
    FILE *f = fopen(part, "w");
    if (f) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d\n%ld files\n", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, n);
        fclose(f);
    }
    remove_all(b);
    if (rename(tmp, b) != 0) {
        droid_log("backup: %s couldn't take the old one's place (errno %d)", tmp, errno);
        return -1;
    }
    droid_log("backup: %s backed up to %s (%ld files)", game_name(dir), b, n);
    return 0;
}

int game_restore(const char *dir) {
    if (!game_root_ok(dir)) return -1;
    char b[300], fast[300], part[340];
    backup_dir(dir, b, sizeof(b));
    if (!game_backup_info(dir, NULL, 0)) return -1;
    game_clear(dir, 0); // its saves now go (Options and unpacked files stay), then the backup's in
    fast_data_path(dir, fast, sizeof(fast));
    int failed = 0;
    snprintf(part, sizeof(part), "%s/app", b);
    long n = copy_tree(part, dir, 0, &failed);
    snprintf(part, sizeof(part), "%s/fast", b);
    struct stat st;
    if (!failed && stat(part, &st) == 0) n += copy_tree(part, fast, 0, &failed);
    droid_log("backup: %s restored from %s (%ld files)%s", game_name(dir), b, n, failed ? ", some failed" : "");
    return failed ? -1 : 0;
}
