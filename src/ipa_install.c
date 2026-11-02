// ipa_install.c — the Infinity Blade 2/3 ports' game files from the player's own .ipa.
// On Android the port's launcher (Java, LauncherActivity) unpacks Payload/SwordGame.app from the
// .ipa into its files folder (game/); Hydroid runs the native game only, so it does that here, at
// the first start: the .ipa in the app's game folder (/app0/game, next to the port's APK), its
// SwordGame.app unpacked into <the game's folder>/game/.
//
// Fast: the PS5 wrote ~1.1 GB at 106-190 MB/s, then 32 -> 11 MB/s (IB3's first installs), and a
// restart didn't help. So: (1) the three big .tfc texture caches (1.5 GB of IB3's 2.87) aren't
// unpacked at all: vfile.c lets the game read them out of the .ipa, from an index made here
// (game/vfile-index.bin); (2) every file is flushed every 32 MB and when done, as Orbit Store does,
// so no GB of unwritten data piles up. 4 workers, each with its own handle on the .ipa (pread),
// unpack whole files, biggest first; a finished file (its full size there) is skipped, so a closed
// install goes on where it was. The marker (game/ipa-installed.txt, the .ipa's path) is written last.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "android_shim.h"

#define APP_PREFIX "Payload/SwordGame.app/"
#define IN_CHUNK (1 << 20)
#define OUT_CHUNK (4 << 20)
#define WORKERS 4
#define SYNC_EVERY (32u << 20) // flushed this often, as Orbit Store does (no GB of unwritten data piling up)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}
static void mkdirs(char *path) { // every folder of path (its last part is the file)
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
}
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int read_at(int fd, void *buf, size_t n, off_t off) {
    for (size_t got = 0; got < n;) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, off + (off_t)got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static int write_all(int fd, const void *buf, size_t n, uint32_t *unsynced) {
    if ((*unsynced += (uint32_t)n) >= SYNC_EVERY) *unsynced = 0, fsync(fd);
    for (size_t put = 0; put < n;) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

typedef struct {
    char dest[1024];
    uint32_t loff, csize, usize;
    uint16_t method;
} ipa_entry;

typedef struct {
    const char *ipa;
    ipa_entry **todo; // this run's files
    uint32_t n;
    volatile uint32_t next;   // the next one to take
    volatile uint64_t done;   // bytes written this run
    volatile int failed;      // -2 read, -3 write
} job;

// one entry: its local header, then stored bytes or a raw deflate stream, into e->dest
static int extract(int in_fd, const ipa_entry *e, uint8_t *in, uint8_t *out, job *j) {
    uint8_t lh[30];
    if (read_at(in_fd, lh, 30, e->loff) || rd32(lh) != 0x04034b50) return -2;
    off_t at = (off_t)e->loff + 30 + rd16(lh + 26) + rd16(lh + 28);
    int fd = open(e->dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -3;
    int rc = 0;
    uint32_t left = e->csize, unsynced = 0;
    if (e->method == 0) {
        while (left && !rc) {
            size_t n = left < OUT_CHUNK ? left : OUT_CHUNK;
            if (read_at(in_fd, out, n, at)) rc = -2;
            else if (write_all(fd, out, n, &unsynced)) rc = -3;
            at += (off_t)n, left -= (uint32_t)n;
            __atomic_add_fetch(&j->done, n, __ATOMIC_RELAXED);
        }
    } else if (e->method == 8) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) rc = -2;
        int zr = Z_OK;
        while (!rc && zr != Z_STREAM_END) {
            if (!z.avail_in && left) {
                size_t n = left < IN_CHUNK ? left : IN_CHUNK;
                if (read_at(in_fd, in, n, at)) { rc = -2; break; }
                at += (off_t)n, left -= (uint32_t)n;
                z.next_in = in, z.avail_in = (uInt)n;
            }
            z.next_out = out, z.avail_out = OUT_CHUNK;
            zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) { rc = -2; break; }
            size_t got = OUT_CHUNK - z.avail_out;
            if (got && write_all(fd, out, got, &unsynced)) rc = -3;
            __atomic_add_fetch(&j->done, got, __ATOMIC_RELAXED);
            if (zr == Z_OK && !z.avail_in && !left && !got) { rc = -2; break; } // (cut short)
        }
        inflateEnd(&z);
    } else {
        rc = -2;
    }
    if (!rc && fsync(fd) != 0) rc = -3;
    if (close(fd) != 0 && !rc) rc = -3;
    if (rc) unlink(e->dest); // (half a file: done again)
    return rc;
}

static void *worker(void *arg) {
    job *j = arg;
    int fd = open(j->ipa, O_RDONLY);
    uint8_t *in = malloc(IN_CHUNK), *out = malloc(OUT_CHUNK);
    if (fd < 0 || !in || !out) {
        j->failed = -2;
    } else {
        for (;;) {
            const uint32_t i = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
            if (i >= j->n || j->failed) break;
            const int r = extract(fd, j->todo[i], in, out, j);
            if (r) {
                droid_log("ipa: %s not unpacked (%d, errno %d)", j->todo[i]->dest, r, errno);
                j->failed = r;
            }
        }
    }
    if (fd >= 0) close(fd);
    free(in);
    free(out);
    return NULL;
}

static int by_size_down(const void *a, const void *b) {
    const ipa_entry *x = a, *y = b;
    return x->usize < y->usize ? 1 : x->usize > y->usize ? -1 : 0;
}
static int finished(const ipa_entry *e) { // its whole size there: unpacked before
    struct stat st;
    return stat(e->dest, &st) == 0 && (uint64_t)st.st_size == e->usize;
}

typedef struct { const char *ipa, *index; int n; const char **names; uint32_t *lo, *cs, *us; int rc; } index_job_t;
int vfile_build_index(const char *ipa, const char *index_path, int n, const char *const *names,
                      const uint32_t *loffs, const uint32_t *csizes, const uint32_t *usizes);
void *index_thread(void *arg) {
    index_job_t *x = arg;
    x->rc = vfile_build_index(x->ipa, x->index, x->n, x->names, x->lo, x->cs, x->us);
    return NULL;
}

// 0: installed (now or before), -1: no .ipa, -2: the .ipa can't be read, -3: no room / can't write.
// relaunch: not used now (a restart didn't make the PS5 write fast again).
int ipa_install(const char *game_dir, void (*say_progress)(const char *text), int (*relaunch)(void)) {
    char marker[512], probe[512];
    snprintf(marker, sizeof(marker), "%s/game/ipa-installed.txt", game_dir);
    snprintf(probe, sizeof(probe), "%s/game/" APP_PREFIX "CookedIPhone/Engine.xxx", game_dir);
    if (exists(marker)) return 0;
    // the .ipa: /app0/game/*.ipa
    char ipa[512] = "";
    DIR *d = opendir(APP0 "/game");
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".ipa")) {
            snprintf(ipa, sizeof(ipa), APP0 "/game/%s", e->d_name);
            break;
        }
    }
    if (d) closedir(d);
    if (!ipa[0]) return exists(probe) ? 0 : -1; // (put in by hand before: as it is)
    int fd = open(ipa, O_RDONLY);
    if (fd < 0) return -2;
    // the end of the central directory (in the last 64 KB)
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -2; }
    const off_t size = st.st_size, tail = size < 65557 ? size : 65557;
    uint8_t *t = malloc((size_t)tail);
    if (!t || read_at(fd, t, (size_t)tail, size - tail)) { free(t); close(fd); return -2; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); close(fd); return -2; }
    const uint32_t entries = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
    free(t);
    if (entries == 0xFFFF || cd_off == 0xFFFFFFFFu) { close(fd); droid_log("ipa: a zip64 .ipa (not read yet)"); return -2; }
    uint8_t *cd = malloc(cd_size);
    if (!cd || read_at(fd, cd, cd_size, cd_off)) { free(cd); close(fd); return -2; }
    close(fd);
    // the game's files, biggest first; every folder made now (the workers then only write files)
    ipa_entry *all = calloc(entries, sizeof(ipa_entry));
    if (!all) { free(cd); return -2; }
    uint32_t n = 0;
    uint64_t total = 0;
    const size_t pl = strlen(APP_PREFIX);
    for (uint32_t i = 0, p = 0; i < entries && p + 46 <= cd_size; i++) {
        const uint16_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        const char *name = (const char *)cd + p + 46;
        if (nl > pl && !memcmp(name, APP_PREFIX, pl) && name[nl - 1] != '/') {
            ipa_entry *e = &all[n++];
            snprintf(e->dest, sizeof(e->dest), "%s/game/%.*s", game_dir, (int)nl, name);
            e->method = rd16(cd + p + 10), e->csize = rd32(cd + p + 20), e->usize = rd32(cd + p + 24);
            e->loff = rd32(cd + p + 42);
            total += e->usize;
        }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    if (!n) { free(all); return -2; }
    qsort(all, n, sizeof(*all), by_size_down);
    ipa_entry **todo = malloc(sizeof(*todo) * n);
    if (!todo) { free(all); return -2; }
    // the big deflated .tfc texture caches stay in the .ipa (vfile.c reads them from it): their
    // index is made while the rest is unpacked. Without an index they're unpacked like the rest.
    char index[600];
    snprintf(index, sizeof(index), "%s/game/vfile-index.bin", game_dir);
    const char *vnames[16];
    uint32_t vloff[16], vcs[16], vus[16];
    int nv = 0, index_rc = exists(index) ? 0 : 1;
    uint64_t kept = 0;
    const size_t gl = strlen(game_dir) + strlen("/game/");
    for (uint32_t i = 0; i < n && nv < 16; i++) {
        const size_t dl = strlen(all[i].dest);
        if (all[i].method == 8 && dl > 4 && !strcasecmp(all[i].dest + dl - 4, ".tfc") && all[i].usize >= (64u << 20)) {
            vnames[nv] = all[i].dest + gl, vloff[nv] = all[i].loff, vcs[nv] = all[i].csize, vus[nv] = all[i].usize;
            all[i].method = 0xFFFF; // (not unpacked)
            kept += all[i].usize;
            nv++;
        }
    }
    index_job_t ij = { ipa, index, nv, vnames, vloff, vcs, vus, 1 };
    pthread_t ith;
    int ith_on = 0;
    if (nv && index_rc) ith_on = pthread_create(&ith, NULL, index_thread, &ij) == 0;
    // the rest: what isn't there yet, biggest first
    uint64_t before = 0, planned = 0;
    job j = { .ipa = ipa, .todo = todo };
    for (uint32_t i = 0; i < n; i++) {
        if (all[i].method == 0xFFFF) continue;
        if (finished(&all[i])) { before += all[i].usize; continue; }
        mkdirs(all[i].dest);
        todo[j.n++] = &all[i];
        planned += all[i].usize;
    }
    total -= kept;
    droid_log("ipa: %u of the %u files to unpack (%llu MB), %llu MB already there; %d file(s) (%llu MB) read from the .ipa; %d at a time",
              j.n, n, (unsigned long long)(planned >> 20), (unsigned long long)(before >> 20), nv,
              (unsigned long long)(kept >> 20), WORKERS);
    const double t0 = now_s();
    pthread_t w[WORKERS];
    int started = 0;
    for (int i = 0; j.n && i < WORKERS; i++)
        if (pthread_create(&w[i], NULL, worker, &j) == 0) started++;
    if (j.n && !started) worker(&j);
    int last = -1;
    uint64_t logged = 0;
    while (j.n && __atomic_load_n(&j.next, __ATOMIC_RELAXED) < j.n + (uint32_t)started && !j.failed) {
        const uint64_t now = __atomic_load_n(&j.done, __ATOMIC_RELAXED);
        const int pct = (int)((before + now) * 100 / (total ? total : 1));
        if (pct / 10 != last / 10 && say_progress && pct < 100) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Installing the game from your .ipa: %d%%", pct);
            say_progress(msg);
            last = pct;
        }
        if (now >= logged + (200ull << 20)) { // the speed, every 200 MB
            logged = now;
            const double sec = now_s() - t0;
            droid_log("ipa: %llu MB written in %.1f s (%.0f MB/s)", (unsigned long long)(now >> 20), sec,
                      sec > 0 ? (double)(now >> 20) / sec : 0.0);
        }
        usleep(200000);
    }
    for (int i = 0; i < started; i++) pthread_join(w[i], NULL);
    const double secs = now_s() - t0;
    droid_log("ipa: wrote %llu MB in %.1f s (%.0f MB/s)", (unsigned long long)(j.done >> 20), secs,
              secs > 0 ? (double)(j.done >> 20) / secs : 0.0);
    if (ith_on) {
        if (say_progress) say_progress("Installing the game from your .ipa: almost done");
        pthread_join(ith, NULL);
        index_rc = ij.rc;
        droid_log("ipa: the .tfc index %s at %.1f s", index_rc ? "FAILED" : "made", now_s() - t0);
    }
    int rc = j.failed ? (j.failed == -3 ? -3 : -2) : 0;
    if (!rc && nv && index_rc) { // no index: unpack those after all
        droid_log("ipa: the .tfc files are unpacked instead");
        j = (job){ .ipa = ipa, .todo = todo };
        for (uint32_t i = 0; i < n; i++)
            if (all[i].method == 0xFFFF) {
                all[i].method = 8;
                if (!finished(&all[i])) mkdirs(all[i].dest), todo[j.n++] = &all[i];
            }
        worker(&j);
        if (j.failed) rc = j.failed == -3 ? -3 : -2;
    }
    (void)relaunch;
    free(todo);
    free(all);
    if (rc) return rc;
    FILE *m = fopen(marker, "w");
    if (m) {
        fprintf(m, "%s\n", ipa);
        fclose(m);
    }
    droid_log("ipa: installed (%u files, %llu MB)", n, (unsigned long long)(total >> 20));
    if (say_progress) say_progress("Installing the game from your .ipa: done");
    return 0;
}
