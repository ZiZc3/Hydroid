// linux_abi.c â€” where Android on ARM64 (Linux numbers, bionic structs) and
// the PS5's FreeBSD libc disagree: struct stat / dirent layouts, open flags,
// sysconf ids, multibyte state size, long double (128-bit on ARM64), locale
// handles, the _chk variants, plus calls a console title can't make
// (fork, sockets for now) answered as failures.

#include <sys/uio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

#include "game_settings.h"
#include "unity_patch.h"
#include "arm_cpu.h"
#include "android_shim.h"

const char *droid_real_path(const char *path, char *buf, size_t n);

int arm_vsnprintf_entry(char *buf, size_t n, const char *fmt, const void *ap);

// ---- struct stat (asm-generic layout, 128 bytes) -------------------------------

typedef struct {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1;
    int64_t st_size;
    int32_t st_blksize, pad2;
    int64_t st_blocks;
    int64_t st_atime_s, st_atime_ns, st_mtime_s, st_mtime_ns, st_ctime_s, st_ctime_ns;
    uint32_t unused[2];
} linux_stat;
_Static_assert(sizeof(linux_stat) == 128, "arm64 struct stat is 128 bytes");

static void to_linux_stat(const struct stat *s, linux_stat *l) {
    memset(l, 0, sizeof(*l));
    l->st_dev = s->st_dev;
    l->st_ino = s->st_ino;
    l->st_mode = s->st_mode;
    l->st_nlink = (uint32_t)s->st_nlink;
    l->st_uid = s->st_uid;
    l->st_gid = s->st_gid;
    l->st_rdev = s->st_rdev;
    l->st_size = s->st_size;
    l->st_blksize = s->st_blksize;
    l->st_blocks = s->st_blocks;
    l->st_atime_s = s->st_atim.tv_sec;
    l->st_atime_ns = s->st_atim.tv_nsec;
    l->st_mtime_s = s->st_mtim.tv_sec;
    l->st_mtime_ns = s->st_mtim.tv_nsec;
    l->st_ctime_s = s->st_ctim.tv_sec;
    l->st_ctime_ns = s->st_ctim.tv_nsec;
}

const char *droid_abs(const char *path, char *buf, size_t n); // below: a relative path, from our cwd
// write_behind.c: a write-only file's writes gathered and written by our threads
void wb_open(int fd, const char *path, int flags);
long wb_write(int fd, const void *p, size_t n);
void wb_flush(int fd);
void wb_forget(int fd);
void wb_flush_path(const char *path);
void wb_path_size(const char *path, long long *size);
int wb_path_stat(const char *path, struct stat *out);
void wb_renamed(const char *a, const char *b);
void wb_rename_begin(void), wb_rename_end(void); // a rename and a move to the fast disk: in turn
int wb_lseek(int fd, long off, int whence, long *out);
void wb_seeked(int fd, long at);
// big_files.c: a game's big files on the disk that writes them fast
const char *big_map(const char *path, char *buf, size_t n);
void big_unlinked(const char *path);
void big_rmdir(const char *path);
void big_renamed(const char *a, const char *b);

// /proc as stat / lstat / access see it (the PS5 has none): /proc, /proc/self and the files
// droid_proc_open makes. .NET looks before it opens (Shadow Fight 3's C# read of
// /proc/self/maps: "Could not find a part of the path", the game's "restart" error). 1: one of
// ours (out filled when given), 0: not /proc, -1: /proc but not there (ENOENT).
// DROID_PC_PROC=1 (PC tests): the PC's own.
int droid_proc_open(const char *path); // below
static int proc_stat(const char *path, linux_stat *out) {
    if (strncmp(path, "/proc", 5) != 0 || (path[5] && path[5] != '/') || getenv("DROID_PC_PROC")) return 0;
    int dir = !strcmp(path, "/proc") || !strcmp(path, "/proc/") || !strcmp(path, "/proc/self") ||
              !strcmp(path, "/proc/self/");
    if (!dir) {
        int fd = droid_proc_open(path);
        if (fd < 0) {
            errno = ENOENT;
            return -1;
        }
        close(fd);
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        out->st_mode = dir ? 0040555 : 0100444; // as Linux: a directory, or a file of size 0
        out->st_nlink = 1;
        out->st_blksize = 4096;
    }
    return 1;
}

// a file call that took long (100 ms and more): logged with its path, the first 80, and
// counted for the 5 s report (Shadow Fight 4's unpacking on the console: stats of ~950 ms)
static double fs_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static volatile int fs_slow_n;
static volatile long long fs_slow_ms;
static void fs_slow(const char *what, const char *path, double t0) {
    double ms = fs_now() - t0;
    if (ms < 100) return;
    __atomic_add_fetch(&fs_slow_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&fs_slow_ms, (long long)ms, __ATOMIC_RELAXED);
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 80) droid_log("files: %s(%s) took %.0f ms", what, path, ms);
}
// CACHE_FS_SERIAL_GAMES: the file calls in Unity's download cache (/UnityCache/) one at a time.
// Vector's story download (113 MB, 2026-11-01): ~14 job threads renaming cache folders from
// Temp to Shared while others made, checked and removed files there; at ~13% all of them sat
// in rename / stat / unlink / mkdir / open / access for good - calls that take none of our
// locks, so the console's download-data disk itself stopped. A watchdog logs a call still
// running after 3 s (which one, on what), to tell if one call alone hangs.
#define CACHE_FS_SERIAL_GAMES "com.nekki.vector"
static pthread_mutex_t cache_fs_m = PTHREAD_MUTEX_INITIALIZER;
static volatile double cache_fs_since;
static const char *volatile cache_fs_what;
static char cache_fs_path[300];
static void *cache_fs_watch(void *arg) {
    (void)arg;
    double told_for = 0;
    for (;;) {
        struct timespec w = { 1, 0 };
        nanosleep(&w, NULL);
        const double since = cache_fs_since;
        if (since && since != told_for && fs_now() - since > 3000) {
            told_for = since;
            droid_log("files: %s(%s) still running after %.0f s (the cache's calls wait for it)",
                      cache_fs_what ? cache_fs_what : "?", cache_fs_path, (fs_now() - since) / 1000);
        }
    }
    return NULL;
}
static int cache_fs_begin(const char *what, const char *path) {
    static int on = -1;
    if (on < 0) {
        on = droid_game_in(CACHE_FS_SERIAL_GAMES) != 0;
        if (on) {
            pthread_t t;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setstacksize(&a, 256 << 10);
            pthread_create(&t, &a, cache_fs_watch, NULL);
            pthread_attr_destroy(&a);
            droid_log("files: the cache's file calls one at a time (CACHE_FS_SERIAL_GAMES)");
        }
    }
    if (!on || !path || !strstr(path, "/UnityCache/")) return 0;
    pthread_mutex_lock(&cache_fs_m);
    cache_fs_what = what;
    snprintf(cache_fs_path, sizeof(cache_fs_path), "%s", path);
    cache_fs_since = fs_now();
    return 1;
}
static void cache_fs_end(int token) {
    if (!token) return;
    cache_fs_since = 0;
    pthread_mutex_unlock(&cache_fs_m);
}
// the speed report: slow file calls since last asked
int fs_slow_report(long long *ms) {
    *ms = __atomic_exchange_n(&fs_slow_ms, 0, __ATOMIC_RELAXED);
    return __atomic_exchange_n(&fs_slow_n, 0, __ATOMIC_RELAXED);
}

static int l_stat(const char *path, linux_stat *out) {
    int p = proc_stat(path, out);
    if (p) return p > 0 ? 0 : -1;
    char ab[600];
    path = droid_abs(path, ab, sizeof(ab));
    struct stat s;
    if (wb_path_stat(path, &s)) { // being written by us: its size known, the disk not asked
        to_linux_stat(&s, out);
        return 0;
    }
    double t0 = fs_now();
    int ct = cache_fs_begin("stat", path);
    int r = stat(path, &s);
    cache_fs_end(ct);
    fs_slow("stat", path, t0);
    if (r == 0) { // its size: with what waits to be written (write_behind.c)
        long long size = (long long)s.st_size;
        wb_path_size(path, &size);
        s.st_size = (off_t)size;
        to_linux_stat(&s, out);
    }
    else if (errno != ENOENT && errno != ENOTDIR) {
        static int told;
        if (told++ < 10) droid_log("stat(%s) failed: errno %d", path, errno);
    }
    return r;
}

// lstat: the console refuses it for a title's files (EPERM: .NET's FileInfo failed on
// every file, "Access denied"); a title has no symbolic links, so stat's answer is the same.
static int l_lstat(const char *path, linux_stat *out) {
    int p = proc_stat(path, out);
    if (p) return p > 0 ? 0 : -1;
    char ab[600];
    path = droid_abs(path, ab, sizeof(ab));
    struct stat s;
    if (wb_path_stat(path, &s)) {
        to_linux_stat(&s, out);
        return 0;
    }
    double t0 = fs_now();
    int ct = cache_fs_begin("lstat", path);
    int r = lstat(path, &s);
    cache_fs_end(ct);
    fs_slow("lstat", path, t0);
    if (r != 0 && errno != ENOENT && errno != ENOTDIR) {
        static int told;
        if (told++ < 5) droid_log("lstat(%s): refused (errno %d), stat instead", path, errno);
        ct = cache_fs_begin("stat", path);
        r = stat(path, &s);
        cache_fs_end(ct);
    }
    if (r == 0) {
        long long size = (long long)s.st_size;
        wb_path_size(path, &size);
        s.st_size = (off_t)size;
        to_linux_stat(&s, out);
    }
    return r;
}

static int l_fstat(int fd, linux_stat *out) {
    wb_flush(fd); // its size: with what waits to be written
    struct stat s;
    int r = fstat(fd, &s);
    if (r == 0) to_linux_stat(&s, out);
    return r;
}

// ---- open flags (Linux arm64 octal values) ----------------------------------

static int host_open_flags(int f) {
    static const struct { int linux_bit, host; } map[] = {
        { 0100, O_CREAT }, { 0200, O_EXCL }, { 0400, O_NOCTTY }, { 01000, O_TRUNC },
        { 02000, O_APPEND }, { 04000, O_NONBLOCK }, { 040000, O_DIRECTORY },
        { 0100000, O_NOFOLLOW }, { 02000000, O_CLOEXEC }, { 04010000, O_SYNC },
    };
    int h = f & 3; // O_RDONLY / O_WRONLY / O_RDWR
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if ((f & map[i].linux_bit) == map[i].linux_bit) h |= map[i].host;
    return h;
}

// ---- where our memory goes: out of RADV's GPU window ------------------------------------
// RADV puts its 32-bit buffers (command ring, descriptors, shader code) where the kernel
// places a mapping given no address: the window 0x2_0000_0000-0x3_0000_0000. Ours given
// no address went there too (JIT caches, the game's libraries, Unity's reservations), the
// window filled, and RADV found no GPU. So ours get addresses from 64 GiB up, as XPSemu
// learned (its mmap hint 0x1000000000), below RADV's device region at 0x40_0000_0000.
static uint64_t va_next = 0x1000000000ull;
static pthread_mutex_t va_lock = PTHREAD_MUTEX_INITIALIZER;

void *droid_va_hint(size_t bytes) {
    pthread_mutex_lock(&va_lock);
    uint64_t at = (va_next + 0x1FFFFF) & ~0x1FFFFFull; // 2 MiB aligned
    va_next = at + ((bytes + 0x1FFFFF) & ~(size_t)0x1FFFFF) + 0x200000; // a gap between
    pthread_mutex_unlock(&va_lock);
    return (void *)at;
}

// fd -> the path and flags it was opened with: dup() is refused on the PS5 (-1),
// so a copy is the same file opened again at the same position.
#define FD_PATHS 4096
static struct { char *path; int flags; } fd_paths[FD_PATHS];
static pthread_mutex_t fd_paths_lock = PTHREAD_MUTEX_INITIALIZER;

static void note_fd(int fd, const char *path, int host_flags) {
    if (fd < 0 || fd >= FD_PATHS) return;
    pthread_mutex_lock(&fd_paths_lock);
    free(fd_paths[fd].path);
    fd_paths[fd].path = strdup(path);
    fd_paths[fd].flags = host_flags & ~(O_CREAT | O_EXCL | O_TRUNC);
    pthread_mutex_unlock(&fd_paths_lock);
}

static int l_dup(int fd) {
    wb_forget(fd); // two numbers for one file from now: no buffer
    static int no_dup = -1; // DROID_NO_DUP=1: the PC runner acts like the PS5
    if (no_dup < 0) no_dup = getenv("DROID_NO_DUP") != NULL;
    int d = no_dup ? -1 : dup(fd);
    if (d < 0 && !no_dup) d = fcntl(fd, F_DUPFD, 0);
    if (d >= 0 || fd < 0 || fd >= FD_PATHS) return d;
    pthread_mutex_lock(&fd_paths_lock);
    char *path = fd_paths[fd].path ? strdup(fd_paths[fd].path) : NULL;
    int flags = fd_paths[fd].flags;
    pthread_mutex_unlock(&fd_paths_lock);
    if (!path) return -1;
    d = open(path, flags);
    if (d >= 0) {
        lseek(d, lseek(fd, 0, SEEK_CUR), SEEK_SET);
        note_fd(d, path, flags);
    }
    static int logged;
    if (logged++ < 8) droid_log("dup(%d): refused, opened %s again -> %d", fd, path, d);
    free(path);
    return d;
}

// ---- /proc: the PS5 has none; games read a few files of it -------------------------
// /proc/self/maps (Unity's garbage collector finds the game's data through it: without
// it the game's objects are freed and only the camera's background is drawn), cpuinfo,
// meminfo. Made on the spot from what we know, the same on the PC and the PS5.

#include "arm_modules.h"
#include <elf.h>

int arm_jit_range(int i, uint64_t *from, uint64_t *to); // arm_cpu.cpp

// The lines in address order, as the kernel gives them. Unity 2019's unwinder (libunwindstack's
// LocalUpdatableMaps::Reparse) relies on it when it reads the list again: in load order (and
// new JIT ranges at the end) it used an entry it had already moved away - Vector 2 crashed
// (BAD read at 0x1, libunity+0x7a4f50) loading its first scene. Vector 2 only so far.
#define MAPS_SORTED_GAMES "com.nekki.vector2"
static int maps_line_cmp(const void *a, const void *b) {
    const unsigned long long x = strtoull(*(char *const *)a, NULL, 16), y = strtoull(*(char *const *)b, NULL, 16);
    return x < y ? -1 : x > y;
}
static size_t maps_sort(char *out, size_t n) {
    char *copy = malloc(n + 1);
    char **line = malloc(sizeof(char *) * (n / 20 + 2));
    if (!copy || !line) { free(copy); free(line); return n; }
    memcpy(copy, out, n);
    copy[n] = 0;
    int count = 0;
    for (char *p = copy; *p;) {
        line[count++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    qsort(line, (size_t)count, sizeof(char *), maps_line_cmp);
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        const size_t len = strlen(line[i]);
        memcpy(out + at, line[i], len);
        at += len;
        out[at++] = '\n';
    }
    free(copy);
    free(line);
    return at;
}

static size_t proc_maps(char *out, size_t cap) {
    size_t at = 0;
    for (int i = 0; i < arm_module_count(); i++) {
        so_module *m = arm_module(i);
        const char *path = arm_module_path(i);
        if (!m || !m->phdr) continue;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)m->phdr;
        for (int k = 0; k < m->phnum && at + 400 < cap; k++) {
            if (ph[k].p_type != PT_LOAD) continue;
            uint64_t from = (m->bias + ph[k].p_vaddr) & ~0xFFFull;
            uint64_t to = (m->bias + ph[k].p_vaddr + ph[k].p_memsz + 0xFFF) & ~0xFFFull;
            at += (size_t)snprintf(out + at, cap - at, "%llx-%llx %c%c%cp %08llx 00:00 %d %s\n",
                                   (unsigned long long)from, (unsigned long long)to,
                                   ph[k].p_flags & PF_R ? 'r' : '-', ph[k].p_flags & PF_W ? 'w' : '-',
                                   ph[k].p_flags & PF_X ? 'x' : '-',
                                   (unsigned long long)(ph[k].p_offset & ~0xFFFull), 1000 + i, path ? path : "");
        }
    }
    // our code (thunks, callbacks, the stub every ARM call returns through): a stack walk
    // reaches it at the bottom of every thread. Not in the list, Unity's unwinder took it for a
    // library loaded since and read the whole list again, then opened every library again -
    // 10 times per log line's stack trace (Among Us: 400 lists and 700 opens a second)
    uint64_t from, to;
    for (int r = 0; arm_jit_range(r, &from, &to) && at + 100 < cap; r++)
        at += (size_t)snprintf(out + at, cap - at, "%llx-%llx r-xp 00000000 00:00 0 [anon:jit]\n",
                               (unsigned long long)from, (unsigned long long)to);
    if (droid_game_in(MAPS_SORTED_GAMES)) at = maps_sort(out, at);
    return at;
}

static size_t proc_cpuinfo(char *out, size_t cap) {
    size_t at = 0;
    for (int c = 0; c < 8 && at + 400 < cap; c++)
        at += (size_t)snprintf(out + at, cap - at,
                               "processor\t: %d\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid\n"
                               "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd0b\n"
                               "CPU revision\t: 0\n\n", c);
    at += (size_t)snprintf(out + at, cap - at, "Hardware\t: PS5\n");
    return at;
}

static size_t proc_meminfo(char *out, size_t cap) {
    return (size_t)snprintf(out, cap,
                            "MemTotal:        8388608 kB\nMemFree:         4194304 kB\nMemAvailable:    6291456 kB\n"
                            "Buffers:               0 kB\nCached:          1048576 kB\nSwapCached:            0 kB\n"
                            "SwapTotal:             0 kB\nSwapFree:              0 kB\n");
}

// self/status and self/statm: games read their memory use (Unity's memory checks)
static size_t proc_status(char *out, size_t cap) {
    return (size_t)snprintf(out, cap,
                            "Name:\tUnityMain\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t1\n"
                            "TracerPid:\t0\nUid:\t10100\t10100\t10100\t10100\nGid:\t10100\t10100\t10100\t10100\n"
                            "VmPeak:\t 4194304 kB\nVmSize:\t 4194304 kB\nVmHWM:\t  786432 kB\nVmRSS:\t  786432 kB\n"
                            "RssAnon:\t  524288 kB\nRssFile:\t  262144 kB\nVmSwap:\t       0 kB\nThreads:\t40\n",
                            getpid(), getpid());
}
// self/cmdline: the package, then the engine's options. Unity reads its command line here:
// -force-vulkan when the game is to draw with Vulkan (droid_vulkan_wanted, app_main.c)
extern const char *java_package; // jni_impl.c
extern int droid_vulkan_wanted;
static size_t proc_cmdline(char *out, size_t cap) {
    int n = snprintf(out, cap, "%s", java_package) + 1;
    if (droid_vulkan_wanted && (size_t)n + 16 < cap) n += snprintf(out + n, cap - (size_t)n, "-force-vulkan") + 1;
    return (size_t)n;
}

static size_t proc_statm(char *out, size_t cap) {
    return (size_t)snprintf(out, cap, "1048576 196608 65536 2048 0 524288 0\n"); // pages: 4 GB, 768 MB resident
}

// /sys/devices/system/cpu: Unity reads every core's capacity and top speed each frame
// (no answer: "0 big, 0 little cores", and ~1 ms per failed open). 8 equal big cores.
static size_t sys_text;
static char sys_value[64];
static size_t proc_sys(char *out, size_t cap) { return (size_t)snprintf(out, cap, "%s\n", sys_value); }

static int sys_cpu(const char *f) { // f: after "/sys/devices/system/cpu/"
    if (!strcmp(f, "possible") || !strcmp(f, "present") || !strcmp(f, "online")) {
        snprintf(sys_value, sizeof(sys_value), "0-7");
        return 1;
    }
    int cpu = -1, used = 0;
    if (sscanf(f, "cpu%d/%n", &cpu, &used) != 1 || cpu < 0 || cpu > 7 || !used) return 0;
    f += used;
    if (!strcmp(f, "cpu_capacity")) snprintf(sys_value, sizeof(sys_value), "1024");
    else if (!strcmp(f, "cpufreq/cpuinfo_max_freq") || !strcmp(f, "cpufreq/scaling_max_freq") ||
             !strcmp(f, "cpufreq/scaling_cur_freq"))
        snprintf(sys_value, sizeof(sys_value), "3500000");
    else if (!strcmp(f, "cpufreq/cpuinfo_min_freq") || !strcmp(f, "cpufreq/scaling_min_freq"))
        snprintf(sys_value, sizeof(sys_value), "400000");
    else if (!strcmp(f, "online")) snprintf(sys_value, sizeof(sys_value), "1");
    else return 0;
    return 1;
}

// a pipe whose writer is a thread: made-up text of any size as an fd, no disk (a temp file on
// the PS5 took 35 ms: Unity reads /proc/self/maps twice per log line's stack trace)
typedef struct { int fd; size_t n; char text[]; } pipe_job;
static void *pipe_writer(void *arg) {
    pipe_job *j = arg;
    size_t put = 0;
    while (put < j->n) {
        ssize_t w = write(j->fd, j->text + put, j->n - put);
        if (w <= 0) break; // the reader closed early
        put += (size_t)w;
    }
    close(j->fd);
    free(j);
    return NULL;
}
static int piped_fd(const char *text, size_t n) {
    int p[2];
    if (pipe(p) != 0) return -1;
    pipe_job *j = malloc(sizeof(*j) + n);
    if (!j) { close(p[0]); close(p[1]); return -1; }
    j->fd = p[1];
    j->n = n;
    memcpy(j->text, text, n);
    pthread_t t;
    if (pthread_create(&t, NULL, pipe_writer, j) != 0) {
        free(j);
        close(p[0]);
        close(p[1]);
        return -1;
    }
    pthread_detach(t);
    return p[0];
}

// Our made-up text as an fd: a pipe (no disk) when it fits, else a temp file.
static int text_fd(const char *text, size_t n, int may_pipe) {
    int p[2];
    if (may_pipe && n <= 8192 && pipe(p) == 0) {
        ssize_t w = write(p[1], text, n);
        close(p[1]);
        if (w == (ssize_t)n) return p[0];
        close(p[0]);
    }
    static int seq;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), APP0 "/data/proc-%d-%d", getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (write(fd, text, n) != (ssize_t)n) { close(fd); fd = -1; }
        else lseek(fd, 0, SEEK_SET);
        unlink(tmp);
    }
    return fd;
}

void arm_call_from(char *out, size_t n); // arm_cpu.cpp
void net_nonblock_set(int fd, int on);   // net_shim.c: non-blocking as the game set it
int net_nonblock(int fd);

// path is a /proc or /sys file: an fd reading what we made for it, or -1 (ENOENT).
// Neither: -2. DROID_PC_PROC=1 (PC tests): the PC's own /proc and /sys.
int droid_proc_open(const char *path) {
    int is_sys = strncmp(path, "/sys/", 5) == 0;
    if (strncmp(path, "/proc/", 6) != 0 && !is_sys) return -2;
    if (getenv("DROID_PC_PROC")) return -2;
    if (is_sys) {
        static pthread_mutex_t sys_lock = PTHREAD_MUTEX_INITIALIZER;
        static int sys_logged;
        pthread_mutex_lock(&sys_lock);
        int fd = -1;
        if (!strncmp(path, "/sys/devices/system/cpu/", 24) && sys_cpu(path + 24)) {
            char t[80];
            sys_text = proc_sys(t, sizeof(t));
            fd = text_fd(t, sys_text, 1);
        }
        pthread_mutex_unlock(&sys_lock);
        if (sys_logged++ < 12) droid_log("sys: %s -> %s", path, fd >= 0 ? "made" : "ENOENT");
        if (fd < 0) errno = ENOENT;
        return fd;
    }
    const char *f = path + 6;
    if (strncmp(f, "self/", 5) == 0) f += 5;
    else {
        char *end; // /proc/<our pid>/...
        long pid = strtol(f, &end, 10);
        if (end != f && *end == '/' && pid == getpid()) f = end + 1;
        else if (end != f && *end == '/') { errno = ENOENT; return -1; }
    }
    size_t (*make)(char *, size_t) = NULL;
    if (strcmp(f, "maps") == 0 || strcmp(path + 6, "self/maps") == 0) make = proc_maps;
    else if (strcmp(path, "/proc/cpuinfo") == 0) make = proc_cpuinfo;
    else if (strcmp(path, "/proc/meminfo") == 0) make = proc_meminfo;
    else if (strcmp(f, "status") == 0) make = proc_status;
    else if (strcmp(f, "statm") == 0) make = proc_statm;
    else if (strcmp(f, "cmdline") == 0) make = proc_cmdline;
    static int logged;
    if (!make) {
        if (logged++ < 30) droid_log("proc: %s: not made, ENOENT", path);
        errno = ENOENT;
        return -1;
    }
    size_t cap = 256 * 1024;
    char *text = malloc(cap);
    if (!text) { errno = ENOMEM; return -1; }
    size_t n = make(text, cap);
    if (make == proc_maps && getenv("DROID_MAPS_DUMP")) { // PC tests: the maps as the game saw them
        char dp[400];
        FILE *df = fopen(game_path(dp, sizeof(dp), "maps.txt"), "w");
        if (df) { fwrite(text, 1, n, df); fclose(df); }
    }
    // a real file for the GC (it seeks / sizes it); Unity's unwinder (libunwindstack, from
    // libunity.so) only reads it through: a pipe, no disk
    char from[64];
    arm_call_from(from, sizeof(from));
    int fd = make == proc_maps && !strncmp(from, "libunity.so", 11) ? piped_fd(text, n) : -1;
    if (fd < 0) fd = text_fd(text, n, 0);
    if (logged++ < 30) droid_log("proc: %s -> %zu bytes made, fd %d", path, n, fd);
    free(text);
    return fd;
}

// what a game opens, counted: the most opened paths every 10 s, with their time (a file
// opened every frame is slow on the PS5: Among Us at 7 fps, each open 35 ms)
static void open_counted(const char *path, double ms) {
    static struct { char path[96]; int n; double ms; char from[64]; } top[24];
    static int used;
    static double since;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double now = t.tv_sec + t.tv_nsec / 1e9;
    pthread_mutex_lock(&lock);
    if (!since) since = now;
    int i = 0;
    while (i < used && strncmp(top[i].path, path, sizeof(top[0].path) - 1)) i++;
    if (i == used && used < 24) {
        snprintf(top[used].path, sizeof(top[0].path), "%s", path);
        arm_call_from(top[used].from, sizeof(top[0].from)); // who opens it (the first time in 10 s)
        used++;
    }
    if (i < used) { top[i].n++; top[i].ms += ms; }
    if (now - since >= 10) {
        for (int k = 0; k < used; k++)
            if (top[k].n >= 20) {
                // on the console (measured there): a file we make (/proc) ~35 ms, an open ~1 ms
                double ps5 = top[k].n * (!strncmp(top[k].path, "/proc/", 6) ? 35.0 : 1.0);
                droid_log("files: %s opened %d times in 10 s, %.0f ms (from %s)%s", top[k].path, top[k].n, top[k].ms,
                          top[k].from, ps5 > 1000 ? " - SLOW ON THE PS5 (about that many seconds' worth per 10 s)" : "");
            }
        used = 0;
        since = now;
    }
    pthread_mutex_unlock(&lock);
}

static int l_open(const char *path, int flags, int mode) {
    char ab[600];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    path = droid_abs(path, ab, sizeof(ab));
    // the PS5 has no /proc: the PC runner hides its own so both behave the same
    int fd = droid_proc_open(path);
    char real[512];
    if (fd == -2) {
        double t0 = fs_now();
        wb_flush_path(path); // what we still hold for this file written first (opened again)
        fs_slow("open: its pending writes", path, t0);
        t0 = fs_now();
        int ct = cache_fs_begin("open", path);
        fd = open(droid_real_path(path, real, sizeof(real)), host_open_flags(flags), mode);
        cache_fs_end(ct);
        fs_slow("open", path, t0);
        if (fd >= 0) wb_open(fd, path, flags);
    }
    static int failed;
    if (fd < 0 && failed++ < 40) droid_log("open(%s, %#o) failed: errno %d", path, flags, errno);
    if (fd >= 0) note_fd(fd, path, host_open_flags(flags));
    if (fd >= 0) net_nonblock_set(fd, (flags & 04000) != 0); // a reused number: a file now
    clock_gettime(CLOCK_MONOTONIC, &t1);
    open_counted(path, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    return fd;
}

static int l_open_2(const char *path, int flags) { return l_open(path, flags, 0); }

// ---- dirent (d_ino, d_off, d_reclen, d_type, d_name[256]) -----------------------

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} linux_dirent;

static _Thread_local linux_dirent dirent_out;

static linux_dirent *l_readdir(DIR *d) {
    struct dirent *e = readdir(d);
    if (!e) return NULL;
    memset(&dirent_out, 0, sizeof(dirent_out));
    dirent_out.d_ino = e->d_fileno;
    dirent_out.d_reclen = sizeof(dirent_out);
    dirent_out.d_type = e->d_type;
    strncpy(dirent_out.d_name, e->d_name, sizeof(dirent_out.d_name) - 1);
    return &dirent_out;
}

// ---- sysconf (bionic ids) ---------------------------------------------------------

// The page size a game sees: 4 KB, as most phones. DROID_PAGE_16K=1 (tests): 16 KB (Android 15
// phones). Not for Unreal: Wuthering Waves' packer runs with 4 KB, and its V8 checks that every
// mmap is page-aligned (16 KB: "Check failed: address % CommitPageSize()" on the PC's 4 KB mmaps)
static int droid_page_size(void) {
    const char *e = getenv("DROID_PAGE_16K");
    return e && *e && *e != '0' ? 16384 : 4096;
}

static long l_sysconf(int name) {
    switch (name) {
    case 0x06: return 100;                            // _SC_CLK_TCK
    case 0x27: case 0x28: return droid_page_size();   // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 0x60: case 0x61: return 8;                   // _SC_NPROCESSORS_CONF / _ONLN
    case 0x62: return (12LL << 30) / 4096;            // _SC_PHYS_PAGES
    case 0x63: return (6LL << 30) / 4096;             // _SC_AVPHYS_PAGES
    }
    static unsigned char warned[256];
    if (name >= 0 && name < 256 && !warned[name]++) droid_log("sysconf(%#x) not known", name);
    errno = EINVAL;
    return -1;
}

// ---- multibyte state: bionic's mbstate_t is 8 bytes, FreeBSD's 128 ------------------

static size_t l_mbrlen(const char *s, size_t n, void *ps) {
    (void)ps;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    return mbrlen(s, n, &st);
}

static size_t l_mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps) {
    (void)ps;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    return mbrtowc(pwc, s, n, &st);
}

// The PS5 libc has no mbsnrtowcs / wcsnrtombs: built on mbrtowc / wcrtomb.
static size_t l_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
    (void)ps;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const char *s = *src;
    size_t count = 0;
    while (nms > 0 && (!dst || count < len)) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, s, nms, &st);
        if (r == (size_t)-1) {
            if (dst) *src = s;
            return (size_t)-1;
        }
        if (r == (size_t)-2) break; // incomplete at the end of the nms bytes
        if (r == 0) {
            if (dst) {
                dst[count] = 0;
                *src = NULL;
            }
            return count;
        }
        if (dst) dst[count] = wc;
        count++;
        s += r;
        nms -= r;
    }
    if (dst) *src = s;
    return count;
}

static size_t l_mbsrtowcs(wchar_t *dst, const char **src, size_t len, void *ps) {
    (void)ps;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    return mbsrtowcs(dst, src, len, &st);
}

static size_t l_wcrtomb(char *s, wchar_t wc, void *ps) {
    (void)ps;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    return wcrtomb(s, wc, &st);
}

static size_t l_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
    (void)ps;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const wchar_t *s = *src;
    size_t count = 0;
    char buf[16];
    while (nwc > 0) {
        size_t r = wcrtomb(buf, *s, &st);
        if (r == (size_t)-1) {
            if (dst) *src = s;
            return (size_t)-1;
        }
        if (dst && count + r > len) break;
        if (dst) memcpy(dst + count, buf, r);
        if (*s == 0) {
            if (dst) *src = NULL;
            return count;
        }
        count += r;
        s++;
        nwc--;
    }
    if (dst) *src = s;
    return count;
}

static size_t l_ctype_get_mb_cur_max(void) { return 1; }

// ---- long double: IEEE binary128 in v0 on ARM64 ----------------------------------

typedef float v4f __attribute__((vector_size(16)));

static v4f quad_from_double(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    uint64_t sign = b >> 63, exp = (b >> 52) & 0x7ff, man = b & ((1ull << 52) - 1);
    uint64_t hi, lo;
    if (exp == 0 && man == 0) {
        hi = 0;
        lo = 0;
    } else if (exp == 0x7ff) {
        hi = 0x7fffull << 48 | man >> 4;
        lo = man << 60;
    } else {
        if (exp == 0) { // subnormal: normalize
            int shift = __builtin_clzll(man) - 11;
            man = (man << shift) & ((1ull << 52) - 1);
            exp = 1 - shift;
        }
        hi = (exp - 1023 + 16383) << 48 | man >> 4;
        lo = man << 60;
    }
    hi |= sign << 63;
    v4f q;
    memcpy(&q, &lo, 8);
    memcpy((char *)&q + 8, &hi, 8);
    return q;
}

static v4f l_strtold(const char *s, char **end) { return quad_from_double(strtod(s, end)); }
static v4f l_strtold_l(const char *s, char **end, void *loc) {
    (void)loc;
    return quad_from_double(strtod(s, end));
}
static v4f l_wcstold(const wchar_t *s, wchar_t **end) { return quad_from_double(wcstod(s, end)); }

// ---- locale handles: everything is the C locale -------------------------------------

static int dummy_locale;

static void *l_newlocale(int mask, const char *name, void *base) {
    (void)mask; (void)name; (void)base;
    return &dummy_locale;
}
static void *l_uselocale(void *loc) {
    (void)loc;
    return (void *)-1L; // LC_GLOBAL_LOCALE
}
static void l_freelocale(void *loc) { (void)loc; }
static char *l_setlocale(int cat, const char *name) {
    (void)cat; (void)name;
    return "C";
}
static long long l_strtoll_l(const char *s, char **e, int base, void *loc) {
    (void)loc;
    return strtoll(s, e, base);
}
static unsigned long long l_strtoull_l(const char *s, char **e, int base, void *loc) {
    (void)loc;
    return strtoull(s, e, base);
}

// ---- _chk (fortify) ----------------------------------------------------------------

static void *l_memmove_chk(void *d, const void *s, size_t n, size_t dlen) {
    (void)dlen;
    return memmove(d, s, n);
}
static ssize_t l_read_chk(int fd, void *buf, size_t n, size_t buflen) {
    (void)buflen;
    return read(fd, buf, n);
}
static size_t l_strlen_chk(const char *s, size_t slen) {
    (void)slen;
    return strlen(s);
}
static int l_vsnprintf_chk(char *buf, size_t maxlen, int flags, size_t slen, const char *fmt,
                           const void *ap) {
    (void)flags; (void)slen;
    return arm_vsnprintf_entry(buf, maxlen, fmt, ap);
}
static void l_fd_set_chk(int fd, fd_set *set, size_t size) {
    (void)size;
    FD_SET(fd, set);
}

// ---- setjmp/longjmp: the host's would save the wrong (x86) state ---------------------

static int l_setjmp(void *buf) {
    (void)buf;
    return 0;
}
static void l_longjmp(void *buf, int v) {
    (void)buf;
    droid_log("longjmp(%d): not supported for ARM64 code, stopping", v);
    abort();
}

// ---- signals, processes, sockets: answered as failures -----------------------------

// bionic's (arm64) struct sigaction: flags, handler, mask (64 bits), restorer. The handlers
// run on their thread when a signal is sent to it (arm_cpu.cpp): the GC pauses threads so.
static int l_sigaction(int sig, const void *act, void *old) {
    const unsigned char *a = act;
    uint64_t handler = ~0ull, mask = 0, oh = 0, om = 0;
    int flags = 0, of = 0;
    if (a) {
        memcpy(&flags, a, 4);
        memcpy(&handler, a + 8, 8);
        memcpy(&mask, a + 16, 8);
    }
    if (arm_sig_action(sig, handler, mask, flags, &oh, &om, &of) != 0) return -1;
    if (old) {
        unsigned char *o = old;
        memset(o, 0, 32);
        memcpy(o, &of, 4);
        memcpy(o + 8, &oh, 8);
        memcpy(o + 16, &om, 8);
    }
    return 0;
}
static int l_fail(void) {
    errno = ENOSYS;
    return -1;
}
// sendfile: a copy by read and write (il2cpp's File.Copy: with "not there" it tried again
// forever, 167 million times, a core busy: Among Us stuttered)
static long l_sendfile(int out, int in, int64_t *offset, size_t count) {
    wb_flush(out);
    wb_flush(in);
    static char buf[1 << 16];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    long done = 0;
    int64_t at = offset ? *offset : 0;
    while ((size_t)done < count) {
        size_t want = count - (size_t)done < sizeof(buf) ? count - (size_t)done : sizeof(buf);
        ssize_t n = offset ? pread(in, buf, want, (off_t)at) : read(in, buf, want);
        if (n < 0) {
            if (!done) done = -1;
            break;
        }
        if (n == 0) break;
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(out, buf + w, (size_t)(n - w));
            if (k <= 0) break;
            w += k;
        }
        done += w;
        at += w;
        if (w < n) break;
    }
    if (offset && done > 0) *offset = at;
    pthread_mutex_unlock(&lock);
    return done;
}

static void *l_null(void) {
    errno = ENOSYS;
    return NULL;
}
static int l_zero(void) { return 0; }
// thread names: the render threads found by them (thread_place.c)
void thread_place_named(pthread_t t, const char *name);
static int l_pthread_setname_np(pthread_t t, const char *name) {
    thread_place_named(t, name);
    return 0;
}
static int l_prctl(int option, unsigned long a2) {
    if (option == 15) thread_place_named(pthread_self(), (const char *)a2); // PR_SET_NAME
    return 0;
}
// The PS5 libc has no working getcwd/chdir: the app lives in /app0.
static char cwd[512] = APP0 "";
static char *l_getcwd(char *buf, size_t n) {
    if (!buf) return strdup(cwd);
    if (strlen(cwd) + 1 > n) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, cwd);
    return buf;
}
static int cwd_chosen; // the game chdir'd: relative paths from there

static int l_chdir(const char *path) {
    char ab[600];
    path = droid_abs(path, ab, sizeof(ab));
    if (!path || strlen(path) >= sizeof(cwd)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        droid_log("chdir(%s): no such folder", path);
        errno = ENOENT;
        return -1;
    }
    strcpy(cwd, path);
    cwd_chosen = 1;
    droid_log("chdir(%s)", cwd);
    return 0;
}

// A relative path: from the game's current folder (the host process's isn't the game's).
// Before any chdir: from the game's cache folder. Unity's GL shader cache asks for
// "UnityShaderCache/<hash>" there (the same files it also keeps at cache/UnityShaderCache);
// saved nowhere, its shaders were built again every run: stutters in every new area.
// Android's own folders, for every file call: scratch folders into the game's tmp, the
// phone's storage and app-data paths into the game's folder (before: only fopen knew the
// storage ones, and nothing knew /data/local/tmp - Shadow Fight 4, Shades and other new
// Unity games check file name case there, C# threw DirectoryNotFoundException, they quit)
static const struct { const char *android; const char *ours; } android_dirs[] = {
    { "/data/local/tmp", "tmp" }, { "/tmp", "tmp" }, { "/var/tmp", "tmp" },
    { "/storage/emulated/0", "" }, { "/sdcard", "" }, { "/mnt/sdcard", "" }, { "/data/data", "" },
};
static const char *android_dir(const char *path, char *buf, size_t n) {
    for (size_t i = 0; i < sizeof(android_dirs) / sizeof(android_dirs[0]); i++) {
        size_t l = strlen(android_dirs[i].android);
        if (strncmp(path, android_dirs[i].android, l) != 0 || (path[l] != 0 && path[l] != '/')) continue;
        char base[512];
        data_path(base, sizeof(base), android_dirs[i].ours);
        size_t bl = strlen(base);
        if (bl && base[bl - 1] == '/') base[--bl] = 0;
        mkdir(base, 0777); // there from the first call
        snprintf(buf, n, "%s%s", base, path + l);
        return buf;
    }
    return NULL;
}

// The phone's fonts: its font list and font files are Hydroid's (assets/fonts/system: Noto Sans CJK).
// Unity reads the list for the fonts it falls back on: none here, Dragon Ball Legends' Chinese and
// Korean language names were blank
static const char *system_fonts(const char *path, char *buf, size_t n) {
    if (!strcmp(path, "/system/etc/fonts.xml") || !strcmp(path, "/etc/fonts.xml") ||
        !strcmp(path, "/system/etc/system_fonts.xml") || !strcmp(path, "/etc/system_fonts.xml")) {
        snprintf(buf, n, APP0 "/assets/fonts/system/fonts.xml");
        return buf;
    }
    if (!strncmp(path, "/system/fonts", 13) && (path[13] == 0 || path[13] == '/')) {
        snprintf(buf, n, APP0 "/assets/fonts/system%s", path + 13);
        return buf;
    }
    return NULL;
}

// the path in the data folder (the game's view): what rename / unlink work on
// CACHE_SLOW_GAMES: Unity's download cache (<data>/UnityCache) kept in the game's own folder on
// the console's main storage, not the download data (/download0). Vector's story download
// (2026-11-01): a plain open(O_WRONLY|O_CREAT|O_TRUNC) of a cache __info file on /download0
// never came back, even with the cache's calls one at a time (the watchdog's line). Its
// cache lived in the game's folder before the fast disk and its downloads went on there.
#define CACHE_SLOW_GAMES "com.nekki.vector"
static const char *cache_slow_map(const char *path, char *buf, size_t n) {
    static int on = -1;
    if (on < 0) on = droid_game_in(CACHE_SLOW_GAMES) != 0;
    if (!on || !path || !game_data_dir[0] || !strcmp(game_data_dir, game_dir)) return path;
    const size_t l = strlen(game_data_dir);
    if (strncmp(path, game_data_dir, l) || strncmp(path + l, "/UnityCache", 11) ||
        (path[l + 11] && path[l + 11] != '/'))
        return path;
    char t[600];
    snprintf(t, sizeof(t), "%s%s", game_dir, path + l);
    snprintf(buf, n, "%s", t);
    static int told;
    if (!told++) droid_log("files: Unity's download cache in %s/UnityCache (CACHE_SLOW_GAMES)", game_dir);
    return buf;
}
static const char *data_abs_raw(const char *path, char *buf, size_t n);
static const char *data_abs(const char *path, char *buf, size_t n) {
    return cache_slow_map(data_abs_raw(path, buf, n), buf, n);
}
static const char *data_abs_raw(const char *path, char *buf, size_t n) {
    if (path && path[0] == '/') {
        const char *fonts = system_fonts(path, buf, n);
        if (fonts) return fonts;
        const char *mapped = android_dir(path, buf, n);
        if (mapped) return mapped;
    }
    if (!path || !*path || path[0] == '/') return path;
    char base[512];
    const char *from = cwd;
    if (!cwd_chosen) from = data_path(base, sizeof(base), "cache");
    if (path[0] == '.' && path[1] == 0) path = "";
    else if (path[0] == '.' && path[1] == '/') path += 2;
    snprintf(buf, n, "%s/%s", from, path);
    return buf;
}

// ... and where the file really is (a big one: on the fast disk, big_files.c)
const char *droid_abs(const char *path, char *buf, size_t n) {
    const char *p = data_abs(path, buf, n);
    if (!p || p[0] != '/') return p;
    char big[640];
    const char *m = big_map(p, big, sizeof(big));
    if (m == p) return p;
    snprintf(buf, n, "%s", m);
    return buf;
}

// ---- the write-behind's file calls (write_behind.c): its buffer first ---------------------
static int fd_trace(int fd) {
    static int on = -1;
    if (on < 0) on = getenv("DROID_FD_TRACE") != NULL;
    static int lines;
    return on && fd >= 60 && fd < 80 && lines++ < 400;
}
static long l_write(int fd, const void *p, size_t n) {
    long r = wb_write(fd, p, n);
    if (r == -2) r = (long)write(fd, p, n);
    if (fd_trace(fd)) droid_log("fd: write(%d, %zu) = %ld byte %u errno %d", fd, n, r,
                                 n ? (unsigned)((const unsigned char *)p)[0] : 0, errno);
    return r;
}
static long l_pwrite(int fd, const void *p, size_t n, long off) {
    wb_flush(fd);
    return (long)pwrite(fd, p, n, off);
}
static long l_writev(int fd, const struct iovec *v, int n) {
    wb_flush(fd);
    return (long)writev(fd, v, n);
}
static long l_lseek(int fd, long off, int whence) {
    long at;
    if (wb_lseek(fd, off, whence, &at)) return at; // where it is: no wait for the disk
    at = (long)lseek(fd, off, whence);
    if (at >= 0) wb_seeked(fd, at);
    return at;
}
static int l_fsync(int fd) {
    wb_flush(fd);
    return fsync(fd);
}
static int l_fdatasync(int fd) {
    wb_flush(fd);
    return fsync(fd);
}
static int l_ftruncate(int fd, long len) {
    wb_flush(fd);
    return ftruncate(fd, len);
}

// file calls with a path: relative ones from our cwd
#define ABS(p, b) char b[600]; p = droid_abs(p, b, sizeof(b))
static int l_no_links(void) { // link / symlink: not for a title's files (a missing one "succeeded")
    errno = EPERM;
    return -1;
}
static int l_mkdir(const char *p, int mode) {
    ABS(p, b1);
    double t0 = fs_now();
    int ct = cache_fs_begin("mkdir", p);
    int r = mkdir(p, (mode_t)mode);
    cache_fs_end(ct);
    fs_slow("mkdir", p, t0);
    return r;
}
#define DATA_ABS(p, b) char b[600]; p = data_abs(p, b, sizeof(b))
static int l_rmdir(const char *p) {
    DATA_ABS(p, b1);
    double t0 = fs_now();
    int ct = cache_fs_begin("rmdir", p);
    int r = rmdir(p);
    cache_fs_end(ct);
    fs_slow("rmdir", p, t0);
    if (r == 0) big_rmdir(p);
    return r;
}
static int l_unlink(const char *p) {
    DATA_ABS(p, b1);
    double t0 = fs_now();
    int ct = cache_fs_begin("unlink", p);
    int r = unlink(p);
    cache_fs_end(ct);
    fs_slow("unlink", p, t0);
    if (r == 0) big_unlinked(p);
    return r;
}
static int l_remove(const char *p) {
    DATA_ABS(p, b1);
    int r = remove(p);
    if (r == 0) {
        big_unlinked(p);
        big_rmdir(p);
    }
    return r;
}
static int l_access(const char *p, int m) {
    int pr = proc_stat(p, NULL);
    if (pr) return pr > 0 && !(m & 2) ? 0 : (errno = pr > 0 ? EACCES : ENOENT, -1); // read-only
    ABS(p, b1);
    int ct = cache_fs_begin("access", p);
    int r = access(p, m);
    cache_fs_end(ct);
    return r;
}
static int l_chmod(const char *p, int m) { ABS(p, b1); return chmod(p, (mode_t)m); }
static int l_truncate(const char *p, long len) { ABS(p, b1); return truncate(p, len); }
// a move the console refused: a folder made again, its files moved one by one (copied when
// even that is refused), the old one removed
static int copy_file(const char *a, const char *b) {
    int in = open(a, O_RDONLY), out = in >= 0 ? open(b, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    char buf[65536];
    ssize_t r = 0;
    while (out >= 0 && (r = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)r) != r) { r = -1; break; }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return out >= 0 && r == 0 ? 0 : -1;
}
static int move_by_hand(const char *a, const char *b) {
    struct stat st;
    if (stat(a, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        if (rename(a, b) == 0) return 0;
        unlink(b);
        if (copy_file(a, b) != 0) return -1;
        unlink(a);
        return 0;
    }
    mkdir(b, 0777);
    DIR *d = opendir(a);
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char from[1024], to[1024];
        snprintf(from, sizeof(from), "%s/%s", a, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", b, e->d_name);
        if (move_by_hand(from, to) != 0) rc = -1;
    }
    closedir(d);
    if (rc == 0) rmdir(a);
    return rc;
}

// rename: through the title's real folder when /app0 is read through it (the same files),
// and by hand when the console refuses (Vector: Unity's downloaded bundles never moved from
// its cache's Temp into place - "Unable to move cache file" - the game stayed black)
#ifdef DROID_PC
int droid_pc_ps5_rules(void); // pc/ps5_stubs.c
#endif
static int l_rename_data(const char *a, const char *b);
static int l_rename(const char *a, const char *b) { // the big files' copies follow (big_files.c)
    DATA_ABS(a, b1);
    DATA_ABS(b, b2);
    char ma[640], mb[640];
    double t0 = fs_now();
    wb_rename_begin();
    const char *big_a = big_map(a, ma, sizeof(ma)); // its copy on the fast disk, if a big one
    int r = l_rename_data(a, b);
    if (r == 0) {
        big_renamed(a, b);
        wb_renamed(a, b); // files being written there: by their new name (no wait)
        if (big_a != a) wb_renamed(big_a, big_map(b, mb, sizeof(mb)));
    }
    wb_rename_end();
    fs_slow("rename", a, t0);
    return r;
}
static int l_rename_data(const char *a, const char *b) {
    char ra[600], rb[600];
    const char *x = droid_real_path(a, ra, sizeof(ra)), *y = droid_real_path(b, rb, sizeof(rb));
    int e = 0;
#ifdef DROID_PC
    struct stat ds;
    int dir = stat(x, &ds) == 0 && S_ISDIR(ds.st_mode);
    if (dir && droid_pc_ps5_rules()) { // the console refuses moving a folder there (EBUSY)
        e = EBUSY;
    } else
#endif
    {
        int ct = cache_fs_begin("rename", a);
        int rr = rename(x, y);
        e = errno;
        cache_fs_end(ct);
        if (rr == 0) return 0;
    }
    if (e == ENOENT && access(x, F_OK) != 0) { errno = e; return -1; } // nothing to move
    static int told;
    if (told++ < 12) droid_log("rename(%s, %s) refused (errno %d): moved by hand", a, b, e);
    if (move_by_hand(x, y) == 0) return 0;
    errno = e;
    return -1;
}
static void *l_opendir(const char *p) { ABS(p, b1); return opendir(p); }

// scandir with the game's filter and order (ARM functions made callable: arm_imports.c). The list
// and its entries are malloc'd, the game frees them
typedef int (*scan_filter_fn)(const linux_dirent *);
typedef int (*scan_compar_fn)(const linux_dirent **, const linux_dirent **);
static scan_compar_fn scan_order;
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static int scan_qsort(const void *a, const void *b) { return scan_order(a, b); }
static int l_alphasort(const linux_dirent **a, const linux_dirent **b) { return strcoll((*a)->d_name, (*b)->d_name); }
int linux_scandir(const char *path, linux_dirent ***out, scan_filter_fn keep, scan_compar_fn order) {
    DIR *d = l_opendir(path);
    if (!d) return -1;
    linux_dirent **list = NULL, *e;
    int n = 0, cap = 0;
    while ((e = l_readdir(d))) {
        if (keep && !keep(e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            linux_dirent **grown = realloc(list, (size_t)cap * sizeof(*list));
            if (!grown) break;
            list = grown;
        }
        if (!(list[n] = malloc(sizeof(*e)))) break;
        memcpy(list[n++], e, sizeof(*e));
    }
    closedir(d);
    if (order && n > 1) {
        pthread_mutex_lock(&scan_lock);
        scan_order = order;
        qsort(list, (size_t)n, sizeof(*list), scan_qsort);
        pthread_mutex_unlock(&scan_lock);
    }
    *out = list;
    return n;
}
static char *l_realpath(const char *p, char *out) { ABS(p, b1); return realpath(p, out); }
static ssize_t l_readlink(const char *path, char *buf, size_t n) {
    (void)path; (void)buf; (void)n;
    errno = EINVAL;
    return -1;
}
static void l_nothing(void) {}
static int l_eai_fail(void) { return 4; } // EAI_FAIL
static const char *l_gai_strerror(int e) {
    (void)e;
    return "networking not available";
}


// ==== Unity / IL2CPP needs (Linux-on-ARM64 numbers and layouts) ===========================

#include <fnmatch.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <ctype.h>
#include <wctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <zlib.h>

// zlib's init calls from ARM code: a game's own zalloc / zfree in its z_stream are ARM functions,
// which our (x86) zlib would call as its own - a jump into ARM code (Wuthering Waves' Unreal:
// inflateInit2_ crashed at once). Cleared, zlib takes its own malloc / free; it writes them back
// into the stream, so inflateEnd / deflateEnd free with the same. (Only streams that crashed.)
static void z_own_alloc(z_streamp s) {
    if (s && (s->zalloc || s->zfree)) {
        static int told;
        if (told++ < 4) droid_log("zlib: a game allocator in a z_stream: ours instead");
        s->zalloc = Z_NULL;
        s->zfree = Z_NULL;
        s->opaque = Z_NULL;
    }
}
int droid_inflateInit2_(z_streamp s, int wb, const char *v, int n) { z_own_alloc(s); return inflateInit2_(s, wb, v, n); }
int droid_inflateInit_(z_streamp s, const char *v, int n) { z_own_alloc(s); return inflateInit_(s, v, n); }
int droid_deflateInit_(z_streamp s, int lvl, const char *v, int n) { z_own_alloc(s); return deflateInit_(s, lvl, v, n); }
int droid_deflateInit2_(z_streamp s, int lvl, int m, int wb, int mem, int st, const char *v, int n) {
    z_own_alloc(s);
    return deflateInit2_(s, lvl, m, wb, mem, st, v, n);
}

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelMprotect(const void *addr, size_t len, int prot);
uint64_t arm_guest_stack(uint64_t *size);

static int host_clock(int id) {
    switch (id) {
    case 0: return CLOCK_REALTIME;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    default: return CLOCK_MONOTONIC;
    }
}

static long l_clock(void) { return (long)((double)clock() * 1000000.0 / CLOCKS_PER_SEC); }
static int l_clock_getres(int id, struct timespec *ts) { return clock_getres(host_clock(id), ts); }
// ---- mutex / condition attributes: bionic's are a value (a long) in the game's memory,
// FreeBSD's a pointer to its own object. Kept bionic's way here; the host object is
// made in pthread_mutex_init / pthread_cond_init from that value. (Passed through,
// FreeBSD read a game's attribute as a pointer: a crash after 30 minutes of play.)
#define MA_TYPE 3u      // bionic: NORMAL 0, RECURSIVE 1, ERRORCHECK 2
#define MA_SHARED 0x10u
#define CA_SHARED 1u    // condattr: bit 0 shared, bits 1-3 the clock
static int l_mutexattr_init(long *a) { *a = 0; return 0; }
static int l_mutexattr_destroy(long *a) { *a = 0; return 0; }
static int l_mutexattr_settype(long *a, int type) {
    if (type < 0 || type > 2) return EINVAL;
    *a = (*a & ~(long)MA_TYPE) | type;
    return 0;
}
static int l_mutexattr_gettype(const long *a, int *type) { *type = (int)(*a & MA_TYPE); return 0; }
static int l_mutexattr_setpshared(long *a, int shared) { *a = shared ? (*a | MA_SHARED) : (*a & ~(long)MA_SHARED); return 0; }
static int l_mutexattr_getpshared(const long *a, int *shared) { *shared = (*a & MA_SHARED) != 0; return 0; }
static int l_mutexattr_noop(long *a, int v) { (void)a; (void)v; return 0; } // protocol, robust

static int l_mutex_init(pthread_mutex_t *m, const long *a) {
    if (!a) return pthread_mutex_init(m, NULL);
    pthread_mutexattr_t h;
    pthread_mutexattr_init(&h);
    int type = (int)(*a & MA_TYPE);
    pthread_mutexattr_settype(&h, type == 1 ? PTHREAD_MUTEX_RECURSIVE
                                  : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    int r = pthread_mutex_init(m, &h);
    pthread_mutexattr_destroy(&h);
    return r;
}

static int l_condattr_init(long *a) { *a = 0; return 0; }
static int l_condattr_destroy(long *a) { *a = 0; return 0; }
static int l_condattr_setclock(long *a, int id) { *a = (*a & CA_SHARED) | ((long)(id & 7) << 1); return 0; }
static int l_condattr_getclock(const long *a, int *id) { *id = (int)((*a >> 1) & 7); return 0; }
static int l_condattr_setpshared(long *a, int shared) { *a = shared ? (*a | CA_SHARED) : (*a & ~(long)CA_SHARED); return 0; }
static int l_condattr_getpshared(const long *a, int *shared) { *shared = (int)(*a & CA_SHARED); return 0; }

static int l_cond_init(pthread_cond_t *c, const long *a) {
    if (!a) return pthread_cond_init(c, NULL);
    pthread_condattr_t h;
    pthread_condattr_init(&h);
    pthread_condattr_setclock(&h, host_clock((int)((*a >> 1) & 7)));
    int r = pthread_cond_init(c, &h);
    pthread_condattr_destroy(&h);
    return r;
}

// ---- mmap: Linux flags; anonymous / file fallbacks in a console title ------------------

#define L_MAP_FIXED 0x10
#define L_MAP_ANON 0x20

static struct { void *addr; size_t len; int64_t phys; } dmaps[4096];
static int dmap_count;
static pthread_mutex_t dmap_lock = PTHREAD_MUTEX_INITIALIZER;

// Reserve-only mappings (prot 0, the way Unity / GCs grab address space): the
// console title has little memory for plain anonymous mmap, so the range is
// reserved without memory and 2 MB pieces get direct memory when mprotect (or
// a MAP_FIXED mmap) turns them on.
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);

#define VCHUNK (2u << 20)
typedef struct {
    char *base;
    size_t len;
    uint8_t *committed; // per VCHUNK
    int64_t *phys;
} vreserve;
static vreserve vres[256];
static int vres_count;

static vreserve *vres_at(const void *addr) {
    for (int i = 0; i < vres_count; i++)
        if ((const char *)addr >= vres[i].base && (const char *)addr < vres[i].base + vres[i].len)
            return &vres[i];
    return NULL;
}

#ifndef MAP_GUARD
#define MAP_GUARD 0x2000
#endif

// Address space without memory. Tries, logging each answer (first calls):
// FreeBSD's MAP_GUARD, then sceKernelReserveVirtualRange a few ways.
static void *vres_reserve(size_t len, void *hint) {
    static int tries_logged;
    int log = tries_logged++ < 4;
    size_t sz = (len + VCHUNK - 1) & ~(size_t)(VCHUNK - 1);
    if (vres_count == 256) return NULL;
    // where the game asked, when it is free (Linux's hint: Genshin Impact's GC builds its heap
    // at 0x7000000000 and counts from there - elsewhere, its sizes came out negative)
    if (hint && !((uintptr_t)hint & (VCHUNK - 1))) {
        void *t = mmap(hint, sz, PROT_NONE, MAP_GUARD, -1, 0);
        void *got = t == hint ? hint : NULL;
        if (!got && t != MAP_FAILED) munmap(t, sz);
        if (!got) {
            void *h = hint;
            int rc = sceKernelReserveVirtualRange(&h, sz, 0, VCHUNK);
            if (rc == 0 && h == hint) got = hint;
            else if (rc == 0) sceKernelMunmap(h, sz);
        }
        static int told;
        if (told++ < 20) droid_log("reserve %zu KB at the game's %p: %s", sz >> 10, hint, got ? "there" : "taken (elsewhere)");
        if (got) {
            size_t chunks = sz / VCHUNK;
            vreserve *v = &vres[vres_count];
            v->committed = calloc(chunks, 1);
            v->phys = calloc(chunks, sizeof(int64_t));
            if (!v->committed || !v->phys) return NULL;
            v->base = got;
            v->len = sz;
            vres_count++;
            return got;
        }
    }
    void *a = mmap(droid_va_hint(sz + VCHUNK), sz + VCHUNK, PROT_NONE, MAP_GUARD, -1, 0);
    if (log) droid_log("reserve %zu KB: MAP_GUARD -> %p (errno %d)", sz >> 10, a, a == MAP_FAILED ? errno : 0);
    if (a != MAP_FAILED) { // align the start to a chunk
        uintptr_t u = ((uintptr_t)a + VCHUNK - 1) & ~(uintptr_t)(VCHUNK - 1);
        a = (void *)u;
    } else {
        static const struct { uintptr_t hint; size_t align; } ways[] = {
            { 1, 2u << 20 }, { 1, 0x10000 }, { 0, 0x10000 }, { 0, 0x4000 }, // 1: our hint (out of RADV's window)
        };
        a = NULL;
        for (size_t w = 0; w < sizeof(ways) / sizeof(ways[0]); w++) {
            void *h = ways[w].hint ? droid_va_hint(sz) : NULL;
            int rc = sceKernelReserveVirtualRange(&h, sz, 0, ways[w].align);
            if (log) droid_log("reserve %zu KB: sceKernelReserveVirtualRange(hint %#lx, align %#zx) = %#x -> %p",
                               sz >> 10, (unsigned long)ways[w].hint, ways[w].align, rc, h);
            if (rc == 0) {
                a = h;
                break;
            }
        }
        if (!a) return NULL;
    }
    size_t chunks = sz / VCHUNK;
    vreserve *v = &vres[vres_count];
    v->committed = calloc(chunks, 1);
    v->phys = calloc(chunks, sizeof(int64_t));
    if (!v->committed || !v->phys) return NULL;
    v->base = a;
    v->len = sz;
    vres_count++;
    return a;
}

// The direct-memory type for the game's memory: 12 unless the speed test (droid_bench)
// found a faster one. Code (exec) always 12.
static int vres_type = 12;

// Gives [addr, addr+len) memory (zeroed) with prot. 0 = ok.
static int vres_commit(vreserve *v, const void *addr, size_t len, int prot) {
    size_t from = (size_t)((const char *)addr - v->base) / VCHUNK;
    size_t to = (size_t)((const char *)addr + len - v->base + VCHUNK - 1) / VCHUNK;
    if (to > v->len / VCHUNK) to = v->len / VCHUNK;
    // read+write first, executable after (the mprotect below): the console refuses a mapping
    // made executable at once - V8 (Vector, Shadow Fight 3 / 4) stopped out of memory there
    int kprot = 0x03;
    for (size_t i = from; i < to; i++) {
        if (v->committed[i]) continue;
        int64_t phys = 0;
        void *at = v->base + i * VCHUNK;
        static int fails;
        int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), VCHUNK, VCHUNK,
                                               (prot & 4) ? 12 : vres_type, &phys);
        if (rc != 0) {
            if (fails++ < 4) droid_log("commit: allocate 2 MB = %#x", rc);
            return -1;
        }
        rc = sceKernelMapDirectMemory(&at, VCHUNK, kprot, 0x10 /* fixed */, phys, VCHUNK);
        if (rc != 0) {
            if (fails++ < 4) droid_log("commit: map 2 MB at %p = %#x", (void *)(v->base + i * VCHUNK), rc);
            sceKernelReleaseDirectMemory(phys, VCHUNK);
            return -1;
        }
        memset(at, 0, VCHUNK);
        v->committed[i] = 1;
        v->phys[i] = phys;
    }
    if (prot & 4) sceKernelMprotect(addr, len, 0x07);
    return 0;
}

static void vres_release(vreserve *v) {
    for (size_t i = 0; i < v->len / VCHUNK; i++) {
        if (!v->committed[i]) continue;
        sceKernelMunmap(v->base + i * VCHUNK, VCHUNK);
        sceKernelReleaseDirectMemory(v->phys[i], VCHUNK);
    }
    sceKernelMunmap(v->base, v->len);
    free(v->committed);
    free(v->phys);
    *v = vres[--vres_count];
}

// ---- speed test (once at start): our committed memory vs malloc, file reads into each --

static double bench_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double bench_mem(const char *what, unsigned char *p, size_t n) {
    double t0 = bench_now();
    for (int r = 0; r < 16; r++) memset(p, r, n);
    double t1 = bench_now();
    volatile uint64_t sum = 0;
    for (int r = 0; r < 16; r++)
        for (size_t i = 0; i < n; i += 8) sum += *(volatile uint64_t *)(p + i);
    double t2 = bench_now();
    double w = 16.0 * n / (t1 - t0) / 1e6, r = 16.0 * n / (t2 - t1) / 1e6;
    droid_log("bench: %s memory: write %.0f MB/s, read %.0f MB/s", what, w, r);
    return w < r ? w : r; // the slower of the two
}

static double bench_file(const char *what, const char *path, unsigned char *p, size_t n, long off) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    double t0 = bench_now();
    ssize_t got = pread(fd, p, n, off);
    double t1 = bench_now();
    unsigned char *tmp = malloc(16384);
    double t2 = bench_now();
    size_t small = 0;
    for (int i = 0; i < 64 && tmp; i++) {
        ssize_t r = pread(fd, tmp, 16384, off + (long)n + i * 16384L);
        if (r > 0) small += (size_t)r;
    }
    double t3 = bench_now();
    free(tmp);
    close(fd);
    double big = got > 0 ? got / (t1 - t0) / 1e6 : 0.0, many = small / (t3 - t2) / 1e6;
    droid_log("bench: %s via %s: %zd bytes in one read %.0f MB/s; 64 x 16 KB reads %.0f MB/s", what, path, got,
              big, many);
    return many;
}

// The title's own folder when /app0 (its mount) reads slower: "" = /app0 is fine.
static char app0_real[128];

// path under /app0 -> the same file in the real folder when that's faster.
const char *droid_real_path(const char *path, char *buf, size_t n) {
    if (!app0_real[0] || strncmp(path, APP0 "/", sizeof(APP0)) != 0) return path;
    snprintf(buf, n, "%s/%s", app0_real, path + sizeof(APP0));
    return buf;
}

static vreserve *vres_at(const void *addr);
static void *vres_reserve(size_t len, void *hint);
static int vres_commit(vreserve *v, const void *addr, size_t len, int prot);

// The game's APKs read once, start to end, on a thread of their own while Unity starts:
// its many small reads later come from the system's file cache.
static void *readahead_thread(void *arg) {
    char *path = arg;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char *buf = malloc(1 << 20);
        long long total = 0;
        ssize_t r;
        double t0 = bench_now();
        while (buf && (r = read(fd, buf, 1 << 20)) > 0) total += r;
        droid_log("readahead: %s, %lld MB in %.1f s", path, total >> 20, bench_now() - t0);
        free(buf);
        close(fd);
    }
    free(path);
    return NULL;
}

void droid_readahead(const char *path) {
    pthread_t t;
    if (pthread_create(&t, NULL, readahead_thread, strdup(path)) == 0) pthread_detach(t);
}

// ---- FsBench (the debug tool, tools.txt "FsBench"): what file work costs in each writable
// place the console may give us - Unity's cache (create, write, open, rename, folders) cost
// ~1-11 ms an operation in /app0 (Vector's menus froze for seconds). Logged per place, ms each.
static void fs_bench_one(const char *base) {
    char dir[300], f[340], g[340];
    snprintf(dir, sizeof(dir), "%s/fsbench%d", base, (int)getpid());
    double t0 = bench_now();
    if (mkdir(dir, 0777) != 0) {
        droid_log("FsBench: %s: can't make a folder there (errno %d)", base, errno);
        return;
    }
    double t1 = bench_now();
    char data[4096];
    memset(data, 7, sizeof(data));
    for (int i = 0; i < 20; i++) { // create + write 4 KB + close
        snprintf(f, sizeof(f), "%s/f%d", dir, i);
        int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            (void)!write(fd, data, sizeof(data));
            close(fd);
        }
    }
    double t2 = bench_now();
    for (int i = 0; i < 20; i++) { // open + read + close an existing file
        snprintf(f, sizeof(f), "%s/f%d", dir, i);
        int fd = open(f, O_RDONLY);
        if (fd >= 0) {
            (void)!read(fd, data, sizeof(data));
            close(fd);
        }
    }
    double t3 = bench_now();
    for (int i = 0; i < 20; i++) { // rewrite in place (Unity's __info)
        snprintf(f, sizeof(f), "%s/f%d", dir, i);
        int fd = open(f, O_WRONLY);
        if (fd >= 0) {
            (void)!write(fd, data, 64);
            close(fd);
        }
    }
    double t4 = bench_now();
    for (int i = 0; i < 20; i++) { // rename a file
        snprintf(f, sizeof(f), "%s/f%d", dir, i);
        snprintf(g, sizeof(g), "%s/g%d", dir, i);
        rename(f, g);
    }
    double t5 = bench_now();
    int dir_ok = 0;
    for (int i = 0; i < 10; i++) { // a folder made, renamed, removed
        snprintf(f, sizeof(f), "%s/d%d", dir, i);
        snprintf(g, sizeof(g), "%s/e%d", dir, i);
        mkdir(f, 0777);
        if (rename(f, g) == 0) {
            dir_ok++;
            rmdir(g);
        } else {
            rmdir(f);
        }
    }
    double t6 = bench_now();
    for (int i = 0; i < 20; i++) { // stat
        struct stat st;
        snprintf(g, sizeof(g), "%s/g%d", dir, i);
        stat(g, &st);
    }
    double t7 = bench_now();
    for (int i = 0; i < 20; i++) {
        snprintf(g, sizeof(g), "%s/g%d", dir, i);
        unlink(g);
    }
    double t8 = bench_now();
    rmdir(dir);
    droid_log("FsBench: %s: mkdir %.2f | create+write %.2f | open+read %.2f | rewrite %.2f | rename %.2f | "
              "folder make+rename+remove %.2f (%d/10 renamed) | stat %.3f | unlink %.2f  (ms each)",
              base, (t1 - t0) * 1e3, (t2 - t1) * 1e3 / 20, (t3 - t2) * 1e3 / 20, (t4 - t3) * 1e3 / 20,
              (t5 - t4) * 1e3 / 20, (t6 - t5) * 1e3 / 10, dir_ok, (t7 - t6) * 1e3 / 20, (t8 - t7) * 1e3 / 20);
}
// big writes: up to 1.5 GB in 8 MB writes, MB/s per 256 MB (an app's writes are throttled after a
// burst, ~1.3 GB, then ~2 MB/s - Unity's extraction of a big download), 20 s at most a place
static void fs_bench_bulk(const char *base) {
    char f[300];
    snprintf(f, sizeof(f), "%s/fsbench-bulk%d.bin", base, (int)getpid());
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    const size_t chunk = 8u << 20;
    char *buf = malloc(chunk);
    if (!buf) {
        close(fd);
        unlink(f);
        return;
    }
    memset(buf, 0x5a, chunk);
    char line[400];
    int at = snprintf(line, sizeof(line), "FsBench: %s big writes, MB/s per 256 MB:", base);
    double start = bench_now(), step = start;
    long done = 0;
    for (int i = 0; i < 192; i++) { // 192 x 8 MB = 1.5 GB
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            at += snprintf(line + at, sizeof(line) - (size_t)at, " (write failed, errno %d)", errno);
            break;
        }
        done++;
        if (done % 32 == 0) {
            double t = bench_now();
            at += snprintf(line + at, sizeof(line) - (size_t)at, " %.0f", 256.0 / (t - step));
            step = t;
        }
        if (bench_now() - start > 20) {
            at += snprintf(line + at, sizeof(line) - (size_t)at, " (20 s: stopped at %ld MB)", done * 8);
            break;
        }
    }
    snprintf(line + at, sizeof(line) - (size_t)at, " | %ld MB in %.1f s", done * 8, bench_now() - start);
    droid_log("%s", line);
    free(buf);
    close(fd);
    unlink(f);
}

void droid_fs_bench(void) {
    fs_bench_bulk(APP0 "/data");
    fs_bench_bulk("/download0");
    static const char *const places[] = { APP0 "/data", "/data", "/user/temp", "/temp0", "/download0", "/tmp",
                                          APP0, "/system_tmp", "/user/data" };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) fs_bench_one(places[i]);
}

// Memory and file speed once at start, and the faster choice kept: the direct-memory
// type for the game's memory, and the title's real folder instead of /app0.
void droid_bench(const char *file) {
    const size_t n = 2u << 20;
    unsigned char *heap = malloc(n);
    if (heap) bench_mem("malloc", heap, n);
    // direct-memory types: 12 (now), then others the PS5 may give for CPU memory
    static const int types[] = { 12, 0, 10 };
    double best = 0;
    int best_type = 12;
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        int64_t phys = 0;
        void *m = NULL;
        int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), n, n, types[t], &phys);
        if (rc != 0) { droid_log("bench: memory type %d: allocate %#x", types[t], rc); continue; }
        rc = sceKernelMapDirectMemory(&m, n, 0x03, 0, phys, n);
        if (rc != 0) {
            droid_log("bench: memory type %d: map %#x", types[t], rc);
            sceKernelReleaseDirectMemory(phys, n);
            continue;
        }
        char what[48];
        snprintf(what, sizeof(what), "direct type %d", types[t]);
        double v = bench_mem(what, m, n);
        if (heap && t == 0) bench_file("into type 12", file, m, n, 8L << 20);
        if (v > best * 1.5) { best = v; best_type = types[t]; }
        sceKernelMunmap(m, n);
        sceKernelReleaseDirectMemory(phys, n);
    }
    if (best_type != 12) droid_log("bench: game memory now direct type %d (faster)", best_type);
    vres_type = best_type;
    // the APK through /app0 and through the title's real folder
    if (file && heap && strncmp(file, APP0 "/", sizeof(APP0)) == 0) {
        static const char *dirs[] = { APP0 };
        double via_app0 = bench_file("file", file, heap, n, 16L << 20);
        for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
            char real[256];
            snprintf(real, sizeof(real), "%s/%s", dirs[d], file + sizeof(APP0));
            struct stat a, b;
            if (stat(real, &b) != 0 || stat(file, &a) != 0 || a.st_size != b.st_size) continue;
            double v = bench_file("file", real, heap, n, 24L << 20);
            if (v > via_app0 * 2) {
                snprintf(app0_real, sizeof(app0_real), "%s", dirs[d]);
                droid_log("bench: game files now read from %s (faster than /app0)", dirs[d]);
            }
            break;
        }
    }
    free(heap);
}

// Read-only file mappings, kept: Unity maps libil2cpp.so (80 MB) and more for every
// exception's stack trace; read each time, that was GBs. The same file part again: the
// same memory (it's never written); munmap leaves it here for the next time.
typedef struct {
    dev_t dev;
    ino_t ino;
    off_t size;
    time_t mtime;
    long off;
    size_t len;
    void *addr;
} file_map;
#define FILE_MAPS 64
static file_map file_maps[FILE_MAPS];
static int file_map_count;

static file_map *file_map_find(const struct stat *st, long off, size_t len) {
    for (int i = 0; i < file_map_count; i++) {
        file_map *f = &file_maps[i];
        if (f->dev == st->st_dev && f->ino == st->st_ino && f->size == st->st_size && f->mtime == st->st_mtime &&
            f->off == off && f->len == len)
            return f;
    }
    return NULL;
}

static int file_map_at(const void *addr) {
    for (int i = 0; i < file_map_count; i++)
        if (file_maps[i].addr == addr) return 1;
    return 0;
}

static int mm_log(void);
static void mm_say(const char *what, void *addr, size_t len, long a, long b);
static void *l_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    if (fd >= 0) wb_flush(fd);
    if (mm_log()) mm_say("mmap", addr, len, prot, flags);
    if ((long)len < 0) { // a size gone negative: who asks (once), and no mapping
        static int told;
        if (!told++) {
            extern void arm_guest_fp_chain(const char *why);
            droid_log("mmap: a negative size %#zx asked (at %p)", len, addr);
            arm_guest_fp_chain("negative mmap");
        }
        errno = ENOMEM;
        return MAP_FAILED;
    }
    static int logged;
    if (logged < 24) {
        logged++;
        droid_log("mmap(%p, %zu KB, prot %d, flags %#x, fd %d, off %ld)", addr, len >> 10, prot, flags, fd, off);
    }
    pthread_mutex_lock(&dmap_lock);
    vreserve *v = (flags & L_MAP_FIXED) ? vres_at(addr) : NULL;
    if (v) { // MAP_FIXED over a reservation: turn that part on
        int rc = vres_commit(v, addr, len, prot ? prot : 3);
        pthread_mutex_unlock(&dmap_lock);
        return rc == 0 ? addr : MAP_FAILED;
    }
    if ((flags & L_MAP_ANON) && prot == 0 && !(flags & L_MAP_FIXED)) {
        void *r = vres_reserve(len, addr);
        pthread_mutex_unlock(&dmap_lock);
        if (r) return r;
        droid_log("mmap: no way to reserve %zu KB; trying real memory", len >> 10);
        prot = 3; // falls through to the direct-memory path below
    }
    pthread_mutex_unlock(&dmap_lock);
    // A file (IL2CPP's global-metadata.dat ...): read into our own memory, never a
    // host file mapping (a title's file mmap isn't to be trusted). Read-only use only.
    int file = !(flags & L_MAP_ANON) && fd >= 0 && !(flags & L_MAP_FIXED) && !getenv("DROID_HOST_MMAP");
    struct stat fst;
    int cacheable = file && !(prot & 2) && fstat(fd, &fst) == 0;
    if (cacheable) {
        pthread_mutex_lock(&dmap_lock);
        file_map *hit = file_map_find(&fst, off, len);
        void *a = hit ? hit->addr : NULL;
        pthread_mutex_unlock(&dmap_lock);
        if (a) {
            static int hits;
            if (hits++ % 100 == 0) droid_log("mmap file: %zu KB at offset %ld again: kept copy (%d times)", len >> 10, off, hits);
            return a;
        }
    }
    static int file_logged;
    if (file && file_logged++ < 40)
        droid_log("mmap file: fd %d, %zu KB at offset %ld, prot %d, flags %#x", fd, len >> 10, off, prot, flags);
    // the game's big memory straight from direct memory: the console's own mmap takes it out
    // of the small pool malloc and thread stacks live on (Shadow Fight 3: "flexible memory free
    // 0 KB", threads refused, its downloads stuck at 0%). Small ones (and MAP_FIXED) stay there.
    pthread_mutex_lock(&dmap_lock);
    int to_direct = (flags & L_MAP_ANON) && !(flags & L_MAP_FIXED) && len >= (64 << 10) && dmap_count < 4000;
    pthread_mutex_unlock(&dmap_lock);
    if (!file && !to_direct) {
        int hf = (flags & 3) | ((flags & L_MAP_FIXED) ? MAP_FIXED : 0) | ((flags & L_MAP_ANON) ? MAP_ANON : 0);
        void *p = mmap(addr, len, prot, hf, (flags & L_MAP_ANON) ? -1 : fd, off);
        if (p != MAP_FAILED || (flags & L_MAP_FIXED)) return p;
        static int told;
        if (!told++) droid_log("mmap: host mmap refused (%d), using direct memory", errno);
    }
    size_t sz = (len + 0xFFFF) & ~(size_t)0xFFFF;
    int64_t phys = 0;
    // where the game asked (a hint, as on Linux: tried, else ours), or ours: out of RADV's window
    void *m = addr && !((uintptr_t)addr & 0xFFFF) ? addr : droid_va_hint(sz);
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, 0x10000, 12, &phys) != 0)
        return MAP_FAILED;
    int mrc = sceKernelMapDirectMemory(&m, sz, 0x03, 0, phys, 0x10000);
    if (mrc != 0 && m != NULL && addr) { // its address refused: ours
        m = droid_va_hint(sz);
        mrc = sceKernelMapDirectMemory(&m, sz, 0x03, 0, phys, 0x10000);
    }
    if (mrc != 0) {
        sceKernelReleaseDirectMemory(phys, sz);
        return MAP_FAILED;
    }
    memset(m, 0, sz);
    if (!(flags & L_MAP_ANON) && fd >= 0) {
        size_t got = 0;
        while (got < len) {
            ssize_t r = pread(fd, (char *)m + got, len - got, off + (long)got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (file_logged <= 40) droid_log("mmap file: read %zu of %zu bytes -> %p", got, len, m);
        if (cacheable && got == len) {
            pthread_mutex_lock(&dmap_lock);
            if (file_map_count < FILE_MAPS && !file_map_find(&fst, off, len))
                file_maps[file_map_count++] = (file_map){ fst.st_dev, fst.st_ino, fst.st_size, fst.st_mtime, off, len, m };
            pthread_mutex_unlock(&dmap_lock);
        }
    }
    if (prot & 4) sceKernelMprotect(m, sz, 0x07);
    pthread_mutex_lock(&dmap_lock);
    if (dmap_count < 4096) {
        dmaps[dmap_count].addr = m;
        dmaps[dmap_count].len = sz;
        dmaps[dmap_count].phys = phys;
        dmap_count++;
    }
    pthread_mutex_unlock(&dmap_lock);
    return m;
}

// Only a whole direct-memory block is given back. Allocators map extra and unmap
// the unaligned ends (start or tail of the block): that part just stays mapped.
// DROID_MM_LOG=1: every memory call of the game, with who made it (a JIT of its own: V8)
static int mm_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("DROID_MM_LOG") != NULL || droid_tool("MemTrace"); // MemTrace
    return on;
}
static void mm_say(const char *what, void *addr, size_t len, long a, long b) {
    char from[64];
    arm_call_from(from, sizeof(from));
    droid_log("mm: %s(%p, %zu KB, %#lx, %#lx) from %s", what, addr, len >> 10, a, b, from);
}

static int l_munmap(void *addr, size_t len) {
    if (mm_log()) mm_say("munmap", addr, len, 0, 0);
    pthread_mutex_lock(&dmap_lock);
    if (file_map_at(addr)) { // a kept file mapping: stays for the next mmap of it
        pthread_mutex_unlock(&dmap_lock);
        return 0;
    }
    vreserve *v = vres_at(addr);
    if (v) { // the whole reservation goes; part of it stays reserved
        if ((char *)addr == v->base && len >= v->len) vres_release(v);
        pthread_mutex_unlock(&dmap_lock);
        return 0;
    }
    for (int i = 0; i < dmap_count; i++) {
        char *b = dmaps[i].addr;
        if ((char *)addr < b || (char *)addr >= b + dmaps[i].len) continue;
        if ((char *)addr == b && len >= dmaps[i].len) {
            sceKernelMunmap(dmaps[i].addr, dmaps[i].len);
            sceKernelReleaseDirectMemory(dmaps[i].phys, dmaps[i].len);
            dmaps[i] = dmaps[--dmap_count];
        }
        pthread_mutex_unlock(&dmap_lock);
        return 0;
    }
    pthread_mutex_unlock(&dmap_lock);
    // the PS5's pages are 16 KB, Android's 4 KB: a munmap at a 4 KB step (V8 trims its pages
    // so) would take the whole 16 KB page with what is still in use below it. Only the 16 KB
    // pages wholly inside go (the PC does the same: what shows here shows there)
    uintptr_t from = ((uintptr_t)addr + 0x3fff) & ~(uintptr_t)0x3fff;
    uintptr_t to = ((uintptr_t)addr + len) & ~(uintptr_t)0x3fff;
    if (to <= from) return 0;
    return munmap((void *)from, to - from);
}

static int l_mprotect(void *addr, size_t len, int prot) {
    if (mm_log()) mm_say("mprotect", addr, len, prot, 0);
    static int logged;
    if (logged < 24) {
        logged++;
        droid_log("mprotect(%p, %zu KB, prot %d)", addr, len >> 10, prot);
    }
    pthread_mutex_lock(&dmap_lock);
    vreserve *v = vres_at(addr);
    if (v) { // turning part of a reservation on; turning it off keeps the memory
        int rc = prot ? vres_commit(v, addr, len, prot) : 0;
        pthread_mutex_unlock(&dmap_lock);
        if (rc != 0) errno = ENOMEM;
        return rc;
    }
    pthread_mutex_unlock(&dmap_lock);
#ifndef DROID_PC
    // Unreal games: the game thinks pages are 4 KB, the console's are 16 KB. Taking write (or all)
    // access away from part of a 16 KB page took it from its neighbours too (V8 made one 4 KB page
    // read-only, then wrote next to it: Wuthering Waves' crash). Only whole 16 KB pages lose it.
    if ((droid_quirks & QUIRK_UNREAL) && !(prot & 2)) {
        uintptr_t s = ((uintptr_t)addr + 0x3FFF) & ~(uintptr_t)0x3FFF;
        uintptr_t e = ((uintptr_t)addr + len) & ~(uintptr_t)0x3FFF;
        if (s >= e) return 0;
        addr = (void *)s;
        len = e - s;
    }
#endif
    if (mprotect(addr, len, prot) == 0) return 0;
    return sceKernelMprotect(addr, len, prot) == 0 ? 0 : -1;
}

int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, uint32_t *prot);
static int l_madvise(void *addr, size_t len, int advice) {
    if (mm_log()) mm_say("madvise", addr, len, advice, 0);
    static int told;
    if (told < 16) {
        told++;
        droid_log("madvise(%p, %zu KB, %d)", addr, len >> 10, advice);
    }
    // Linux's MADV_DONTNEED (4) on private memory: the pages read as zeros after. miHoYo's
    // Noelle GC gives pages back so and reuses them uncleared (Genshin Impact: its dictionaries
    // came out broken, "Index was outside the bounds of the array"). FreeBSD keeps the bytes.
    // (miHoYo's and Unreal games only: every other game had it as nothing, and works so)
    if (advice != 4 || !len || !(droid_quirks & (QUIRK_MIHOYO | QUIRK_UNREAL))) return 0;
    // A loaded library was a file mapping on Android: MADV_DONTNEED there re-reads the file.
    // (Wuthering Waves' packer decrypts half the code and drops the rest this way.)
    if (droid_quirks & QUIRK_UNREAL) {
        extern int arm_module_madvise(void *, size_t);
        if (arm_module_madvise(addr, len)) return 0;
    }
#ifdef DROID_PC
    return madvise(addr, len, MADV_DONTNEED);
#else
    // the writable parts cleared by hand (a reservation's untouched part is skipped)
    char *p = (char *)((uintptr_t)addr & ~(uintptr_t)0x3FFF), *end = (char *)addr + len;
    while (p < end) {
        void *s0, *e0;
        uint32_t prot;
        if (sceKernelQueryMemoryProtection(p, &s0, &e0, &prot) != 0) break;
        char *e = (char *)e0 < end ? (char *)e0 : end;
        char *from = (char *)s0 > p ? (char *)s0 : p;
        if (from < (char *)addr) from = (char *)addr;
        if ((prot & 2) && e > from) memset(from, 0, (size_t)(e - from));
        if ((char *)e0 <= p) break;
        p = (char *)e0;
    }
    return 0;
#endif
}

// Big blocks the console's malloc refuses (its pool is small): from direct memory, as a phone's
// malloc would hand them out of mmap. Genshin Impact asks 7 GB (label File) at its first
// resource check: malloc NULL -> "System out of memory", then a read at 0. Only taken when
// malloc failed, so what works stays as it is. free / realloc / malloc_usable_size know them.
#define BIG_ALLOCS 64
static struct { void *addr; size_t len; } big_allocs[BIG_ALLOCS];
static volatile int big_alloc_count;
void *droid_big_alloc(size_t size) {
    if (size < (64u << 20) || big_alloc_count >= BIG_ALLOCS) return NULL;
    void *p = l_mmap(NULL, size, 3, L_MAP_ANON | 0x02 /* MAP_PRIVATE */, -1, 0);
    if (p == MAP_FAILED) {
        char from[64];
        arm_call_from(from, sizeof(from));
        droid_log("malloc: %zu MB refused, and no direct memory for it either (from %s)", size >> 20, from);
        return NULL;
    }
    pthread_mutex_lock(&dmap_lock);
    int i = 0;
    while (i < BIG_ALLOCS && big_allocs[i].addr) i++;
    if (i < BIG_ALLOCS) {
        big_allocs[i].addr = p;
        big_allocs[i].len = size;
        big_alloc_count++;
    }
    pthread_mutex_unlock(&dmap_lock);
    if (i == BIG_ALLOCS) {
        l_munmap(p, size);
        return NULL;
    }
    char from[64];
    arm_call_from(from, sizeof(from));
    droid_log("malloc: %zu MB from direct memory (the console's malloc refused) at %p, from %s", size >> 20, p, from);
    return p;
}
// its size if p is one of them, else 0; release: given back too
static size_t big_alloc_find(void *p, int release) {
    if (!big_alloc_count || !p) return 0;
    size_t len = 0;
    pthread_mutex_lock(&dmap_lock);
    for (int i = 0; i < BIG_ALLOCS; i++)
        if (big_allocs[i].addr == p) {
            len = big_allocs[i].len;
            if (release) {
                big_allocs[i].addr = NULL;
                big_alloc_count--;
            }
            break;
        }
    pthread_mutex_unlock(&dmap_lock);
    if (len && release) l_munmap(p, len);
    return len;
}
size_t droid_big_size(void *p) { return big_alloc_find(p, 0); }
int droid_big_free(void *p) { return big_alloc_find(p, 1) != 0; }

static void *l_memalign(size_t align, size_t size) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (size >= (1ull << 30)) { // (a GB or more: who asks, to compare the PC with the console)
        char from[64];
        arm_call_from(from, sizeof(from));
        droid_log("memalign: %zu MB asked, from %s", size >> 20, from);
    }
    if (posix_memalign(&p, align, size) == 0) return p;
    return align <= 0x10000 ? droid_big_alloc(size) : NULL;
}

// ---- fcntl: open-flag numbers, lock commands -------------------------------------

static int linux_open_flags(int h) {
    static const struct { int linux_bit, host; } map[] = {
        { 0100, O_CREAT }, { 0200, O_EXCL }, { 01000, O_TRUNC }, { 02000, O_APPEND },
        { 04000, O_NONBLOCK }, { 040000, O_DIRECTORY }, { 0100000, O_NOFOLLOW },
    };
    int f = h & 3;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (h & map[i].host) f |= map[i].linux_bit;
    return f;
}

#define GETLK_FREE_GAMES "com.king.candycrushsaga" // F_GETLK answers "unlocked" (SQLite WAL)
static int l_fcntl(int fd, int cmd, long arg) {
    if (cmd == 0 || cmd == 1030) wb_forget(fd); // F_DUPFD / F_DUPFD_CLOEXEC
    switch (cmd) {
    case 0: return fcntl(fd, F_DUPFD, (int)arg);
    case 1: return fcntl(fd, F_GETFD);
    case 2: { // F_SETFD (close-on-exec: nothing is ever exec'd here). The console refuses it for
        // pipes; curl then gave up every name lookup ("Cannot resolve destination host")
        int r = fcntl(fd, F_SETFD, (int)arg);
        if (r < 0) {
            static int said;
            if (!said++) droid_log("fcntl(%d, F_SETFD, %ld) refused by the console (errno %d): taken as done", fd, arg, errno);
            r = 0;
        }
        return r;
    }
    case 3: {
        int h = fcntl(fd, F_GETFL);
        return h < 0 ? h : linux_open_flags(h) | (net_nonblock(fd) ? 04000 : 0); // as the game set it
    }
    case 4: {
        net_nonblock_set(fd, (arg & 04000) != 0);
        // only what F_SETFL changes (append, non-blocking, async, direct): Linux ignores the
        // rest, the console refuses the call (Vector's server socket: O_RDWR | O_NONBLOCK,
        // EACCES, "Access denied", its game offline after the first level)
        long changeable = arg & (02000 | 04000 | 020000 | 040000);
        int r = fcntl(fd, F_SETFL, host_open_flags((int)changeable));
        int fionbio_ok = 0;
#ifdef DROID_PC
        { // PS5 rules: the console refuses F_SETFL on a socket (EACCES)
            int ty = 0;
            socklen_t tl = sizeof(ty);
            if (droid_pc_ps5_rules() && getsockopt(fd, SOL_SOCKET, SO_TYPE, &ty, &tl) == 0) {
                r = -1;
                errno = EACCES;
            }
        }
#endif
#ifndef __linux__
        // the console's sockets ignore O_NONBLOCK through fcntl: a "non-blocking" socket blocked
        // in recv while its server waited for the rest of a POST (Shadow Fight 2's put.php).
        // FIONBIO too (a plain file refuses it, no harm)
        {
            int nb = (arg & 04000) != 0;
            int io = ioctl(fd, FIONBIO, &nb);
            fionbio_ok = io == 0;
            static int said;
            if (io == 0 && said++ < 3) {
                int fl = fcntl(fd, F_GETFL);
                droid_log("fcntl(%d, F_SETFL, %#lx): non-blocking %s through FIONBIO (F_GETFL says %s)", fd, arg,
                          nb ? "on" : "off", fl >= 0 && (fl & O_NONBLOCK) ? "non-blocking" : "blocking");
            }
        }
#endif
        if (r < 0 && fionbio_ok && !(changeable & ~04000L)) r = 0; // non-blocking was all, and it's set
        if (r < 0) { // the console refuses F_SETFL (and FIONBIO) on a socket: net_shim keeps the
            // non-blocking setting itself (MSG_DONTWAIT on every call), set above - done (Vector:
            // "Access denied", its handshake failed, the game offline)
            int ty = 0;
            socklen_t tl = sizeof(ty);
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &ty, &tl) == 0) r = 0;
        }
        if (r < 0) {
            static int said;
            if (said++ < 4) droid_log("fcntl(%d, F_SETFL, %#lx) failed (errno %d)", fd, arg, errno);
        }
        return r;
    }
    case 5: // F_GETLK: "who holds it?" - nobody (one process): l_type F_UNLCK (2), the struct's first
        // short. Left as asked (a write lock), SQLite saw another process holding its WAL and
        // slept in its busy loop for good (Candy Crush's key-value store: a black screen).
        if (arg > 0x10000 && droid_game_in(GETLK_FREE_GAMES)) *(short *)(uintptr_t)arg = 2;
        return 0;
    case 6: case 7: return 0; // F_SETLK / F_SETLKW: pretend locked
    case 1030: return fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
    }
    errno = EINVAL;
    return -1;
}

static int l_flock(int fd, int op) {
    (void)fd; (void)op;
    return 0;
}

// ---- signals: bionic's sigset_t is 8 bytes (FreeBSD's 16) ----------------------------

static int l_sigemptyset(uint64_t *s) { *s = 0; return 0; }
static int l_sigfillset(uint64_t *s) { *s = ~0ull; return 0; }
static int l_sigaddset(uint64_t *s, int sig) {
    if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    *s |= 1ull << (sig - 1);
    return 0;
}
static int l_sigdelset(uint64_t *s, int sig) {
    if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    *s &= ~(1ull << (sig - 1));
    return 0;
}
static long l_signal(int sig, long handler) {
    uint64_t old = 0;
    if (arm_sig_action(sig, (uint64_t)handler, 0, 0x10000000 /* SA_RESTART */, &old, NULL, NULL) != 0) return -1;
    return (long)old;
}
static int l_sigsuspend(const uint64_t *mask) { return arm_sig_suspend(mask ? *mask : 0); }
static int l_pthread_kill(pthread_t t, int sig) { return arm_sig_kill(t, sig); }
static int l_raise(int sig) {
    int rc = arm_sig_kill(pthread_self(), sig);
    if (rc) { errno = rc; return -1; }
    return 0;
}
static int l_pthread_sigmask(int how, const uint64_t *set, uint64_t *old) {
    return arm_sig_mask(how, set, old) == 0 ? 0 : EINVAL;
}
static int l_sigprocmask(int how, const uint64_t *set, uint64_t *old) { return arm_sig_mask(how, set, old); }

// ---- thread stacks: the ARM stack, which the GC scans ------------------------------------

#define GUEST_ATTR_MAGIC 0x4152535441434b31ull // "ARSTACK1"
typedef struct { uint64_t magic, lo, size; } guest_attr;

static int l_pthread_getattr_np(pthread_t t, guest_attr *a) {
    (void)t;
    uint64_t size = 0;
    a->magic = GUEST_ATTR_MAGIC;
    a->lo = arm_guest_stack(&size);
    a->size = size;
    return 0;
}

static int l_pthread_attr_getstack(guest_attr *a, void **addr, size_t *size) {
    if (a->magic == GUEST_ATTR_MAGIC) {
        *addr = (void *)a->lo;
        *size = a->size;
        return 0;
    }
    return pthread_attr_getstack((pthread_attr_t *)a, addr, size);
}

static int l_pthread_attr_destroy(guest_attr *a) {
    if (a->magic == GUEST_ATTR_MAGIC) {
        a->magic = 0;
        return 0;
    }
    return pthread_attr_destroy((pthread_attr_t *)a);
}

static int l_sched_getaffinity(int pid, size_t size, uint8_t *mask) {
    (void)pid;
    memset(mask, 0, size);
    if (size) mask[0] = 0xff; // 8 cores
    return 0;
}

static int l_gettid(void) { return (int)((uintptr_t)pthread_self() & 0x7fffffff); }

// ---- uname: Linux's struct utsname (6 x 65 bytes) --------------------------------------

static int l_uname(char *u) {
    memset(u, 0, 6 * 65);
    strcpy(u, "Linux");
    strcpy(u + 65, "localhost");
    strcpy(u + 130, "5.10.0-android");
    strcpy(u + 195, "#1 SMP PREEMPT");
    strcpy(u + 260, "aarch64");
    strcpy(u + 325, "localdomain");
    return 0;
}

static int l_fnmatch(const char *pat, const char *s, int flags) {
    int h = 0; // bionic: NOESCAPE 2, PATHNAME 1, PERIOD 4
    if (flags & 2) h |= FNM_NOESCAPE;
    if (flags & 1) h |= FNM_PATHNAME;
    if (flags & 4) h |= FNM_PERIOD;
    return fnmatch(pat, s, h) == 0 ? 0 : 1;
}

static char *l_basename(const char *path) {
    static __thread char buf[256];
    if (!path || !*path) return ".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t start = n;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t len = n - start < sizeof(buf) - 1 ? n - start : sizeof(buf) - 1;
    memcpy(buf, path + start, len);
    buf[len] = 0;
    return buf;
}

typedef struct { long long quot, rem; } l_lldiv_t;
static l_lldiv_t l_lldiv(long long a, long long b) {
    l_lldiv_t r = { a / b, a % b };
    return r;
}

static const char *l_inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    return inet_ntop(af == 10 ? AF_INET6 : af, src, dst, size);
}

static int l_statfs(const char *path, void *buf) {
    (void)path; (void)buf;
    errno = ENOSYS;
    return -1;
}

// bionic's struct passwd (LP64): the app's own user, home = its data folder.
// Unity reads pw_dir when HOME isn't set (the PS5 has no HOME; the PC runner does).
typedef struct {
    char *pw_name, *pw_passwd;
    uint32_t pw_uid, pw_gid;
    char *pw_gecos, *pw_dir, *pw_shell;
} l_passwd;
static l_passwd app_user = {"u0_a100", "", 10100, 10100, "", game_dir, "/system/bin/sh"};

static void *l_getpwuid(unsigned uid) {
    (void)uid;
    return &app_user;
}

static int l_getpwuid_r(unsigned uid, void *pwd, char *buf, size_t n, void **result) {
    (void)uid; (void)buf; (void)n;
    memcpy(pwd, &app_user, sizeof(app_user));
    *result = pwd;
    return 0;
}

static int l_nullptr_int(void) { return 0; }

// ---- *_l: the C locale only ---------------------------------------------------------

// (f)(c): the real libc functions, not FreeBSD's inline rune-table macros
#define LOC1(name, base, T) static T name(T c, void *l) { (void)l; return (T)(base)(c); }
LOC1(l_isdigit_l, isdigit, int)
LOC1(l_islower_l, islower, int)
LOC1(l_isupper_l, isupper, int)
LOC1(l_isxdigit_l, isxdigit, int)
LOC1(l_tolower_l, tolower, int)
LOC1(l_toupper_l, toupper, int)
LOC1(l_iswalpha_l, iswalpha, wint_t)
LOC1(l_iswblank_l, iswblank, wint_t)
LOC1(l_iswcntrl_l, iswcntrl, wint_t)
LOC1(l_iswdigit_l, iswdigit, wint_t)
LOC1(l_iswlower_l, iswlower, wint_t)
LOC1(l_iswprint_l, iswprint, wint_t)
LOC1(l_iswpunct_l, iswpunct, wint_t)
LOC1(l_iswspace_l, iswspace, wint_t)
LOC1(l_iswupper_l, iswupper, wint_t)
LOC1(l_iswxdigit_l, iswxdigit, wint_t)
LOC1(l_towlower_l, towlower, wint_t)
LOC1(l_towupper_l, towupper, wint_t)
static int l_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
static size_t l_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
static int l_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
static size_t l_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
static size_t l_strftime_l(char *s, size_t n, const char *f, const struct tm *t, void *l) {
    (void)l;
    return strftime(s, n, f, t);
}

// ---- more _chk ----------------------------------------------------------------------

static int l_fd_isset_chk(int fd, const fd_set *set, size_t size) {
    (void)size;
    return FD_ISSET(fd, set) ? 1 : 0;
}
static char *l_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static char *l_strncat_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncat(d, s, n); }
// umask: the PS5's libc has none (its address came out 0: Firebase crashed on it). Ours
// only remembers it; files get the mode they ask for.
static unsigned file_mask = 022;
static unsigned l_umask(unsigned m) {
    unsigned old = file_mask;
    file_mask = m & 0777;
    return old;
}
static unsigned l_umask_chk(unsigned m) { return l_umask(m); }

// ---- bionic's _ctype_ table (BSD bits), indexed [1 + c] ---------------------------------

static char bionic_ctype[1 + 256];

static void build_ctype(void) {
    for (int c = 0; c < 256; c++) {
        char b = 0;
        if (c < 128) {
            if ((isupper)(c)) b |= 0x01;
            if ((islower)(c)) b |= 0x02;
            if ((isdigit)(c)) b |= 0x04;
            if ((isspace)(c)) b |= 0x08;
            if ((ispunct)(c)) b |= 0x10;
            if ((iscntrl)(c)) b |= 0x20;
            if ((isxdigit)(c) && !(isdigit)(c)) b |= 0x40;
            if (c == ' ') b |= (char)0x80;
        }
        bionic_ctype[1 + c] = b;
    }
}

// ---- Android sensors: sensors.c (the DualSense's motion) ------------------------------
extern const struct droid_sym { const char *name; void *fn; } droid_sensor_syms[];

void *activity_window_ptr(void);
void *mediandk_video_window(void *surface); // mediandk.c: a SurfaceTexture's Surface (video games), or NULL
static void *l_window_from_surface(void *env, void *surface) {
    (void)env;
    void *video = mediandk_video_window(surface);
    return video ? video : activity_window_ptr();
}

static int l_pagesize(void) { return droid_page_size(); }

#define L(name, fn) { name, (void *)fn }
static const struct { const char *name; void *fn; } table[] = {
    L("stat", l_stat), L("lstat", l_lstat), L("fstat", l_fstat), L("open", l_open), L("dup", l_dup),
    L("__open_2", l_open_2), L("readdir", l_readdir), L("opendir", l_opendir),
    L("closedir", closedir), L("alphasort", l_alphasort), L("alphasort64", l_alphasort), L("sysconf", l_sysconf),
    L("mbrlen", l_mbrlen), L("mbrtowc", l_mbrtowc), L("mbsnrtowcs", l_mbsnrtowcs),
    L("mbsrtowcs", l_mbsrtowcs), L("wcrtomb", l_wcrtomb), L("wcsnrtombs", l_wcsnrtombs),
    L("__ctype_get_mb_cur_max", l_ctype_get_mb_cur_max),
    L("strtold", l_strtold), L("strtold_l", l_strtold_l), L("wcstold", l_wcstold),
    L("newlocale", l_newlocale), L("uselocale", l_uselocale), L("freelocale", l_freelocale),
    L("setlocale", l_setlocale), L("strtoll_l", l_strtoll_l), L("strtoull_l", l_strtoull_l),
    L("__memmove_chk", l_memmove_chk), L("__read_chk", l_read_chk),
    L("__strlen_chk", l_strlen_chk), L("__vsnprintf_chk", l_vsnprintf_chk),
    L("__FD_SET_chk", l_fd_set_chk), L("setjmp", l_setjmp), L("longjmp", l_longjmp),
    L("sigaction", l_sigaction), L("raise", l_raise), L("kill", l_fail), L("fork", l_fail),
    L("execvp", l_fail), L("waitpid", l_fail), L("setsid", l_fail), L("popen", l_null),
    L("pclose", l_fail), L("socket", l_fail), L("accept", l_fail), L("bind", l_fail),
    L("connect", l_fail), L("listen", l_fail), L("recv", l_fail), L("recvfrom", l_fail),
    L("send", l_fail), L("sendto", l_fail), L("setsockopt", l_fail), L("getsockopt", l_fail),
    L("getsockname", l_fail), L("ioctl", l_fail), L("getaddrinfo", l_eai_fail),
    L("getnameinfo", l_eai_fail), L("freeaddrinfo", l_nothing),
    L("gai_strerror", l_gai_strerror), L("inet_pton", l_zero), L("if_nametoindex", l_zero),
    L("if_indextoname", l_null), L("pthread_setname_np", l_pthread_setname_np),
    L("isatty", l_zero), L("readlink", l_readlink), L("getcwd", l_getcwd), L("chdir", l_chdir),
    L("link", l_no_links), L("symlink", l_no_links), L("linkat", l_no_links), L("symlinkat", l_no_links),
    L("mkdir", l_mkdir), L("rmdir", l_rmdir), L("unlink", l_unlink), L("remove", l_remove), L("access", l_access),
    L("chmod", l_chmod), L("truncate", l_truncate), L("rename", l_rename), L("realpath", l_realpath),
    // Unity / IL2CPP
    L("clock", l_clock), L("clock_getres", l_clock_getres),
    L("pthread_condattr_setclock", l_condattr_setclock), L("mmap", l_mmap), L("munmap", l_munmap),
    L("pthread_mutexattr_init", l_mutexattr_init), L("pthread_mutexattr_destroy", l_mutexattr_destroy),
    L("pthread_mutexattr_settype", l_mutexattr_settype), L("pthread_mutexattr_gettype", l_mutexattr_gettype),
    L("pthread_mutexattr_setpshared", l_mutexattr_setpshared), L("pthread_mutexattr_getpshared", l_mutexattr_getpshared),
    L("pthread_mutexattr_setprotocol", l_mutexattr_noop), L("pthread_mutexattr_setrobust", l_mutexattr_noop),
    L("pthread_mutex_init", l_mutex_init), L("pthread_condattr_init", l_condattr_init),
    L("pthread_condattr_destroy", l_condattr_destroy), L("pthread_condattr_getclock", l_condattr_getclock),
    L("pthread_condattr_setpshared", l_condattr_setpshared), L("pthread_condattr_getpshared", l_condattr_getpshared),
    L("pthread_cond_init", l_cond_init),
    L("mprotect", l_mprotect), L("madvise", l_madvise), L("memalign", l_memalign),
    L("fcntl", l_fcntl), L("flock", l_flock), L("sigemptyset", l_sigemptyset),
    L("sigfillset", l_sigfillset), L("sigaddset", l_sigaddset), L("sigdelset", l_sigdelset),
    L("signal", l_signal), L("sigsuspend", l_sigsuspend), L("sigaltstack", l_zero),
    L("pthread_sigmask", l_pthread_sigmask), L("sigprocmask", l_sigprocmask), L("pthread_kill", l_pthread_kill), L("pthread_atfork", l_zero),
    L("pthread_getattr_np", l_pthread_getattr_np), L("pthread_attr_getstack", l_pthread_attr_getstack),
    L("pthread_attr_destroy", l_pthread_attr_destroy), L("sched_getaffinity", l_sched_getaffinity),
    L("sched_setaffinity", l_zero), L("gettid", l_gettid), L("uname", l_uname),
    L("fnmatch", l_fnmatch), L("basename", l_basename), L("lldiv", l_lldiv),
    L("inet_ntop", l_inet_ntop), L("statfs", l_statfs), L("getpwuid", l_getpwuid), L("getpwnam", l_getpwuid),
    L("getpwuid_r", l_getpwuid_r), L("gethostbyname", l_null), L("gethostbyaddr", l_null),
    L("getpeername", l_fail), L("recvmsg", l_fail), L("sendmsg", l_fail), L("sendfile", l_sendfile),
    L("shutdown", l_fail), L("prctl", l_prctl), L("ptrace", l_fail),
    L("getpriority", l_zero), L("setpriority", l_zero),
    L("getpagesize", l_pagesize), L("lseek64", l_lseek), L("lseek", l_lseek), L("write", l_write), L("pwrite", l_pwrite),
    L("pwrite64", l_pwrite), L("writev", l_writev), L("fsync", l_fsync), L("fdatasync", l_fdatasync),
    L("ftruncate", l_ftruncate), L("ftruncate64", l_ftruncate),
    L("isdigit_l", l_isdigit_l), L("islower_l", l_islower_l), L("isupper_l", l_isupper_l),
    L("isxdigit_l", l_isxdigit_l), L("tolower_l", l_tolower_l), L("toupper_l", l_toupper_l),
    L("iswalpha_l", l_iswalpha_l), L("iswblank_l", l_iswblank_l), L("iswcntrl_l", l_iswcntrl_l),
    L("iswdigit_l", l_iswdigit_l), L("iswlower_l", l_iswlower_l), L("iswprint_l", l_iswprint_l),
    L("iswpunct_l", l_iswpunct_l), L("iswspace_l", l_iswspace_l), L("iswupper_l", l_iswupper_l),
    L("iswxdigit_l", l_iswxdigit_l), L("towlower_l", l_towlower_l), L("towupper_l", l_towupper_l),
    L("strcoll_l", l_strcoll_l), L("strxfrm_l", l_strxfrm_l), L("wcscoll_l", l_wcscoll_l),
    L("wcsxfrm_l", l_wcsxfrm_l), L("strftime_l", l_strftime_l),
    L("__FD_ISSET_chk", l_fd_isset_chk), L("__strchr_chk", l_strchr_chk),
    L("__strncat_chk", l_strncat_chk), L("__umask_chk", l_umask_chk), L("umask", l_umask),
    L("__system_property_find", l_null), L("__system_property_read", l_nullptr_int),
    L("_ctype_", bionic_ctype), L("inflate", unity_inflate), L("inflateEnd", inflateEnd),
    L("inflateInit2_", droid_inflateInit2_), L("inflateInit_", droid_inflateInit_), L("deflateInit_", droid_deflateInit_),
    L("deflateInit2_", droid_deflateInit2_), L("ANativeWindow_fromSurface", l_window_from_surface),
    L("compressBound", compressBound), L("compress2", compress2), L("adler32", adler32),
    L("inflateReset", inflateReset), L("inflateReset2", inflateReset2), L("deflateReset", deflateReset),
    L("get_crc_table", get_crc_table),
};

void *net_resolve(const char *name); // net_shim.c: sockets, getaddrinfo & co

void *linux_abi_resolve(const char *name) {
    void *net = net_resolve(name);
    if (net) return net;
    static int ctype_ready;
    if (!ctype_ready) {
        build_ctype();
        ctype_ready = 1;
    }
    static int checked;
    if (!checked) { // a function the console's libc lacks comes out as address 0: say so once
        checked = 1;
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (!table[i].fn) droid_log("linux_abi: %s has no code on this system (address 0)", table[i].name);
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    if (!strncmp(name, "ASensor", 7))
        for (int i = 0; droid_sensor_syms[i].name; i++)
            if (!strcmp(droid_sensor_syms[i].name, name)) return droid_sensor_syms[i].fn;
    void *libc_extra_resolve(const char *name); // libc_extra.c: the calls the APK scanner found missing
    return libc_extra_resolve(name);
}
