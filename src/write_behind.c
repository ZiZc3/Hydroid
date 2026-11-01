// write_behind.c — the game's writes to a file it opened write-only, gathered into 4 MB and
// written by a thread of ours: the game goes on at once. The console takes ~14 ms a write in
// the download data (Shadow Fight 4's extraction: 57 825 writes, 827 s of waiting); Overdrive
// writes 16-32 MB at a time without waiting either. Everything waiting is written before the
// game closes, syncs, seeks, sizes, maps or duplicates the file, or opens it again.
// DROID_DIRECT_WRITES=1: every write as it comes (the old way).

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "game_settings.h"

void droid_log(const char *fmt, ...);
// big_files.c: a file grown past BIG_AT moves to the disk that writes big files fast
int big_candidate(const char *path);
int big_promote(int fd, const char *path, char *to, size_t to_n, int *copy);
void big_closed(const char *path);
void big_renamed(const char *a, const char *b);
const char *big_map(const char *path, char *buf, size_t n);
#define BIG_AT (2u << 20)

#define WB_FDS 4096
#define WB_CAP (16u << 20)       // a buffer: 16 MB (the console writes big pieces far faster)
#define WB_MAX_PENDING (128 << 20) // waiting for the disk at most: then the game waits too (the console's
                                   // disk: ~5 MB/s a writer while Shadow Fight 4 unpacks; 64 MB was full)
#define WB_THREADS 2              // (settings.txt writers=, default 1) a file's writes always on the same one: in order

typedef struct wb_job {
    struct wb_job *next;
    int fd;
    char *data;
    size_t len;
    char *promote; // not data: the file (this path) moved to the fast disk, in its turn
    int close;     // not data: the file closed, in its turn (wb_close)
} wb_job;

typedef struct {
    char *buf;
    size_t len;
    int pending;    // jobs not written yet
    int err;        // a write that failed (errno)
    char path[256]; // to write it out when the file is opened again
    long long total; // written so far
    int moved;      // to the fast disk (big_files.c): asked; 2 done
    char moved_from[256];
    long long pos;  // where the game's next write goes (its lseeks answered here); -1 unknown
    int closing;    // closed by the game: our writer closes it after its writes
    int out;        // moved: the copy's fd, where its writes go (-1: the game's fd)
    struct stat st; // as it was opened (its stat answered from here while it is written)
    int have_st;
} wb_file;

static wb_file *files[WB_FDS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER; // a job done, or room again
static struct {
    wb_job *head, *tail;
    pthread_cond_t work;
    pthread_t thread;
} queues[WB_THREADS];
static long long pending_bytes;
static int started, off = -1;
static long long stats_jobs, stats_bytes;
static int hi_fd = -1; // the highest number with a buffer (the path lookups go up to it)
static int async_closes;
// a move to the fast disk and a rename take turns: a rename in the middle of the copy left it
// reading the old name (Shadow Fight 4: "x_extracting" renamed "x" as it was copied, errno 2)
static pthread_mutex_t move_lock = PTHREAD_MUTEX_INITIALIZER;
void wb_rename_begin(void) { pthread_mutex_lock(&move_lock); }
void wb_rename_end(void) { pthread_mutex_unlock(&move_lock); }

// how many writers take files (settings.txt writers=1 or 2; 1: one stream to the disk, as
// the console writes fastest)
static int writer_count(void) { return settings.writers >= 2 ? 2 : 1; }

static void *writer(void *arg) {
    int q = (int)(intptr_t)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!queues[q].head) pthread_cond_wait(&queues[q].work, &lock);
        wb_job *j = queues[q].head;
        queues[q].head = j->next;
        if (!queues[q].head) queues[q].tail = NULL;
        const wb_file *jf = j->fd >= 0 && j->fd < WB_FDS ? files[j->fd] : NULL;
        const int to_fd = jf && jf->out >= 0 ? jf->out : j->fd; // moved: the copy
        pthread_mutex_unlock(&lock);
        size_t done = 0;
        int err = 0;
        if (j->close) { // everything before it written: the file closed, its number free again
            pthread_mutex_lock(&lock);
            wb_file *f = files[j->fd];
            files[j->fd] = NULL; // before the close: a new file can get the number after it
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&lock);
            if (f) {
                if (f->err) droid_log("write-behind: fd %d (%s) closed with a failed write (errno %d)", j->fd, f->path, f->err);
                if (f->moved == 2) big_closed(f->moved_from);
                if (f->out >= 0) close(f->out);
                free(f->buf);
                free(f);
            }
            close(j->fd);
            free(j);
            pthread_mutex_lock(&lock);
            continue;
        }
        if (j->promote) {
            char to[640], now[256];
            int copy = -1;
            pthread_mutex_lock(&move_lock); // no rename until it is moved (wb_rename_begin)
            pthread_mutex_lock(&lock); // its name now: renamed since it was asked (Shadow Fight 4: "x_extracting"
            const wb_file *pf = j->fd >= 0 && j->fd < WB_FDS ? files[j->fd] : NULL; // -> "x", errno 2)
            snprintf(now, sizeof(now), "%s", pf ? pf->path : j->promote);
            pthread_mutex_unlock(&lock);
            int ok = big_promote(j->fd, now, to, sizeof(to), &copy) == 0;
            pthread_mutex_lock(&lock);
            wb_file *f = j->fd >= 0 && j->fd < WB_FDS ? files[j->fd] : NULL;
            if (f && ok && strcmp(f->path, now) != 0) { // renamed while it was copied: its copy follows
                char renamed_to[256];
                snprintf(renamed_to, sizeof(renamed_to), "%s", f->path);
                pthread_mutex_unlock(&lock);
                big_renamed(now, renamed_to);
                big_map(renamed_to, to, sizeof(to));
                pthread_mutex_lock(&lock);
                f = j->fd >= 0 && j->fd < WB_FDS ? files[j->fd] : NULL;
            }
            if (f) {
                f->pending--;
                if (ok) { // a later open names the copy: what waits is written first then too
                    snprintf(f->moved_from, sizeof(f->moved_from), "%s", f->path);
                    snprintf(f->path, sizeof(f->path), "%s", to);
                    f->moved = 2;
                    f->out = copy;
                }
            } else if (copy >= 0) close(copy);
            pthread_mutex_unlock(&move_lock); // (the lock order: move_lock, then lock)
            pthread_cond_broadcast(&changed);
            free(j->promote);
            free(j);
            continue; // the lock held, as the loop wants it
        }
        struct timespec w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        while (done < j->len) {
            ssize_t r = write(to_fd, j->data + done, j->len - done);
            if (r < 0) {
                if (errno == EINTR) continue;
                err = errno;
                break;
            }
            done += (size_t)r;
        }
        clock_gettime(CLOCK_MONOTONIC, &w1);
        const double wms = (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6;
        pthread_mutex_lock(&lock);
        wb_file *f = j->fd >= 0 && j->fd < WB_FDS ? files[j->fd] : NULL;
        if (f) {
            f->pending--;
            if (err && !f->err) f->err = err;
        }
        static int slow_told;
        if (wms >= 300 && slow_told++ < 60) // the disk stalling: which file, how much
            droid_log("write-behind: %zu KB to %s took %.0f ms (%lld MB waiting)", j->len >> 10, f ? f->path : "?", wms,
                      pending_bytes >> 20);
        if (err) droid_log("write-behind: fd %d write failed (errno %d)", j->fd, err);
        pending_bytes -= (long long)j->len;
        stats_jobs++;
        stats_bytes += (long long)j->len;
        pthread_cond_broadcast(&changed);
        free(j->data);
        free(j);
    }
    return NULL;
}

static int wb_on(void) {
    // (Shadow Fight 3: off. It writes its bundles, then reads them back in a way that saw only what
    // had reached the disk, 44 bytes of each: "data extraction failed". Every write as it comes:
    // 0 errors, its extraction done, 2026-10-29. Shadow Fight 4, the reason for write-behind: on.)
    // Vector: the same with its downloaded event bundles ("Mismatching archive size ... Expected
    // 8672, got 44" in UnityCache/Temp/<hash>/__data, "Error download data", 2026-11-01): off.
    // Asked at each open: files are written before the game is known.
    if (droid_game_in("com.nekki.shadowfight3,com.nekki.vector")) return 0;
    if (off < 0) off = getenv("DROID_DIRECT_WRITES") != NULL;
    if (off) return 0;
    if (!started) {
        started = 1;
        for (int q = 0; q < WB_THREADS; q++) {
            pthread_cond_init(&queues[q].work, NULL);
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setstacksize(&a, 256 << 10);
            pthread_create(&queues[q].thread, &a, writer, (void *)(intptr_t)q);
            pthread_attr_destroy(&a);
        }
        droid_log("write-behind: on (%u MB buffers, %d writer thread(s) in use: settings.txt writers=)", WB_CAP >> 20, writer_count());
    }
    return 1;
}

// a buffer (or data) to the writer of its file; lock held
static void submit(int fd, char *data, size_t len) {
    while (pending_bytes > WB_MAX_PENDING) pthread_cond_wait(&changed, &lock); // the disk is behind
    wb_job *j = malloc(sizeof(*j));
    if (!j) { // no memory: written here
        const int to_fd = files[fd]->out >= 0 ? files[fd]->out : fd;
        pthread_mutex_unlock(&lock);
        size_t done = 0;
        while (done < len) {
            ssize_t r = write(to_fd, data + done, len - done);
            if (r <= 0) break;
            done += (size_t)r;
        }
        free(data);
        pthread_mutex_lock(&lock);
        return;
    }
    j->next = NULL;
    j->fd = fd;
    j->data = data;
    j->len = len;
    j->promote = NULL;
    j->close = 0;
    files[fd]->pending++;
    pending_bytes += (long long)len;
    int q = fd % writer_count();
    if (queues[q].tail) queues[q].tail->next = j;
    else queues[q].head = j;
    queues[q].tail = j;
    pthread_cond_signal(&queues[q].work);
}

// the file, past BIG_AT, to the fast disk: after what was written so far; lock held
static void submit_promote(int fd) {
    wb_file *f = files[fd];
    wb_job *j = malloc(sizeof(*j));
    if (!j || !(j->promote = strdup(f->path))) {
        free(j);
        return;
    }
    if (f->len) {
        char *b = f->buf;
        size_t l = f->len;
        f->buf = NULL;
        f->len = 0;
        submit(fd, b, l);
    }
    j->next = NULL;
    j->fd = fd;
    j->data = NULL;
    j->len = 0;
    j->close = 0;
    f->pending++;
    int q = fd % writer_count();
    if (queues[q].tail) queues[q].tail->next = j;
    else queues[q].head = j;
    queues[q].tail = j;
    pthread_cond_signal(&queues[q].work);
}

// the file's buffer to its writer, and everything written; lock held
static void drain(int fd) {
    wb_file *f = files[fd];
    if (!f) return;
    if (f->len) {
        char *b = f->buf;
        size_t n = f->len;
        f->buf = NULL;
        f->len = 0;
        submit(fd, b, n);
    }
    while (files[fd] == f && f->pending > 0) pthread_cond_wait(&changed, &lock); // (a closing one goes)
}

// a file opened: write-only ones get a buffer (linux flags: O_WRONLY 1, O_RDWR 2)
void wb_open(int fd, const char *path, int flags) {
    if (fd < 0 || fd >= WB_FDS || (flags & 3) != 1 || !wb_on()) return;
    wb_file *f = calloc(1, sizeof(*f));
    if (!f) return;
    snprintf(f->path, sizeof(f->path), "%s", path ? path : "");
    // added to the end of an old file: not moved (the copy checks the rest: all of it written here)
    f->moved = (flags & 0x400) || !big_candidate(f->path) ? -1 : 0; // O_APPEND
    f->pos = flags & 0x400 ? -1 : lseek(fd, 0, SEEK_CUR);
    f->out = -1;
    f->have_st = fstat(fd, &f->st) == 0;
    pthread_mutex_lock(&lock);
    if (fd > hi_fd) hi_fd = fd;
    if (files[fd]) { // a number reused before we heard of its close
        drain(fd);
        free(files[fd]->buf);
        free(files[fd]);
    }
    files[fd] = f;
    pthread_mutex_unlock(&lock);
}

// write: -2 when the file isn't one of ours (the caller writes it)
long wb_write(int fd, const void *p, size_t n) {
    if (fd < 0 || fd >= WB_FDS || !files[fd]) return -2;
    pthread_mutex_lock(&lock);
    wb_file *f = files[fd];
    if (!f) {
        pthread_mutex_unlock(&lock);
        return -2;
    }
    if (f->err) { // an earlier write failed: say so now
        errno = f->err;
        f->err = 0;
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (n >= WB_CAP / 2) { // a big one: whatever waits first, then a copy of it
        if (f->len) {
            char *b = f->buf;
            size_t l = f->len;
            f->buf = NULL;
            f->len = 0;
            submit(fd, b, l);
        }
        char *c = malloc(n);
        if (!c) {
            drain(fd);
            pthread_mutex_unlock(&lock);
            return -2; // written by the caller
        }
        memcpy(c, p, n);
        submit(fd, c, n);
        f->total += (long long)n;
        if (f->pos >= 0) f->pos += (long long)n;
        if (!f->moved && f->total >= BIG_AT) {
            f->moved = 1;
            submit_promote(fd);
        }
        pthread_mutex_unlock(&lock);
        return (long)n;
    }
    if (f->len + n > WB_CAP) {
        char *b = f->buf;
        size_t l = f->len;
        f->buf = NULL;
        f->len = 0;
        submit(fd, b, l);
        f = files[fd];
        if (!f) {
            pthread_mutex_unlock(&lock);
            return -2;
        }
    }
    if (!f->buf && !(f->buf = malloc(WB_CAP))) {
        drain(fd);
        pthread_mutex_unlock(&lock);
        return -2;
    }
    memcpy(f->buf + f->len, p, n);
    f->len += n;
    f->total += (long long)n;
    if (f->pos >= 0) f->pos += (long long)n;
    if (!f->moved && f->total >= BIG_AT) {
        f->moved = 1;
        submit_promote(fd);
    }
    pthread_mutex_unlock(&lock);
    return (long)n;
}

// everything written now (close, fsync, lseek, fstat, ftruncate, mmap, pwrite...)
void wb_flush(int fd) {
    if (fd < 0 || fd >= WB_FDS || !files[fd]) return;
    pthread_mutex_lock(&lock);
    drain(fd);
    pthread_mutex_unlock(&lock);
}

// written and forgotten (close, dup: its number shared from now)
void wb_forget(int fd) {
    if (fd < 0 || fd >= WB_FDS || !files[fd]) return;
    pthread_mutex_lock(&lock);
    drain(fd);
    wb_file *f = files[fd];
    files[fd] = NULL;
    pthread_mutex_unlock(&lock);
    if (f) {
        if (f->moved == 2) big_closed(f->moved_from); // its name in the data folder: emptied
        free(f->buf);
        free(f);
    }
}

// lseek: Unity asks before every write where it is (Shadow Fight 4's unpacking: 13 693 writes,
// 13 693 lseeks), each one waited for the disk. Where it is, or a move to there: answered here.
// 1: answered (*out), 0: everything written first, the caller asks the system
int wb_lseek(int fd, long off, int whence, long *out) {
    if (fd < 0 || fd >= WB_FDS || !files[fd]) return 0;
    pthread_mutex_lock(&lock);
    wb_file *f = files[fd];
    int mine = f && f->pos >= 0 && ((whence == SEEK_CUR && off == 0) || (whence == SEEK_SET && off == f->pos));
    if (mine) *out = (long)f->pos;
    else if (f) drain(fd);
    pthread_mutex_unlock(&lock);
    return mine;
}
// after the system's lseek: where the file is now
void wb_seeked(int fd, long at) {
    if (fd < 0 || fd >= WB_FDS || !files[fd]) return;
    pthread_mutex_lock(&lock);
    if (files[fd]) files[fd]->pos = at;
    pthread_mutex_unlock(&lock);
}

// close: a file of 1 MB and more is closed by its writer after its writes (Shadow Fight 4's
// unpacking waited 2.1 s of every 5 s in close); the game goes on at once. Small ones (saves)
// are written before close returns. 1: ours (closed later), 0: the caller closes it
int wb_close(int fd) {
    if (fd < 0 || fd >= WB_FDS || !files[fd]) return 0;
    pthread_mutex_lock(&lock);
    wb_file *f = files[fd];
    if (!f || f->closing || f->total < (1 << 20) || f->err) {
        pthread_mutex_unlock(&lock);
        wb_forget(fd);
        return 0;
    }
    wb_job *j = malloc(sizeof(*j));
    if (!j) {
        pthread_mutex_unlock(&lock);
        wb_forget(fd);
        return 0;
    }
    if (f->len) {
        char *b = f->buf;
        size_t l = f->len;
        f->buf = NULL;
        f->len = 0;
        submit(fd, b, l);
    }
    f->closing = 1;
    j->next = NULL;
    j->fd = fd;
    j->data = NULL;
    j->len = 0;
    j->promote = NULL;
    j->close = 1;
    int q = fd % writer_count();
    if (queues[q].tail) queues[q].tail->next = j;
    else queues[q].head = j;
    queues[q].tail = j;
    pthread_cond_signal(&queues[q].work);
    async_closes++;
    pthread_mutex_unlock(&lock);
    return 1;
}

// stat of a file still being written: its size with what waits (the game writes it from the
// start to the end: the end is where it is), without waiting for the disk (Shadow Fight 4's
// unpacking: 952 stats waited 553 ms in 5 s)
void wb_path_size(const char *path, long long *size) {
    if (!started || !path) return;
    pthread_mutex_lock(&lock);
    for (int fd = 0; fd <= hi_fd; fd++)
        if (files[fd] && files[fd]->pos > *size && !strcmp(files[fd]->path, path)) *size = files[fd]->pos;
    pthread_mutex_unlock(&lock);
}

// stat of a file being written: answered from what we know, without the disk - the console
// held a stat of a file until the write to it in progress was done (Shadow Fight 4's unpacking:
// stats of 460-560 ms on its main thread, behind 4 MB writes of 700-880 ms). 1: answered.
int wb_path_stat(const char *path, struct stat *out) {
    if (!started || !path) return 0;
    int found = 0;
    pthread_mutex_lock(&lock);
    for (int fd = 0; fd <= hi_fd && !found; fd++) {
        const wb_file *f = files[fd];
        if (!f || !f->have_st || strcmp(f->path, path) != 0) continue;
        *out = f->st;
        out->st_size = (off_t)(f->pos >= 0 ? f->pos : f->total);
        out->st_blocks = (out->st_size + 511) / 512;
        out->st_mtime = time(NULL);
        found = 1;
    }
    pthread_mutex_unlock(&lock);
    return found;
}

// a rename done: the files being written under the old name (or folder) go by the new one -
// nothing waits (Shadow Fight 4: a rename waited 2.2 s for a big file's data)
static int renamed(char *path, size_t cap, const char *a, size_t la, const char *b) {
    if (strncmp(path, a, la) || (path[la] && path[la] != '/')) return 0;
    char n[600];
    snprintf(n, sizeof(n), "%s%s", b, path + la);
    snprintf(path, cap, "%s", n);
    return 1;
}
void wb_renamed(const char *a, const char *b) {
    if (!started || !a || !b) return;
    size_t la = strlen(a);
    pthread_mutex_lock(&lock);
    for (int fd = 0; fd <= hi_fd; fd++) {
        wb_file *f = files[fd];
        if (!f) continue;
        renamed(f->path, sizeof(f->path), a, la, b);
        if (f->moved == 2) renamed(f->moved_from, sizeof(f->moved_from), a, la, b);
    }
    pthread_mutex_unlock(&lock);
}

// a path the game opens or sizes: what we hold for it (or under it, a folder) first
void wb_flush_path(const char *path) {
    if (!started || !path) return;
    size_t l = strlen(path);
    pthread_mutex_lock(&lock);
    for (int fd = 0; fd <= hi_fd; fd++)
        if (files[fd] && !strncmp(files[fd]->path, path, l) && (!files[fd]->path[l] || files[fd]->path[l] == '/'))
            drain(fd);
    pthread_mutex_unlock(&lock);
}

// for the speed report: MB written by the writers since last asked
double wb_written_mb(void) {
    pthread_mutex_lock(&lock);
    double mb = (double)stats_bytes / (1 << 20);
    stats_bytes = 0;
    stats_jobs = 0;
    pthread_mutex_unlock(&lock);
    return mb;
}
