// vfile.c — files read straight out of the player's .ipa, never unpacked (the Infinity Blade ports).
// The PS5 lets an app write ~1.1-1.3 GB fast, then slows its writes to a crawl (IB3's install:
// 120 MB/s, then 32 -> 11 MB/s, and a new process doesn't reset it). IB3's three texture caches
// (Textures / CharTextures / Lighting .tfc: 1.5 GB of its 2.87) stay inside the .ipa; the game
// opens them through Hydroid's open / read / pread / lseek / fstat / close (linux_abi.c,
// libc_extra.c ask here first), and reads are inflated from the nearest access point.
//
// Random access into a deflate stream (the zlib "zran" way): at install, each such file is inflated
// once and every ~2 MB, at a deflate block boundary, its place is kept - the uncompressed and
// compressed offsets, the bits of the byte before, and the last 32 KB of output (the dictionary).
// A read takes the pieces it needs (point k to k+1, inflated from point k: inflatePrime,
// inflateSetDictionary), kept in a shared cache; the next piece is inflated ahead on a thread.
// The index: <game>/game/vfile-index.bin.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "android_shim.h"

#define WIN 32768
#define SPAN (2u << 20)
#define IN_CHUNK (256 * 1024)
#define MAX_FD 4096

typedef struct {
    uint64_t out, in; // uncompressed offset; the .ipa offset of the first whole byte after it
    uint8_t bits;     // bits of the byte before still to use (0-7)
    uint8_t window[WIN];
} vpoint;

typedef struct {
    char name[256];   // in the .ipa ("Payload/SwordGame.app/CookedIPhone/Textures.tfc")
    char path[600];   // where the game looks for it (<game>/game/<name>)
    uint64_t data_off, csize, usize;
    uint32_t npoints;
    vpoint *points;
} vfile_entry;

typedef struct {
    vfile_entry *e;
    pthread_mutex_t lock;
    uint64_t pos; // read / lseek's position
} vfd;

static char ipa_path[512];
static vfile_entry *files;
static int nfiles;
static int state = -1; // -1 not looked at yet, 0 none, 1 on
static vfd *fds[MAX_FD];
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;

static int read_at(int fd, void *buf, size_t n, off_t off) {
    for (size_t got = 0; got < n;) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, off + (off_t)got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

// ---- the index, made at install --------------------------------------------------------------
// inflate e (its data at data_off in the .ipa, open as fd) once, keeping a point every SPAN bytes
static int build_points(int fd, vfile_entry *e) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return -1;
    uint8_t *in = malloc(IN_CHUNK), *win = malloc(WIN);
    uint32_t cap = (uint32_t)(e->usize / SPAN) + 4;
    e->points = malloc(sizeof(vpoint) * cap);
    e->npoints = 0;
    if (!in || !win || !e->points) { free(in); free(win); inflateEnd(&s); return -1; }
    vpoint *p0 = &e->points[e->npoints++]; // the start: no dictionary
    p0->out = 0, p0->in = e->data_off, p0->bits = 0;
    uint64_t totin = 0, totout = 0, last = 0, fed = 0;
    int ret = Z_OK;
    s.avail_out = 0;
    while (ret != Z_STREAM_END) {
        if (!s.avail_in && fed < e->csize) {
            const size_t n = e->csize - fed < IN_CHUNK ? (size_t)(e->csize - fed) : IN_CHUNK;
            if (read_at(fd, in, n, (off_t)(e->data_off + fed))) { ret = Z_ERRNO; break; }
            fed += n;
            s.next_in = in, s.avail_in = (uInt)n;
        }
        do {
            if (!s.avail_out) s.avail_out = WIN, s.next_out = win;
            totin += s.avail_in, totout += s.avail_out;
            ret = inflate(&s, Z_BLOCK);
            totin -= s.avail_in, totout -= s.avail_out;
            if (ret == Z_NEED_DICT || (ret == Z_BUF_ERROR && fed >= e->csize && !s.avail_in)) ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR || ret == Z_STREAM_END) break;
            if ((s.data_type & 128) && !(s.data_type & 64) && totout - last >= SPAN && e->npoints < cap) {
                vpoint *p = &e->points[e->npoints++];
                p->out = totout, p->in = e->data_off + totin, p->bits = (uint8_t)(s.data_type & 7);
                const unsigned left = s.avail_out; // the window: the last 32 KB of output, in order
                if (left) memcpy(p->window, win + WIN - left, left);
                if (left < WIN) memcpy(p->window + left, win, WIN - left);
                last = totout;
            }
        } while (s.avail_in);
        if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR || ret == Z_ERRNO) break;
    }
    inflateEnd(&s);
    free(in);
    free(win);
    if (ret != Z_STREAM_END || totout != e->usize) {
        droid_log("vfile: %s: its stream didn't read (%d, %llu of %llu bytes)", e->name, ret,
                  (unsigned long long)totout, (unsigned long long)e->usize);
        return -1;
    }
    return 0;
}

typedef struct { const char *ipa; vfile_entry *e; int rc; } build_job;
static void *build_thread(void *arg) {
    build_job *b = arg;
    int fd = open(b->ipa, O_RDONLY);
    b->rc = fd >= 0 ? build_points(fd, b->e) : -1;
    if (fd >= 0) close(fd);
    return NULL;
}

// (ipa_install.c) the files to keep in the .ipa: name, local header offset, sizes; the index
// written to index_path. 0 = ok
int vfile_build_index(const char *ipa, const char *index_path, int n, const char *const *names,
                      const uint32_t *loffs, const uint32_t *csizes, const uint32_t *usizes) {
    int fd = open(ipa, O_RDONLY);
    if (fd < 0) return -1;
    vfile_entry *es = calloc((size_t)n, sizeof(*es));
    build_job *jobs = calloc((size_t)n, sizeof(*jobs));
    pthread_t *th = calloc((size_t)n, sizeof(*th));
    if (!es || !jobs || !th) { close(fd); free(es); free(jobs); free(th); return -1; }
    for (int i = 0; i < n; i++) {
        uint8_t lh[30];
        if (read_at(fd, lh, 30, loffs[i]) || lh[0] != 'P' || lh[1] != 'K') { close(fd); free(es); free(jobs); free(th); return -1; }
        snprintf(es[i].name, sizeof(es[i].name), "%s", names[i]);
        es[i].data_off = (uint64_t)loffs[i] + 30 + (lh[26] | lh[27] << 8) + (lh[28] | lh[29] << 8);
        es[i].csize = csizes[i], es[i].usize = usizes[i];
        jobs[i] = (build_job){ ipa, &es[i], -1 };
    }
    close(fd);
    for (int i = 0; i < n; i++) // (one thread a file: each is one stream)
        if (pthread_create(&th[i], NULL, build_thread, &jobs[i]) != 0) build_thread(&jobs[i]), th[i] = 0;
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (th[i]) pthread_join(th[i], NULL);
        if (jobs[i].rc) rc = -1;
    }
    FILE *f = rc ? NULL : fopen(index_path, "wb");
    if (!rc && !f) rc = -1;
    if (f) {
        fwrite("HVF1", 1, 4, f);
        fwrite(&n, 4, 1, f);
        for (int i = 0; i < n && !rc; i++) {
            const vfile_entry *e = &es[i];
            if (fwrite(e->name, sizeof(e->name), 1, f) != 1 || fwrite(&e->data_off, 8, 1, f) != 1 ||
                fwrite(&e->csize, 8, 1, f) != 1 || fwrite(&e->usize, 8, 1, f) != 1 ||
                fwrite(&e->npoints, 4, 1, f) != 1 || fwrite(e->points, sizeof(vpoint), e->npoints, f) != e->npoints)
                rc = -1;
        }
        if (fclose(f) != 0) rc = -1;
        if (rc) unlink(index_path);
    }
    uint64_t pts = 0;
    for (int i = 0; i < n; i++) pts += es[i].npoints, free(es[i].points);
    droid_log("vfile: index of %d file(s), %llu points (%llu MB): %s", n, (unsigned long long)pts,
              (unsigned long long)(pts * sizeof(vpoint) >> 20), rc ? "failed" : "written");
    free(es);
    free(jobs);
    free(th);
    return rc;
}

// ---- at run time ------------------------------------------------------------------------------
// the index of this game, once: <game_dir>/game/vfile-index.bin, the .ipa in /app0/game
int droid_game_in(const char *list);
static int vfile_on(void) {
    if (state >= 0) return state;
    pthread_mutex_lock(&init_lock);
    if (state < 0) {
        state = 0;
        extern char game_dir[256];
        char index[600];
        snprintf(index, sizeof(index), "%s/game/vfile-index.bin", game_dir);
        FILE *f = droid_game_in("com.ib3port.,com.ib2port.,com.ib1port.") ? fopen(index, "rb") : NULL;
        FILE *m = NULL;
        if (f) { // the .ipa it was made from (the install's marker names it)
            char marker[600];
            snprintf(marker, sizeof(marker), "%s/game/ipa-installed.txt", game_dir);
            m = fopen(marker, "r");
            if (m && fgets(ipa_path, sizeof(ipa_path), m)) ipa_path[strcspn(ipa_path, "\r\n")] = 0;
            if (m) fclose(m);
        }
        char magic[4];
        int n = 0;
        if (f && ipa_path[0] && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "HVF1", 4) && fread(&n, 4, 1, f) == 1 && n > 0 &&
            n < 64 && (files = calloc((size_t)n, sizeof(*files)))) {
            int ok = 1;
            for (int i = 0; i < n && ok; i++) {
                vfile_entry *e = &files[i];
                ok = fread(e->name, sizeof(e->name), 1, f) == 1 && fread(&e->data_off, 8, 1, f) == 1 &&
                     fread(&e->csize, 8, 1, f) == 1 && fread(&e->usize, 8, 1, f) == 1 && fread(&e->npoints, 4, 1, f) == 1 &&
                     e->npoints && (e->points = malloc(sizeof(vpoint) * e->npoints)) &&
                     fread(e->points, sizeof(vpoint), e->npoints, f) == e->npoints;
                snprintf(e->path, sizeof(e->path), "%s/game/%s", game_dir, e->name);
            }
            if (ok) {
                nfiles = n, state = 1;
                droid_log("vfile: %d file(s) read straight from %s", n, ipa_path);
            } else {
                droid_log("vfile: %s unreadable: the files are looked for on the disk", index);
            }
        }
        if (f) fclose(f);
    }
    pthread_mutex_unlock(&init_lock);
    return state;
}

static vfile_entry *by_path(const char *path) {
    if (!path || vfile_on() != 1) return NULL;
    for (int i = 0; i < nfiles; i++)
        if (!strcasecmp(files[i].path, path)) return &files[i]; // (iOS paths: any case)
    return NULL;
}
int64_t vfile_path_size(const char *abs_path) { // a virtual file's size, or -1
    vfile_entry *e = by_path(abs_path);
    return e ? (int64_t)e->usize : -1;
}
const char *vfile_ipa(void) { return ipa_path; }

int vfile_open(const char *abs_path, int flags) { // a virtual file: its fd; else -1 (not ours)
    vfile_entry *e = by_path(abs_path);
    if (!e) return -1;
    if ((flags & 3) != 0) { errno = EROFS; return -2; } // (read only)
    int fd = open(ipa_path, O_RDONLY);
    if (fd < 0 || fd >= MAX_FD) { if (fd >= 0) close(fd); errno = EMFILE; return -2; }
    vfd *v = calloc(1, sizeof(*v));
    if (!v) { close(fd); errno = ENOMEM; return -2; }
    v->e = e;
    pthread_mutex_init(&v->lock, NULL);
    __atomic_store_n(&fds[fd], v, __ATOMIC_RELEASE);
    static int told;
    if (told++ < 8) droid_log("vfile: open %s -> fd %d (%llu MB, from the .ipa)", e->name, fd, (unsigned long long)(e->usize >> 20));
    return fd;
}
int vfile_owns(int fd) { return fd >= 0 && fd < MAX_FD && __atomic_load_n(&fds[fd], __ATOMIC_ACQUIRE) != NULL; }

// ---- reads: whole pieces (point k to point k+1, ~2 MB) inflated once, kept in a shared cache ----
// (a read used to inflate from its point every time: IB3's first start read its textures ~35 s
// slower than from the disk; a texture's neighbours are in the same piece)
#define CACHE_SLOTS 64 // up to ~130 MB
typedef struct { const vfile_entry *e; uint32_t k, len; uint8_t *data; uint64_t used; } piece;
static piece cache[CACHE_SLOTS];
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ahead_cv = PTHREAD_COND_INITIALIZER;
static uint64_t tick, hits, misses, inflate_us;
static const vfile_entry *ahead_e; // the piece the read-ahead thread inflates next
static uint32_t ahead_k;
static int ahead_on;

static uint32_t point_at(const vfile_entry *e, uint64_t off) { // the last point with out <= off
    uint32_t lo = 0, hi = e->npoints;
    while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (e->points[mid].out <= off) lo = mid; else hi = mid;
    }
    return lo;
}
static uint32_t piece_len(const vfile_entry *e, uint32_t k) {
    return (uint32_t)((k + 1 < e->npoints ? e->points[k + 1].out : e->usize) - e->points[k].out);
}
static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static uint8_t *inflate_piece(int fd, const vfile_entry *e, uint32_t k, uint32_t len) {
    const vpoint *p = &e->points[k];
    uint8_t *out = malloc(len), *in = malloc(IN_CHUNK);
    z_stream z;
    memset(&z, 0, sizeof(z));
    const double t0 = now_us();
    int ok = out && in && inflateInit2(&z, -MAX_WBITS) == Z_OK;
    if (ok && p->bits) {
        uint8_t b;
        ok = !read_at(fd, &b, 1, (off_t)p->in - 1);
        if (ok) inflatePrime(&z, p->bits, b >> (8 - p->bits));
    }
    if (ok && p->out) inflateSetDictionary(&z, p->window, WIN);
    uint64_t at = p->in;
    const uint64_t end = e->data_off + e->csize;
    z.next_out = out, z.avail_out = len;
    while (ok && z.avail_out) {
        if (!z.avail_in) {
            if (at >= end) { ok = 0; break; }
            const size_t n = end - at < IN_CHUNK ? (size_t)(end - at) : IN_CHUNK;
            if (read_at(fd, in, n, (off_t)at)) { ok = 0; break; }
            at += n;
            z.next_in = in, z.avail_in = (uInt)n;
        }
        const int r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_STREAM_END) { ok = !z.avail_out; break; }
        if (r != Z_OK) ok = 0;
    }
    inflateEnd(&z);
    free(in);
    if (!ok) {
        free(out);
        droid_log("vfile: %s piece %u didn't inflate", e->name, k);
        return NULL;
    }
    __atomic_add_fetch(&inflate_us, (uint64_t)(now_us() - t0), __ATOMIC_RELAXED);
    return out;
}
static piece *find_locked(const vfile_entry *e, uint32_t k) {
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (cache[i].data && cache[i].e == e && cache[i].k == k) return &cache[i];
    return NULL;
}
static piece *insert_locked(const vfile_entry *e, uint32_t k, uint8_t *data, uint32_t len) {
    piece *have = find_locked(e, k);
    if (have) { free(data); return have; } // (made by another thread meanwhile)
    piece *old = &cache[0];
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!cache[i].data) { old = &cache[i]; break; }
        if (cache[i].used < old->used) old = &cache[i];
    }
    free(old->data);
    *old = (piece){ e, k, len, data, ++tick };
    return old;
}
static void *ahead_thread(void *arg) { // inflates the piece after the one just read
    (void)arg;
    int fd = open(ipa_path, O_RDONLY);
    pthread_mutex_lock(&cache_lock);
    for (;;) {
        while (!ahead_e) pthread_cond_wait(&ahead_cv, &cache_lock);
        const vfile_entry *e = ahead_e;
        const uint32_t k = ahead_k;
        ahead_e = NULL;
        if (find_locked(e, k) || fd < 0) continue;
        pthread_mutex_unlock(&cache_lock);
        const uint32_t len = piece_len(e, k);
        uint8_t *d = inflate_piece(fd, e, k, len);
        pthread_mutex_lock(&cache_lock);
        if (d) insert_locked(e, k, d, len)->used = tick; // (not newer than what was read)
    }
    return NULL;
}

static ssize_t read_pieces(int fd, vfd *v, void *buf, size_t n, uint64_t off);
// every read timed: slow ones logged, and a summary every 5 s of reading (how the .ipa holds up)
static ssize_t read_into(int fd, vfd *v, void *buf, size_t n, uint64_t off) {
    static uint64_t calls, bytes, us_all, last_say;
    static int slow_told;
    const double t0 = now_us();
    const ssize_t r = read_pieces(fd, v, buf, n, off);
    const uint64_t us = (uint64_t)(now_us() - t0);
    __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&bytes, r > 0 ? (uint64_t)r : 0, __ATOMIC_RELAXED);
    const uint64_t all = __atomic_add_fetch(&us_all, us, __ATOMIC_RELAXED);
    if (us > 100000 && slow_told < 30) {
        slow_told++;
        droid_log("vfile: slow read %s at %llu, %zu bytes: %llu ms", v->e->name, (unsigned long long)off, n,
                  (unsigned long long)(us / 1000));
    }
    const uint64_t now = (uint64_t)t0;
    if (now - __atomic_load_n(&last_say, __ATOMIC_RELAXED) > 5000000) {
        __atomic_store_n(&last_say, now, __ATOMIC_RELAXED);
        droid_log("vfile: %llu reads, %llu MB, %llu ms reading in all (%llu pieces inflated, %llu ms; %llu from the cache)",
                  (unsigned long long)calls, (unsigned long long)(bytes >> 20), (unsigned long long)(all / 1000),
                  (unsigned long long)misses, (unsigned long long)(inflate_us / 1000), (unsigned long long)hits);
    }
    return r;
}
static ssize_t read_pieces(int fd, vfd *v, void *buf, size_t n, uint64_t off) {
    const vfile_entry *e = v->e;
    if (off >= e->usize || !n) return 0;
    if (n > e->usize - off) n = (size_t)(e->usize - off);
    size_t done = 0;
    while (done < n) {
        const uint64_t at = off + done;
        const uint32_t k = point_at(e, at), len = piece_len(e, k);
        const uint64_t start = e->points[k].out;
        size_t take = (size_t)(start + len - at);
        if (take > n - done) take = n - done;
        pthread_mutex_lock(&cache_lock);
        piece *pc = find_locked(e, k);
        if (pc) {
            hits++;
        } else {
            misses++;
            pthread_mutex_unlock(&cache_lock);
            uint8_t *d = inflate_piece(fd, e, k, len);
            if (!d) { errno = EIO; return done ? (ssize_t)done : -1; }
            pthread_mutex_lock(&cache_lock);
            pc = insert_locked(e, k, d, len);
        }
        pc->used = ++tick;
        memcpy((uint8_t *)buf + done, pc->data + (at - start), take);
        if (k + 1 < e->npoints && !find_locked(e, k + 1)) { // the next piece: made meanwhile
            if (!ahead_on) {
                pthread_t t;
                ahead_on = pthread_create(&t, NULL, ahead_thread, NULL) == 0 ? 1 : -1;
                if (ahead_on == 1) pthread_detach(t);
            }
            ahead_e = e, ahead_k = k + 1;
            pthread_cond_signal(&ahead_cv);
        }
        pthread_mutex_unlock(&cache_lock);
        done += take;
    }
    return (ssize_t)done;
}

ssize_t vfile_pread(int fd, void *buf, size_t n, int64_t off) {
    vfd *v = fds[fd];
    if (off < 0) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&v->lock);
    const ssize_t r = read_into(fd, v, buf, n, (uint64_t)off);
    pthread_mutex_unlock(&v->lock);
    return r;
}
ssize_t vfile_read(int fd, void *buf, size_t n) {
    vfd *v = fds[fd];
    pthread_mutex_lock(&v->lock);
    const ssize_t r = read_into(fd, v, buf, n, v->pos);
    if (r > 0) v->pos += (uint64_t)r;
    pthread_mutex_unlock(&v->lock);
    return r;
}
int64_t vfile_lseek(int fd, int64_t off, int whence) {
    vfd *v = fds[fd];
    pthread_mutex_lock(&v->lock);
    int64_t at = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)v->pos + off
               : whence == SEEK_END ? (int64_t)v->e->usize + off : -1;
    if (at < 0) errno = EINVAL, at = -1;
    else v->pos = (uint64_t)at;
    pthread_mutex_unlock(&v->lock);
    return at;
}
int64_t vfile_size(int fd) { return (int64_t)fds[fd]->e->usize; }
int vfile_close(int fd) {
    vfd *v = __atomic_exchange_n(&fds[fd], NULL, __ATOMIC_ACQ_REL);
    if (!v) return close(fd);
    pthread_mutex_destroy(&v->lock);
    free(v);
    return close(fd);
}
