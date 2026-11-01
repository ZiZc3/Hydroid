// big_files.c — a game's big files on the disk that writes them fast. The data folder
// (/download0) is quick for small file work (create, rename, folders: under 1 ms; 6-48 ms in
// /app0/data) but writes big files at 28 MB/s, /app0/data at 150-190 MB/s (FsBench). Shadow Fight
// 4's download: 1561 files, 85% of its bytes in the 122 of 1 MB and more. A file the game writes
// past 2 MB moves there: on its writer thread (write_behind.c) what was written is copied and
// the writer writes the rest to the copy (the game's fd stays the placeholder's: the console
// refuses dup2, errno 1 - its writes, seeks and sizes go to the copy). Its name stays in the data folder
// (folder listings stay right; emptied when the game closes it) and its path leads to the copy
// from then on (droid_abs). The list: big-files.txt in the game's folder, "+path" / "-path"
// lines added as they come. DROID_NO_BIG=1: off.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "game_settings.h"

void droid_log(const char *fmt, ...);

#define BUCKETS 4096

typedef struct entry {
    struct entry *next;
    char rel[]; // from the data folder
} entry;

static entry *table[BUCKETS];
static int count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char fast_dir[300], mirror_dir[320]; // the data folder; the copies' (<game>/big)
static size_t fast_len;
static int list_fd = -1, on;
static long long moved_bytes;
static int moved_files;

static unsigned hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h % BUCKETS;
}

static entry **find(const char *rel) {
    entry **e = &table[hash(rel)];
    while (*e && strcmp((*e)->rel, rel)) e = &(*e)->next;
    return e;
}

static void put(const char *rel) {
    entry **e = find(rel);
    if (*e) return;
    size_t l = strlen(rel) + 1;
    entry *n = malloc(sizeof(*n) + l);
    if (!n) return;
    memcpy(n->rel, rel, l);
    n->next = NULL;
    *e = n;
    count++;
}

static void drop(const char *rel) {
    entry **e = find(rel);
    if (!*e) return;
    entry *d = *e;
    *e = d->next;
    free(d);
    count--;
}

static void note(char sign, const char *rel) { // lock held
    if (list_fd < 0) return;
    char line[600];
    int l = snprintf(line, sizeof(line), "%c%s\n", sign, rel);
    if (l > 0 && l < (int)sizeof(line) && write(list_fd, line, (size_t)l) != l) {}
}

// a path in the data folder: its part after it, or NULL
static const char *rel_of(const char *path) {
    if (!on || !path || strncmp(path, fast_dir, fast_len) || path[fast_len] != '/') return NULL;
    return path + fast_len + 1;
}

static void make_parents(const char *path) {
    char p[600];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + strlen(mirror_dir) + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(p, 0777);
        *s = '/';
    }
}

// the game starts: its data folder (fast) and its own folder (the copies go to <dir>/big)
void big_files_setup(const char *fast, const char *game_dir) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < BUCKETS; i++)
        while (table[i]) {
            entry *d = table[i];
            table[i] = d->next;
            free(d);
        }
    count = 0;
    if (list_fd >= 0) close(list_fd);
    list_fd = -1;
    on = 0;
    if (getenv("DROID_NO_BIG")) {
        pthread_mutex_unlock(&lock);
        droid_log("data: big files stay in the data folder (DROID_NO_BIG)");
        return;
    }
    snprintf(fast_dir, sizeof(fast_dir), "%s", fast);
    fast_len = strlen(fast_dir);
    snprintf(mirror_dir, sizeof(mirror_dir), "%s/big", game_dir);
    mkdir(mirror_dir, 0777);
    char list[400];
    snprintf(list, sizeof(list), "%s/big-files.txt", game_dir);
    FILE *f = fopen(list, "r");
    int lines = 0;
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            if (l < 2) continue;
            lines++;
            if (line[0] == '+') put(line + 1);
            else if (line[0] == '-') drop(line + 1);
        }
        fclose(f);
    }
    if (lines > 4 * count + 256) { // mostly gone ones: written again, short
        f = fopen(list, "w");
        for (int i = 0; f && i < BUCKETS; i++)
            for (entry *e = table[i]; e; e = e->next) fprintf(f, "+%s\n", e->rel);
        if (f) fclose(f);
    }
    list_fd = open(list, O_WRONLY | O_CREAT | O_APPEND, 0666);
    on = 1;
    pthread_mutex_unlock(&lock);
    droid_log("data: big files (2 MB and more) in %s: %d there", mirror_dir, count);
}

// a path the game named (made absolute): its copy's when it is one of the big files
const char *big_map(const char *path, char *buf, size_t n) {
    const char *rel = rel_of(path);
    if (!rel || !count) return path;
    pthread_mutex_lock(&lock);
    int big = *find(rel) != NULL;
    pthread_mutex_unlock(&lock);
    if (!big) return path;
    snprintf(buf, n, "%s/%s", mirror_dir, rel);
    return buf;
}

// the file at path (the data folder's), written through fd, moved to the fast disk: on its
// writer thread, after everything before was written. 0: moved (*copy: the copy's fd, open at
// its end, for the rest of the writes)
int big_promote(int fd, const char *path, char *to, size_t to_n, int *copy) {
    const char *rel = rel_of(path);
    if (!rel) return -1;
    snprintf(to, to_n, "%s/%s", mirror_dir, rel);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    make_parents(to);
    int in = open(path, O_RDONLY), out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    static char buf[1 << 20]; // writer threads one at a time: the lock below
    static pthread_mutex_t copying = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&copying);
    long long total = 0;
    ssize_t r = 0;
    int bad = in < 0 || out < 0;
    while (!bad && (r = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)r) != r) bad = 1;
        total += r;
    }
    pthread_mutex_unlock(&copying);
    if (in >= 0) close(in);
    off_t end = lseek(fd, 0, SEEK_CUR);
    if (bad || r < 0 || end != total) {
        int e = errno;
        if (out >= 0) close(out);
        unlink(to);
        static int told;
        if (told++ < 10) droid_log("data: %s not moved to the fast disk (errno %d, %lld of %lld bytes)", rel, e, total, (long long)end);
        return -1;
    }
    *copy = out;
    pthread_mutex_lock(&lock);
    put(rel);
    note('+', rel);
    moved_files++;
    moved_bytes += total;
    pthread_mutex_unlock(&lock);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    static int told;
    if (told++ < 20)
        droid_log("data: %s to the fast disk (%lld KB copied in %.0f ms)", rel, total >> 10,
                  (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    return 0;
}

// one to move: in the data folder, not moved yet
int big_candidate(const char *path) {
    const char *rel = rel_of(path);
    if (!rel || !settings.bigfiles) return 0; // settings.txt bigfiles=0: new ones stay (moved ones still found)
    pthread_mutex_lock(&lock);
    int big = *find(rel) != NULL;
    pthread_mutex_unlock(&lock);
    return !big;
}

// the game closed a moved file: its name in the data folder emptied (the copy has the data),
// when it is still one of ours (not deleted and made again meanwhile)
void big_closed(const char *path) {
    const char *rel = rel_of(path);
    if (!rel) return;
    pthread_mutex_lock(&lock);
    int big = *find(rel) != NULL;
    pthread_mutex_unlock(&lock);
    if (big) truncate(path, 0);
}

// unlink / remove of a data folder path (before the game's own): its copy goes too
void big_unlinked(const char *path) {
    const char *rel = rel_of(path);
    if (!rel || !count) return;
    pthread_mutex_lock(&lock);
    int big = *find(rel) != NULL;
    if (big) {
        drop(rel);
        note('-', rel);
    }
    pthread_mutex_unlock(&lock);
    if (big) {
        char to[640];
        snprintf(to, sizeof(to), "%s/%s", mirror_dir, rel);
        unlink(to);
    }
}

// rmdir of a data folder path: its copies' folder too (empty ones)
void big_rmdir(const char *path) {
    const char *rel = rel_of(path);
    if (!rel || !count) return;
    char to[640];
    snprintf(to, sizeof(to), "%s/%s", mirror_dir, rel);
    rmdir(to);
}

// rename a -> b in the data folder (before the game's own): the copies follow - one file, or a
// folder with big files in it (Unity's cache: a temporary folder renamed when it is complete).
// The list changes under the lock, the disk work after it (a folder made there: 7-48 ms)
void big_renamed(const char *a, const char *b) {
    const char *ra = rel_of(a), *rb = rel_of(b);
    if (!count || (!ra && !rb) || (ra && rb && !strcmp(ra, rb))) return;
    char *gone = NULL;  // the big file a normal one replaces
    char **olds = NULL, **news = NULL;
    int moves = 0, cap = 0;
    pthread_mutex_lock(&lock);
    if (rb && *find(rb) && !(ra && *find(ra))) {
        drop(rb);
        note('-', rb);
        gone = strdup(rb);
    }
    size_t la = ra ? strlen(ra) : 0;
    for (int i = 0; ra && i < BUCKETS; i++)
        for (entry **e = &table[i]; *e;) {
            entry *x = *e;
            if (strncmp(x->rel, ra, la) || (x->rel[la] && x->rel[la] != '/')) {
                e = &x->next;
                continue;
            }
            *e = x->next;
            count--;
            note('-', x->rel);
            if (moves == cap) {
                cap = cap ? cap * 2 : 16;
                olds = realloc(olds, sizeof(*olds) * (size_t)cap);
                news = realloc(news, sizeof(*news) * (size_t)cap);
            }
            olds[moves] = strdup(x->rel);
            news[moves] = NULL;
            if (rb) {
                char nrel[600];
                snprintf(nrel, sizeof(nrel), "%s%s", rb, x->rel + la);
                news[moves] = strdup(nrel);
            }
            moves++;
            free(x);
        }
    for (int k = 0; k < moves; k++)
        if (news[k]) { // the new names, after the old ones are all out (none matched twice)
            put(news[k]);
            note('+', news[k]);
        }
    pthread_mutex_unlock(&lock);
    char from[640], to[640];
    if (gone) {
        snprintf(to, sizeof(to), "%s/%s", mirror_dir, gone);
        unlink(to);
        free(gone);
    }
    for (int k = 0; k < moves; k++) {
        snprintf(from, sizeof(from), "%s/%s", mirror_dir, olds[k]);
        if (news[k]) {
            snprintf(to, sizeof(to), "%s/%s", mirror_dir, news[k]);
            make_parents(to);
            if (rename(from, to) != 0) droid_log("data: big file %s not renamed (errno %d)", olds[k], errno);
        } else {
            unlink(from); // moved out of the data folder: not ours to follow
        }
        free(olds[k]);
        free(news[k]);
    }
    free(olds);
    free(news);
}

// for the speed report: files and MB moved since the last
void big_report(void) {
    pthread_mutex_lock(&lock);
    int f = moved_files;
    long long b = moved_bytes;
    moved_files = 0;
    moved_bytes = 0;
    pthread_mutex_unlock(&lock);
    if (f) droid_log("data: %d big file(s), %lld MB, moved to the fast disk", f, b >> 20);
}
