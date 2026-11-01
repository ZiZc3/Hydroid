// libc_extra.c — the calls the APK scanner (tools/apk_scan.py, scan/missing.md) found games
// importing that we had no answer for: until now each was a stub returning 0, which "worked"
// for a call nobody makes and lied for one that is made (process_vm_readv "read" nothing and
// succeeded, sysinfo left its struct empty: 0 bytes of memory). Each one here answers as Linux
// does on a phone, or fails the way Linux fails (-1 and an errno) when a title can't do it
// (processes, epoll, timers). Asked by linux_abi_resolve after its own table.

#include <ctype.h>
#include <regex.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
int droid_inflateInit_(z_streamp s, const char *v, int n); // linux_abi.c: z_streams with ARM allocators
int droid_deflateInit_(z_streamp s, int lvl, const char *v, int n);
int droid_deflateInit2_(z_streamp s, int lvl, int m, int wb, int mem, int st, const char *v, int n);

#include "game_settings.h"

void droid_log(const char *fmt, ...);
void *linux_abi_resolve(const char *name);
int arm_vsnprintf_entry(char *buf, size_t n, const char *fmt, const void *ap); // arm_imports.c
int __android_log_write(int prio, const char *tag, const char *msg);

// our own path calls (linux_abi.c: the game's paths made ours, the big files followed)
static void *ours(const char *name) {
    void *p = linux_abi_resolve(name);
    if (!p) droid_log("libc_extra: no %s to call", name);
    return p;
}

// ---- the _chk family (FORTIFY): the plain call, its size check left out --------------------
static void *x_memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
static void *x_memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }
static char *x_strcat_chk(char *d, const char *s, size_t dn) { (void)dn; return strcat(d, s); }
static char *x_strcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return strcpy(d, s); }
static char *x_strncpy_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncpy(d, s, n); }
static char *x_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn) {
    (void)dn; (void)sn;
    return strncpy(d, s, n);
}
static char *x_strrchr_chk(const char *s, int c, size_t sn) { (void)sn; return strrchr(s, c); }
static void *x_memchr_chk(const void *s, int c, size_t n, size_t bn) { (void)bn; return memchr(s, c, n); }
// sprintf writes all it makes: the compiler's idea of the buffer's size (slen) is only for a check
// after the fact, never a limit. Used as one, GTA San Andreas's "~%c~" into a 3-byte array came
// out "~w", the lost '~' ran its text parser off the stack (its first mission hint, a crash)
static int x_vsprintf_chk(char *buf, int flags, size_t slen, const char *fmt, const void *ap) {
    (void)flags; (void)slen;
    int r = arm_vsnprintf_entry(buf, (size_t)-1 >> 1, fmt, ap);
    static int told; // DROID_SPRINTF_LOG=1: what the first ones made (a game's text from them)
    static int on = -1;
    if (on < 0) on = getenv("DROID_SPRINTF_LOG") != NULL;
    if (on && told < 200 && (strchr(fmt, '~') || (r > 0 && strchr(buf, '~')))) {
        told++;
        droid_log("sprintf_chk: \"%.60s\" -> \"%.120s\" (%d)", fmt, buf, r);
    }
    return r;
}
static ssize_t x_pread(int fd, void *buf, size_t n, int64_t off) { return pread(fd, buf, n, (off_t)off); }
static ssize_t x_pread_chk(int fd, void *buf, size_t n, int64_t off, size_t bn) {
    (void)bn;
    return pread(fd, buf, n, (off_t)off);
}
static ssize_t x_readlink_chk(const char *p, char *buf, size_t n, size_t bn) {
    (void)bn;
    ssize_t (*rl)(const char *, char *, size_t) = ours("readlink");
    return rl ? rl(p, buf, n) : (errno = EINVAL, -1);
}
static void x_fd_clr_chk(int fd, fd_set *set, size_t size) {
    (void)size;
    if (fd >= 0 && fd < FD_SETSIZE) FD_CLR(fd, set);
}

// ---- processes, signals: a title has one process --------------------------------------------
static void x_exit(int code) { _exit(code); }
static int x_fail_nosys(void) {
    errno = ENOSYS;
    return -1;
}
// epoll / eventfd: not here either, but said once each (a game's .NET sockets wait on them: Dragon
// Ball Legends connected and never sent a byte, 2026-10-29)
#define NOSYS_TOLD(fn)                                                                    \
    static int x_##fn(void) {                                                             \
        static int told;                                                                  \
        if (!told++) droid_log("libc: " #fn " asked: not on this system (ENOSYS)");       \
        errno = ENOSYS;                                                                   \
        return -1;                                                                        \
    }
NOSYS_TOLD(epoll_create)
NOSYS_TOLD(epoll_create1)
NOSYS_TOLD(epoll_ctl)
NOSYS_TOLD(epoll_wait)
NOSYS_TOLD(eventfd)
static int x_zero(void) { return 0; }
static void *x_null(void) { return NULL; }
static int x_getppid(void) { return 1; }
static int x_getgid(void) { return 10100; } // the app's own user (linux_abi.c: getpwuid)
static int x_wait(int *st) {
    (void)st;
    errno = ECHILD;
    return -1;
}
static int x_sigrtmin(void) { return 34; }
static const char *x_strsignal(int sig) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "Signal %d", sig);
    return buf;
}
static int x_sigwait(const void *set, int *sig) {
    (void)set; (void)sig;
    return EINVAL;
}

// process_vm_readv: another process's memory - none here (it "succeeded" with 0 bytes)
static long x_process_vm_readv(void) {
    errno = ENOSYS;
    return -1;
}

// ---- memory, limits, the machine -----------------------------------------------------------
// Linux's struct sysinfo (LP64): what a phone with 12 GB says (the console's games get ~12.5)
typedef struct {
    long uptime;
    unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
} linux_sysinfo;
static int x_sysinfo(linux_sysinfo *s) {
    memset(s, 0, sizeof(*s));
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    s->uptime = t.tv_sec;
    s->totalram = 12ull << 30;
    s->freeram = 6ull << 30;
    s->bufferram = 256ull << 20;
    s->procs = 1;
    s->mem_unit = 1;
    return 0;
}
// rlimits: no limit, the main thread's stack 8 MB (RLIMIT_STACK 3), 32768 files (NOFILE 7)
typedef struct { uint64_t cur, max; } linux_rlimit;
static int x_getrlimit(int res, linux_rlimit *r) {
    r->cur = r->max = ~0ull;
    if (res == 3) r->cur = 8u << 20;
    if (res == 7) r->cur = r->max = 32768;
    return 0;
}
static int x_setrlimit(int res, const linux_rlimit *r) {
    (void)res; (void)r;
    return 0;
}
// Linux's struct rusage: two timevals and 14 longs - all 0 but the CPU time
static int x_getrusage(int who, void *ru) {
    (void)who;
    memset(ru, 0, 144);
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    int64_t *v = ru;
    v[0] = t.tv_sec;
    v[1] = t.tv_nsec / 1000;
    return 0;
}
static long x_times(int64_t *tms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (tms) memset(tms, 0, 32);
    return (long)(t.tv_sec * 100 + t.tv_nsec / 10000000); // clock ticks (100 Hz)
}
static int x_msync(void *a, size_t n, int f) {
    (void)a; (void)n; (void)f;
    return 0;
}
static void *x_mremap(void) {
    errno = ENOMEM; // never moved: callers fall back to a new map and a copy
    return MAP_FAILED;
}
static int x_cpucount(size_t size, const unsigned long *set) {
    int n = 0;
    for (size_t i = 0; i < size / sizeof(unsigned long); i++) n += __builtin_popcountl(set[i]);
    return n;
}
static uintptr_t x_stack_chk_guard = 0x2f8a9c71d3e45b06ull;

// ---- scheduling: one policy (SCHED_OTHER 0), priority 0 ----------------------------------------
static int x_sched_getparam(int pid, int *param) {
    (void)pid;
    *param = 0;
    return 0;
}
static int x_pthread_getschedparam(pthread_t t, int *policy, int *param) {
    (void)t;
    if (policy) *policy = 0;
    if (param) *param = 0;
    return 0;
}
static int x_pthread_setschedparam(pthread_t t, int policy, const int *param) {
    (void)t; (void)policy; (void)param;
    return 0;
}
static int x_attr_getguardsize(const void *a, size_t *size) {
    (void)a;
    *size = 4096;
    return 0;
}

// ---- pipes, events: pipe2 by pipe; epoll / eventfd / timerfd / inotify not in a title ------------
static int x_pipe2(int fds[2], int flags) {
    void *droid_resolve(const char *name); // android_shim.c: bionic's pipe (ours)
    int (*p)(int *) = (int (*)(int *))droid_resolve("pipe");
    int r = p ? p(fds) : pipe(fds);
    if (r == 0 && (flags & 04000)) { // O_NONBLOCK (Linux)
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
        fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    }
    return r;
}

// inotify: a descriptor that never reports a change (the read end of a pipe whose write end stays
// open). Firebase Cloud Messaging's C++ SDK watches its storage file on a thread of its own and
// aborts on "file_descriptor >= 0" (Jetpack Joyride Classic); nothing changes that file here.
// (Halfbrick's games only, where it was needed; every other game: "not supported", as before)
#define INOTIFY_GAMES "com.halfbrick."
static int x_inotify_init1(int flags) {
    if (!droid_game_in(INOTIFY_GAMES)) return x_fail_nosys();
    int fds[2];
    if (x_pipe2(fds, flags & 04000) != 0) return -1; // IN_NONBLOCK = O_NONBLOCK
    return fds[0]; // fds[1] kept open: a read waits, as for a file nobody touches
}
static int x_inotify_init(void) { return x_inotify_init1(0); }
static int x_inotify_add_watch(int fd, const char *path, uint32_t mask) {
    (void)fd; (void)path; (void)mask;
    if (!droid_game_in(INOTIFY_GAMES)) return x_fail_nosys();
    static int next = 1;
    return next++;
}
static int x_inotify_rm_watch(int fd, int wd) {
    (void)fd; (void)wd;
    return droid_game_in(INOTIFY_GAMES) ? 0 : x_fail_nosys();
}

// ---- paths relative to a folder (the *at calls): AT_FDCWD (-100) or an absolute path only ------
#define AT_CWD (-100)
static int at_ok(int dirfd, const char *p) {
    if (dirfd == AT_CWD || (p && p[0] == '/')) return 1;
    errno = ENOSYS;
    return 0;
}
static int x_openat(int dirfd, const char *p, int flags, int mode) {
    if (!at_ok(dirfd, p)) return -1;
    int (*o)(const char *, int, int) = ours("open");
    return o ? o(p, flags, mode) : -1;
}
static int x_openat_2(int dirfd, const char *p, int flags) { return x_openat(dirfd, p, flags, 0); }
static int x_fstatat(int dirfd, const char *p, void *st, int flags) {
    if (!at_ok(dirfd, p)) return -1;
    int (*s)(const char *, void *) = ours(flags & 0x100 ? "lstat" : "stat"); // AT_SYMLINK_NOFOLLOW
    return s ? s(p, st) : -1;
}
static int x_unlinkat(int dirfd, const char *p, int flags) {
    if (!at_ok(dirfd, p)) return -1;
    int (*u)(const char *) = ours(flags & 0x200 ? "rmdir" : "unlink"); // AT_REMOVEDIR
    return u ? u(p) : -1;
}
static int x_mkdirat(int dirfd, const char *p, int mode) {
    if (!at_ok(dirfd, p)) return -1;
    int (*m)(const char *, int) = ours("mkdir");
    return m ? m(p, mode) : -1;
}
static int x_renameat(int da, const char *a, int db, const char *b) {
    if (!at_ok(da, a) || !at_ok(db, b)) return -1;
    int (*r)(const char *, const char *) = ours("rename");
    return r ? r(a, b) : -1;
}
static int x_faccessat(int dirfd, const char *p, int m, int flags) {
    (void)flags;
    if (!at_ok(dirfd, p)) return -1;
    int (*a)(const char *, int) = ours("access");
    return a ? a(p, m) : -1;
}
static int x_fchmodat(int dirfd, const char *p, int m, int flags) {
    (void)flags;
    if (!at_ok(dirfd, p)) return -1;
    int (*c)(const char *, int) = ours("chmod");
    return c ? c(p, m) : -1;
}
static ssize_t x_readlinkat(int dirfd, const char *p, char *buf, size_t n) {
    if (!at_ok(dirfd, p)) return -1;
    ssize_t (*rl)(const char *, char *, size_t) = ours("readlink");
    return rl ? rl(p, buf, n) : -1;
}
static int x_utimensat(void) { return 0; } // times not kept
static long x_pathconf(const char *p, int name) {
    (void)p;
    return name == 3 ? 255 : name == 4 ? 4096 : 4096; // _PC_NAME_MAX 3, _PC_PATH_MAX 4
}
// statvfs: Linux's layout (LP64): 128 GB, 64 GB free (a download checks there is room)
typedef struct {
    unsigned long bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, flag, namemax;
    int spare[6];
} linux_statvfs;
static int x_statvfs(const char *p, linux_statvfs *out) {
    (void)p;
    memset(out, 0, sizeof(*out));
    out->bsize = out->frsize = 4096; // (the console's own numbers are not ours to ask in a title)
    out->blocks = (128ull << 30) / 4096;
    out->bfree = out->bavail = (64ull << 30) / 4096;
    out->files = out->ffree = out->favail = 1u << 20;
    out->namemax = 255;
    return 0;
}

// ---- time ---------------------------------------------------------------------------------------
static int x_daylight;
static long x_timezone;
static const char *x_tzname[2] = { "UTC", "UTC" };
static void x_tzset(void) {}
static int x_clock_nanosleep(int clk, int flags, const struct timespec *req, struct timespec *rem) {
    if (flags & 1) { // TIMER_ABSTIME
        struct timespec now;
        clock_gettime(clk == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
        int64_t ns = (req->tv_sec - now.tv_sec) * 1000000000ll + (req->tv_nsec - now.tv_nsec);
        if (ns <= 0) return 0;
        struct timespec d = { (time_t)(ns / 1000000000ll), (long)(ns % 1000000000ll) };
        return nanosleep(&d, NULL) == 0 ? 0 : errno;
    }
    return nanosleep(req, rem) == 0 ? 0 : errno;
}

// timegm / asctime_r: not in the console's libc
static time_t x_timegm(struct tm *t) {
    long long y = t->tm_year + 1900LL, m = t->tm_mon + 1;
    y += (m - 1) / 12; // a month out of range carries into the year
    m = (m - 1) % 12 + 1;
    if (m <= 0) { m += 12; y--; }
    const long long era_y = m <= 2 ? y - 1 : y; // days from 1970-01-01 (Howard Hinnant's days_from_civil)
    const long long era = (era_y >= 0 ? era_y : era_y - 399) / 400;
    const long long yoe = era_y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + t->tm_hour * 3600LL + t->tm_min * 60LL + t->tm_sec);
}
static char *x_asctime_r(const struct tm *t, char *buf) {
    static const char wd[][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char mo[][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", wd[(unsigned)t->tm_wday % 7], mo[(unsigned)t->tm_mon % 12],
             t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, 1900 + t->tm_year);
    return buf;
}

static char *x_ctime(const time_t *t) {
    static char buf[32];
    struct tm tm;
    localtime_r(t, &tm);
    return x_asctime_r(&tm, buf);
}

// ---- GTA San Andreas (2.11): users, signals, scheduling, bitmaps ------------------------------
static int x_sigismember(const uint64_t *set, int sig) { return sig >= 1 && sig <= 64 ? (int)(*set >> (sig - 1) & 1) : 0; }
static int x_attr_getschedparam(const void *a, int *param) {
    (void)a;
    *param = 0;
    return 0;
}
static int x_attr_setschedparam(void *a, const int *param) {
    (void)a; (void)param;
    return 0;
}
static int x_res_search(void) { return -1; } // DNS through getaddrinfo only (net_shim.c)
// AndroidBitmap / ANativeWindow_lock (drawing into Java bitmaps and windows by the CPU): not yet -
// "bad parameter" (-1), which callers treat as "no bitmap", logged once
static int x_bitmap_fail(void) {
    static int told;
    if (!told++) droid_log("libc_extra: AndroidBitmap / ANativeWindow_lock asked for (not done yet)");
    return -1;
}

// ---- strings, numbers ------------------------------------------------------------------------
static long long x_atoll(const char *s) { return strtoll(s, NULL, 10); }
// small math / ctype the old NDK builds call by name (stubbed they answered 0)
static int x_abs(int v) { return v < 0 ? -v : v; }
static long x_labs(long v) { return v < 0 ? -v : v; }
static long long x_llabs(long long v) { return v < 0 ? -v : v; }
static double x_fmax(double a, double b) { return fmax(a, b); }
static double x_fmin(double a, double b) { return fmin(a, b); }
static float x_fmaxf(float a, float b) { return fmaxf(a, b); }
static float x_fminf(float a, float b) { return fminf(a, b); }
static int x_isascii(int c) { return (unsigned)c < 128; }
static int x_isgraph(int c) { return c > 32 && c < 127; }
// bionic's float classifiers (old NDK builds call them by name: Geometry Dash)
// (compiler builtins: the console's libc would want __isinf / __fpclassifyd imports for the macros)
static int x_isnan(double v) { return __builtin_isnan(v); }
static int x_isnanf(float v) { return __builtin_isnan(v); }
static int x_isinf(double v) { return __builtin_isinf_sign(v); }
static int x_isinff(float v) { return __builtin_isinf_sign(v); }
static int x_isfinite(double v) { return __builtin_isfinite(v); }
static int x_isfinitef(float v) { return __builtin_isfinite(v); }
static int x_signbit(double v) { return __builtin_signbit(v) != 0; }
static int x_signbitf(float v) { return __builtin_signbitf(v) != 0; }
// bionic's FP_* bits: INFINITE 1, NAN 2, NORMAL 4, SUBNORMAL 8, ZERO 16 (not the host's numbers)
static int bionic_fp(int c) {
    return c == FP_INFINITE ? 1 : c == FP_NAN ? 2 : c == FP_NORMAL ? 4 : c == FP_SUBNORMAL ? 8 : 16;
}
static int x_fpclassify(double v) {
    return bionic_fp(__builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, v));
}
static int x_fpclassifyf(float v) {
    return bionic_fp(__builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, v));
}
static char *x_gnu_strerror_r(int e, char *buf, size_t n) {
    snprintf(buf, n, "%s", strerror(e));
    return buf;
}
static int x_android_log_buf_write(int buf, int prio, const char *tag, const char *msg) {
    (void)buf;
    return __android_log_write(prio, tag, msg);
}

// ---- the 2026-10-26 batch: 19 more games scanned (scan/missing.md), what most of them import ----
// (Unity's Alto's Odyssey / Fruit Ninja / Terraria / Crossy Road / Temple Run 2 / Cut the Rope,
// INSIDE, Candy Crush, Jetpack Joyride, Minecraft, MK X, Hearthstone, Brotato ...)
void *droid_resolve(const char *name);
static void *bionic(const char *name) { // the game's FILE / mutex calls (bionic_shim.c), or ours
    void *p = droid_resolve(name);
    return p ? p : ours(name);
}
static const char *const x_sys_signame[65] = {
    "0", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE",
    "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ",
    "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
};
// command-line options: a game has none
static char *x_optarg;
static int x_optind = 1, x_opterr = 1, x_optopt;
static int x_getopt(void) { return -1; }
// an executable's start (a helper program shipped as lib*.so): never ours to run
static void x_libc_init(void) { droid_log("libc_extra: __libc_init called (a program's start, not run)"); }
// mallinfo returns its struct through x8 (arm_cpu.cpp passes it as the first argument): 10 counts
static void *x_mallinfo(size_t *out) {
    memset(out, 0, 10 * sizeof(size_t));
    out[0] = 256u << 20; // arena: what's taken from the system
    out[7] = 128u << 20; // uordblks: in use
    out[8] = 128u << 20; // fordblks: free
    return out;
}
static unsigned x_weof(void) { return 0xffffffffu; } // wide-char FILE calls: none (WEOF)
static void x_arc4random_buf(void *buf, size_t n) {
    uint8_t *b = buf;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t r = arc4random();
        memcpy(b + i, &r, n - i < 4 ? n - i : 4);
    }
}
static uint32_t x_arc4random_uniform(uint32_t n) {
    if (n < 2) return 0;
    uint32_t min = -n % n, r; // no modulo bias
    do r = arc4random(); while (r < min);
    return r % n;
}
static void *x_null_enosys(void) {
    errno = ENOSYS;
    return NULL;
}
static int x_fail_notty(void) {
    errno = ENOTTY;
    return -1;
}
static int x_fail_notsup(void) {
    errno = EOPNOTSUPP;
    return -1;
}
static double x_fabs(double v) { return __builtin_fabs(v); }
static double x_log1p(double v) { return log1p(v); }
static float x_frexpf(float v, int *e) { return frexpf(v, e); }
static float x_expm1f(float v) { return expm1f(v); }
static double x_erf(double v) { return erf(v); }
static float x_erfcf(float v) { return erfcf(v); }
static float x_tgammaf(float v) { return tgammaf(v); }
static float x_nanf(const char *s) { (void)s; return __builtin_nanf(""); }
typedef struct { long quot, rem; } x_ldiv_t; // x0 / x1 (rax / rdx)
static x_ldiv_t x_ldiv(long a, long b) {
    x_ldiv_t r = { a / b, a % b };
    return r;
}
// long double: IEEE binary128 in v0 on ARM64; done in double (what a game asks of it)
typedef float x_v4f __attribute__((vector_size(16)));
static double quad_to_double(x_v4f q) {
    uint64_t lo, hi;
    memcpy(&lo, &q, 8);
    memcpy(&hi, (char *)&q + 8, 8);
    uint64_t sign = hi >> 63, exp = (hi >> 48) & 0x7fff, man = (hi & ((1ull << 48) - 1)) << 4 | lo >> 60;
    uint64_t b;
    if (exp == 0) b = 0; // zero (subnormal quads: 0 in double too)
    else if (exp == 0x7fff) b = 0x7ffull << 52 | man;
    else {
        long e = (long)exp - 16383 + 1023;
        if (e >= 0x7ff) b = 0x7ffull << 52;
        else if (e <= 0) b = 0;
        else b = (uint64_t)e << 52 | man;
    }
    b |= sign << 63;
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static x_v4f double_to_quad(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    uint64_t sign = b >> 63, exp = (b >> 52) & 0x7ff, man = b & ((1ull << 52) - 1), hi, lo;
    if (exp == 0) hi = lo = 0;
    else if (exp == 0x7ff) {
        hi = 0x7fffull << 48 | man >> 4;
        lo = man << 60;
    } else {
        hi = (exp - 1023 + 16383) << 48 | man >> 4;
        lo = man << 60;
    }
    hi |= sign << 63;
    x_v4f q;
    memcpy(&q, &lo, 8);
    memcpy((char *)&q + 8, &hi, 8);
    return q;
}
static x_v4f x_floorl(x_v4f q) { return double_to_quad(floor(quad_to_double(q))); }
static x_v4f x_frexpl(x_v4f q, int *e) { return double_to_quad(frexp(quad_to_double(q), e)); }
static x_v4f x_ldexpl(x_v4f q, int e) { return double_to_quad(ldexp(quad_to_double(q), e)); }
// strptime: not in the console's libc - the fields games parse (dates in HTTP headers, saves)
static int x_num(const char **s, int lo, int hi, int width, int *out) {
    int v = 0, n = 0;
    while (n < width && **s >= '0' && **s <= '9') v = v * 10 + (*(*s)++ - '0'), n++;
    if (!n || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}
static int x_name(const char **s, const char *const *names, int count, int *out) {
    for (int i = 0; i < count; i++) {
        size_t full = strlen(names[i]);
        if (!strncasecmp(*s, names[i], full)) { *s += full; *out = i; return 1; }
        if (!strncasecmp(*s, names[i], 3)) { *s += 3; *out = i; return 1; }
    }
    return 0;
}
static char *x_strptime(const char *s, const char *f, struct tm *t) {
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
    int v;
    for (; *f; f++) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*f != '%') {
            if (*s++ != *f) return NULL;
            continue;
        }
        f++;
        if (*f == 'E' || *f == 'O') f++;
        switch (*f) {
        case 'Y': if (!x_num(&s, 0, 9999, 4, &v)) return NULL; t->tm_year = v - 1900; break;
        case 'y': if (!x_num(&s, 0, 99, 2, &v)) return NULL; t->tm_year = v < 69 ? v + 100 : v; break;
        case 'm': if (!x_num(&s, 1, 12, 2, &v)) return NULL; t->tm_mon = v - 1; break;
        case 'd': case 'e':
            while (*s == ' ') s++;
            if (!x_num(&s, 1, 31, 2, &v)) return NULL;
            t->tm_mday = v;
            break;
        case 'H': if (!x_num(&s, 0, 23, 2, &v)) return NULL; t->tm_hour = v; break;
        case 'M': if (!x_num(&s, 0, 59, 2, &v)) return NULL; t->tm_min = v; break;
        case 'S': if (!x_num(&s, 0, 61, 2, &v)) return NULL; t->tm_sec = v; break;
        case 'j': if (!x_num(&s, 1, 366, 3, &v)) return NULL; t->tm_yday = v - 1; break;
        case 'a': case 'A': if (!x_name(&s, days, 7, &v)) return NULL; t->tm_wday = v; break;
        case 'b': case 'B': case 'h': if (!x_name(&s, months, 12, &v)) return NULL; t->tm_mon = v; break;
        case 'T': if (!(s = x_strptime(s, "%H:%M:%S", t))) return NULL; break;
        case 'D': if (!(s = x_strptime(s, "%m/%d/%y", t))) return NULL; break;
        case 'F': if (!(s = x_strptime(s, "%Y-%m-%d", t))) return NULL; break;
        case 'R': if (!(s = x_strptime(s, "%H:%M", t))) return NULL; break;
        case 'n': case 't': while (isspace((unsigned char)*s)) s++; break;
        case 'Z': while (isalpha((unsigned char)*s)) s++; break; // a zone name: skipped
        case 'z':
            if (*s == 'Z') { s++; break; }
            if (*s != '+' && *s != '-') return NULL;
            s++;
            for (int i = 0; i < 4 && *s; i++) {
                if (*s == ':') s++;
                if (*s < '0' || *s > '9') return NULL;
                s++;
            }
            break;
        case '%': if (*s++ != '%') return NULL; break;
        default: return NULL;
        }
    }
    return (char *)s;
}
static size_t x_strlcpy_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strlcpy(d, s, n); }
static size_t x_strlcat_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strlcat(d, s, n); }
static size_t x_strlcat(char *d, const char *s, size_t n) { return strlcat(d, s, n); }
static ssize_t x_write_chk(int fd, const void *b, size_t n, size_t bn) {
    (void)bn;
    ssize_t (*w)(int, const void *, size_t) = ours("write");
    return w ? w(fd, b, n) : -1;
}
static ssize_t x_pwrite_chk(int fd, const void *b, size_t n, int64_t off, size_t bn) {
    (void)bn;
    ssize_t (*w)(int, const void *, size_t, int64_t) = ours("pwrite");
    return w ? w(fd, b, n, off) : -1;
}
static size_t x_fread_chk(void *b, size_t bn, size_t sz, size_t n, void *f) {
    (void)bn;
    size_t (*r)(void *, size_t, size_t, void *) = bionic("fread");
    return r ? r(b, sz, n, f) : 0;
}
static size_t x_fwrite_chk(const void *b, size_t sz, size_t n, void *f, size_t bn) {
    (void)bn;
    size_t (*w)(const void *, size_t, size_t, void *) = bionic("fwrite");
    return w ? w(b, sz, n, f) : 0;
}
static char *x_fgets_chk(char *b, int n, void *f, size_t bn) {
    (void)bn;
    char *(*g)(char *, int, void *) = bionic("fgets");
    return g ? g(b, n, f) : NULL;
}
static ssize_t x_sendto_chk(int fd, const void *b, size_t n, size_t bn, int flags, const void *to, unsigned tl) {
    (void)bn;
    ssize_t (*s)(int, const void *, size_t, int, const void *, unsigned) = ours("sendto");
    return s ? s(fd, b, n, flags, to, tl) : -1;
}
static int x_fgetpos(void *f, int64_t *pos) {
    int64_t (*t)(void *) = bionic("ftello");
    if (!t) t = bionic("ftell");
    *pos = t ? t(f) : -1;
    return *pos < 0 ? -1 : 0;
}
static int x_fsetpos(void *f, const int64_t *pos) {
    int (*s)(void *, int64_t, int) = bionic("fseeko");
    if (!s) s = bionic("fseek");
    return s ? s(f, *pos, 0) : -1;
}
// the frame clock: none here (NULL: Swappy / Unity use the Java Choreographer, java_layer.c)
static void x_post_frame(void) {}
static int x_media_error(void) { return -10000; } // AMEDIA_ERROR_BASE
static const char *x_aaudio_state_text(int s) {
    static const char *const n[] = { "UNINITIALIZED", "UNKNOWN", "OPEN", "STARTING", "STARTED", "PAUSING", "PAUSED",
                                     "FLUSHING", "FLUSHED", "STOPPING", "STOPPED", "CLOSING", "CLOSED", "DISCONNECTED" };
    return s >= 0 && s < 14 ? n[s] : "UNKNOWN";
}
static int x_dirfd(void) {
    errno = EINVAL;
    return -1;
}
static int x_getifaddrs(void **list) { // no interfaces to list (the network: net_shim.c)
    *list = NULL;
    return 0;
}
static void x_nothing(void) {}
static char *x_inet_ntoa(uint32_t a) {
    static char buf[16];
    const uint8_t *b = (const uint8_t *)&a;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}
static int x_inet_aton(const char *s, uint32_t *out) {
    unsigned v[4];
    char c;
    if (sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &c) != 4) return 0;
    for (int i = 0; i < 4; i++)
        if (v[i] > 255) return 0;
    uint8_t b[4] = { (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3] };
    memcpy(out, b, 4);
    return 1;
}
static void *x_memmem(const void *h, size_t hn, const void *n, size_t nn) { // (not in the console's libc)
    if (!nn) return (void *)h;
    const uint8_t *p = h, *e = p + hn;
    for (; (size_t)(e - p) >= nn; p++) {
        p = memchr(p, *(const uint8_t *)n, (size_t)(e - p) - nn + 1);
        if (!p) return NULL;
        if (!memcmp(p, n, nn)) return (void *)p;
    }
    return NULL;
}
// regex: bionic's regex_t / regmatch_t are BSD's (Henry Spencer's), the same layout and flags
static int x_regcomp(void *re, const char *p, int f) { return regcomp(re, p, f); }
static int x_regexec(const void *re, const char *s, size_t n, void *m, int f) { return regexec(re, s, n, m, f); }
static void x_regfree(void *re) { regfree(re); }
static size_t x_wcstombs(char *d, const wchar_t *s, size_t n) { return wcstombs(d, s, n); }
static size_t x_wcsrtombs(char *d, const wchar_t **s, size_t n, mbstate_t *st) { return wcsrtombs(d, s, n, st); }
static int x_wcscmp(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
static wchar_t *x_wcscpy(wchar_t *d, const wchar_t *s) { return wcscpy(d, s); }
static int x_pthread_gettid_np(pthread_t t) {
    (void)t;
    int (*g)(void) = ours("gettid");
    return g ? g() : 1;
}
static int x_pthread_getname_np(pthread_t t, char *buf, size_t n) {
    (void)t;
    if (n) buf[0] = 0;
    return 0;
}
// bionic's pthread_cleanup_push / pop: { prev, routine, arg }; pop with execute runs the routine (ARM)
typedef struct { void *prev, *routine, *arg; } x_cleanup;
void *arm_callback(uint64_t guest);
static void x_cleanup_push(x_cleanup *c, void *routine, void *arg) {
    c->prev = NULL;
    c->routine = routine;
    c->arg = arg;
}
static void x_cleanup_pop(x_cleanup *c, int execute) {
    if (execute && c->routine) ((void (*)(void *))arm_callback((uint64_t)(uintptr_t)c->routine))(c->arg);
}
static char *x_asctime(const struct tm *t) {
    static char buf[32];
    return x_asctime_r(t, buf);
}
static const char *x_getprogname(void) { return "app_process64"; }
static int x_getpwnam_r(const char *name, void *pwd, char *buf, size_t n, void **res) {
    (void)name;
    int (*g)(unsigned, void *, char *, size_t, void **) = ours("getpwuid_r");
    return g ? g(10100, pwd, buf, n, res) : ENOENT;
}
// temporary files: through our open / mkdir (the game's paths are ours: linux_abi.c)
static void x_fill_x(char *t, size_t suffix) {
    size_t n = strlen(t);
    if (n < 6 + suffix) return;
    static const char abc[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = n - 6 - suffix; i < n - suffix; i++) t[i] = abc[arc4random() % 36];
}
static int x_mkstemp(char *t) {
    int (*o)(const char *, int, int) = ours("open");
    if (!o) return -1;
    for (int tries = 0; tries < 20; tries++) {
        x_fill_x(t, 0);
        int fd = o(t, 02 | 0100 | 0200, 0600); // Linux O_RDWR | O_CREAT | O_EXCL
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}
static char *x_mkdtemp(char *t) {
    int (*m)(const char *, int) = ours("mkdir");
    x_fill_x(t, 0);
    return m && m(t, 0700) == 0 ? t : NULL;
}
static int x_pause(void) { // waits for a signal: here, for good
    for (;;) sleep(3600);
    return -1;
}
static int64_t x_strtoimax(const char *s, char **e, int b) { return strtoll(s, e, b); }
static int x_swscanf(const wchar_t *s, const wchar_t *f, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return swscanf(s, f, a, b, c, d); // (up to 4 results: what the games scanned ask)
}
static int x_fesetround(int mode) { return mode == 0 ? 0 : -1; } // to nearest (the guest's FPCR: ARM)
// timed locks: the game's mutex / rwlock (bionic_shim.c), tried until the deadline
static int x_timed(const char *try_name, void *lock, const struct timespec *at) {
    int (*t)(void *) = bionic(try_name);
    if (!t) return ENOSYS;
    for (;;) {
        int r = t(lock);
        if (r != EBUSY) return r;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > at->tv_sec || (now.tv_sec == at->tv_sec && now.tv_nsec >= at->tv_nsec)) return ETIMEDOUT;
        struct timespec d = { 0, 500000 };
        nanosleep(&d, NULL);
    }
}
static int x_mutex_timedlock(void *m, const struct timespec *at) { return x_timed("pthread_mutex_trylock", m, at); }
static int x_rw_timedrd(void *l, const struct timespec *at) { return x_timed("pthread_rwlock_tryrdlock", l, at); }
static int x_rw_timedwr(void *l, const struct timespec *at) { return x_timed("pthread_rwlock_trywrlock", l, at); }
static int x_rwlockattr_init(int64_t *a) {
    *a = 0;
    return 0;
}

#define X(name, fn) { name, (void *)(fn) }
static int x_api_level(void) { return 30; } // Android 11, as Build.VERSION.SDK_INT

static const struct { const char *name; void *fn; } table[] = {
    X("__memcpy_chk", x_memcpy_chk), X("__memset_chk", x_memset_chk), X("__strcat_chk", x_strcat_chk),
    X("__strcpy_chk", x_strcpy_chk), X("__strncpy_chk", x_strncpy_chk), X("__strncpy_chk2", x_strncpy_chk2),
    X("__strrchr_chk", x_strrchr_chk), X("__memchr_chk", x_memchr_chk), X("__vsprintf_chk", x_vsprintf_chk),
    X("__pread64_chk", x_pread_chk), X("__pread_chk", x_pread_chk), X("pread", x_pread), X("pread64", x_pread),
    X("__readlink_chk", x_readlink_chk), X("__FD_CLR_chk", x_fd_clr_chk),
    X("_exit", x_exit), X("execv", x_fail_nosys), X("execve", x_fail_nosys), X("execl", x_fail_nosys),
    X("execlp", x_fail_nosys), X("clone", x_fail_nosys), X("wait", x_wait), X("getppid", x_getppid),
    X("getgid", x_getgid), X("getegid", x_getgid), X("setgid", x_zero), X("setuid", x_zero), X("initgroups", x_zero),
    X("alarm", x_zero), X("__register_atfork", x_zero), X("__libc_current_sigrtmin", x_sigrtmin),
    X("strsignal", x_strsignal), X("sigwait", x_sigwait), X("process_vm_readv", x_process_vm_readv),
    X("process_vm_writev", x_process_vm_readv),
    X("sysinfo", x_sysinfo), X("getrlimit", x_getrlimit), X("setrlimit", x_setrlimit), X("getrlimit64", x_getrlimit),
    X("setrlimit64", x_setrlimit), X("getrusage", x_getrusage), X("times", x_times), X("msync", x_msync),
    X("mremap", x_mremap), X("__sched_cpucount", x_cpucount), X("__stack_chk_guard", &x_stack_chk_guard),
    X("sched_getparam", x_sched_getparam), X("sched_getscheduler", x_zero), X("sched_get_priority_max", x_zero),
    X("sched_get_priority_min", x_zero), X("pthread_getschedparam", x_pthread_getschedparam),
    X("pthread_setschedparam", x_pthread_setschedparam), X("pthread_attr_getguardsize", x_attr_getguardsize),
    X("pipe2", x_pipe2), X("epoll_create", x_epoll_create), X("epoll_create1", x_epoll_create1),
    X("epoll_ctl", x_epoll_ctl), X("epoll_wait", x_epoll_wait), X("eventfd", x_eventfd),
    X("eventfd_read", x_fail_nosys), X("eventfd_write", x_fail_nosys), X("timerfd_create", x_fail_nosys),
    X("timerfd_settime", x_fail_nosys), X("inotify_init1", x_inotify_init1), X("inotify_rm_watch", x_inotify_rm_watch),
    X("inotify_init", x_inotify_init), X("inotify_add_watch", x_inotify_add_watch),
    X("__cmsg_nxthdr", x_null), X("fdopendir", x_null),
    X("openat", x_openat), X("__openat_2", x_openat_2), X("fstatat", x_fstatat), X("fstatat64", x_fstatat),
    X("newfstatat", x_fstatat), X("unlinkat", x_unlinkat), X("mkdirat", x_mkdirat), X("renameat", x_renameat),
    X("faccessat", x_faccessat), X("fchmodat", x_fchmodat), X("readlinkat", x_readlinkat),
    X("utimensat", x_utimensat), X("pathconf", x_pathconf), X("statvfs", x_statvfs), X("statvfs64", x_statvfs),
    X("daylight", &x_daylight), X("timezone", &x_timezone), X("tzname", x_tzname), X("tzset", x_tzset),
    X("clock_nanosleep", x_clock_nanosleep), X("timegm", x_timegm), X("asctime_r", x_asctime_r),
    X("abs", x_abs), X("labs", x_labs), X("llabs", x_llabs), X("fmax", x_fmax), X("fmin", x_fmin),
    X("fmaxf", x_fmaxf), X("fminf", x_fminf), X("isascii", x_isascii), X("isgraph", x_isgraph),
    X("__isnan", x_isnan), X("__isnanf", x_isnanf), X("isnanf", x_isnanf), X("__isinf", x_isinf), X("__isinff", x_isinff),
    X("isinff", x_isinff), X("__isfinite", x_isfinite), X("__isfinitef", x_isfinitef), X("__signbit", x_signbit),
    X("__signbitf", x_signbitf), X("__fpclassifyd", x_fpclassify), X("__fpclassifyf", x_fpclassifyf), X("__fpclassify", x_fpclassify),
    X("atoll", x_atoll), X("__gnu_strerror_r", x_gnu_strerror_r), X("strndup", strndup), X("strtok", strtok),
    X("random", random), X("arc4random", arc4random), X("dirname", dirname),
    X("__android_log_buf_write", x_android_log_buf_write), X("ctime", x_ctime), X("rewinddir", rewinddir),
    X("sigismember", x_sigismember), X("pthread_attr_getschedparam", x_attr_getschedparam),
    X("pthread_attr_setschedparam", x_attr_setschedparam), X("res_search", x_res_search),
    X("setegid", x_zero), X("seteuid", x_zero), X("setgroups", x_zero), X("setregid", x_zero),
    X("setresgid", x_zero), X("setresuid", x_zero), X("setreuid", x_zero),
    X("AndroidBitmap_getInfo", x_bitmap_fail), X("AndroidBitmap_lockPixels", x_bitmap_fail),
    X("AndroidBitmap_unlockPixels", x_bitmap_fail), X("ANativeWindow_lock", x_bitmap_fail),
    X("ANativeWindow_unlockAndPost", x_bitmap_fail), X("frexp", frexp), X("sinhf", sinhf),
    X("isspace", isspace), X("isprint", isprint), X("iswalnum", iswalnum), X("iswctype", iswctype),
    X("wctype", wctype), X("wcschr", wcschr),
    // libm
    X("tanhf", tanhf), X("hypotf", hypotf), X("nextafter", nextafter), X("nextafterf", nextafterf),
    X("acosh", acosh), X("asinh", asinh), X("atanh", atanh), X("cbrt", cbrt), X("erff", erff), X("expm1", expm1),
    X("ilogbf", ilogbf), X("lrintf", lrintf), X("lrint", lrint), X("lround", lround), X("lroundf", lroundf),
    X("llround", llround), X("llroundf", llroundf), X("llrint", llrint), X("nan", nan), X("remainderf", remainderf), X("round", round),
    X("scalbnf", scalbnf), X("trunc", trunc),
    // zlib (Unity's: its inflate is ours already, linux_abi.c)
    X("deflate", deflate), X("deflateEnd", deflateEnd), X("deflateInit2_", droid_deflateInit2_),
    X("deflateInit_", droid_deflateInit_), X("inflateInit_", droid_inflateInit_), X("deflateBound", deflateBound),
    X("zError", zError), X("crc32", crc32), X("zlibVersion", zlibVersion),
    X("strsep", strsep), X("android_get_device_api_level", x_api_level),
    // the 2026-10-26 batch
    X("sys_signame", x_sys_signame), X("getopt_long", x_getopt), X("getopt", x_getopt), X("optarg", &x_optarg),
    X("optind", &x_optind), X("opterr", &x_opterr), X("optopt", &x_optopt), X("__libc_init", x_libc_init),
    X("mallinfo", x_mallinfo), X("fputwc", x_weof), X("getwc", x_weof), X("ungetwc", x_weof), X("fgetwc", x_weof),
    X("mlock", x_zero), X("munlock", x_zero), X("arc4random_buf", x_arc4random_buf),
    X("arc4random_uniform", x_arc4random_uniform), X("freopen", x_null_enosys), X("freopen64", x_null_enosys),
    X("tmpfile", x_null_enosys), X("funopen", x_null_enosys), X("log1p", x_log1p), X("fabs", x_fabs),
    X("frexpf", x_frexpf), X("expm1f", x_expm1f), X("erf", x_erf), X("erfcf", x_erfcf), X("tgammaf", x_tgammaf),
    X("nanf", x_nanf), X("ldiv", x_ldiv), X("floorl", x_floorl), X("frexpl", x_frexpl), X("ldexpl", x_ldexpl),
    X("recvmmsg", x_fail_nosys), X("sendmmsg", x_fail_nosys), X("strptime", x_strptime),
    X("tcgetattr", x_fail_notty), X("tcsetattr", x_fail_notty), X("tcflush", x_fail_notty),
    X("__strlcpy_chk", x_strlcpy_chk), X("__strlcat_chk", x_strlcat_chk), X("strlcat", x_strlcat),
    X("__write_chk", x_write_chk), X("__pwrite64_chk", x_pwrite_chk), X("__fread_chk", x_fread_chk),
    X("__fwrite_chk", x_fwrite_chk), X("__fgets_chk", x_fgets_chk), X("__sendto_chk", x_sendto_chk),
    X("fgetpos", x_fgetpos), X("fsetpos", x_fsetpos),
    X("AChoreographer_getInstance", x_null), X("AChoreographer_postFrameCallback", x_post_frame),
    X("AChoreographer_postFrameCallback64", x_post_frame), X("AChoreographer_postFrameCallbackDelayed", x_post_frame),
    X("AImage_getWidth", x_media_error), X("AImage_getHeight", x_media_error),
    X("AMotionEvent_getButtonState", x_zero), X("AKeyEvent_getFlags", x_zero),
    X("ANativeActivity_setWindowFormat", x_nothing), /* (ASensorManager_getInstanceForPackage: sensors.c) */
    X("AAudio_convertStreamStateToText", x_aaudio_state_text), X("AMediaDrm_createByUUID", x_null),
    X("AMediaDrm_getPropertyByteArray", x_media_error), X("AMediaDrm_release", x_nothing),
    X("dirfd", x_dirfd), X("fchown", x_zero), X("chown", x_zero), X("fallocate", x_fail_notsup),
    X("getifaddrs", x_getifaddrs), X("freeifaddrs", x_nothing), X("getgrnam", x_null), X("getgrgid", x_null),
    X("inet_ntoa", x_inet_ntoa), X("inet_aton", x_inet_aton), X("memmem", x_memmem), X("nftw", x_fail_nosys),
    X("regcomp", x_regcomp), X("regexec", x_regexec), X("regfree", x_regfree),
    X("timer_create", x_fail_nosys), X("timer_settime", x_fail_nosys), X("timer_delete", x_fail_nosys),
    X("setitimer", x_fail_nosys), X("wcstombs", x_wcstombs), X("wcsrtombs", x_wcsrtombs), X("wcscmp", x_wcscmp),
    X("wcscpy", x_wcscpy), X("swscanf", x_swscanf), X("pthread_gettid_np", x_pthread_gettid_np),
    X("pthread_getname_np", x_pthread_getname_np), X("__pthread_cleanup_push", x_cleanup_push),
    X("__pthread_cleanup_pop", x_cleanup_pop), X("__system_property_foreach", x_zero),
    X("__system_property_read_callback", x_nothing), X("asctime", x_asctime), X("getprogname", x_getprogname),
    X("getpwnam_r", x_getpwnam_r), X("mkstemp", x_mkstemp), X("mkdtemp", x_mkdtemp), X("nice", x_zero),
    X("pause", x_pause), X("vfork", x_fail_nosys), X("execvpe", x_fail_nosys), X("system", x_fail_nosys),
    X("vsyslog", x_nothing), X("strtoimax", x_strtoimax), X("fesetround", x_fesetround),
    X("getxattr", x_fail_notsup), X("setxattr", x_fail_notsup), X("listxattr", x_fail_notsup),
    X("removexattr", x_fail_notsup), X("fgetxattr", x_fail_notsup), X("fsetxattr", x_fail_notsup),
    X("flistxattr", x_fail_notsup), X("fremovexattr", x_fail_notsup),
    X("pthread_mutex_timedlock", x_mutex_timedlock), X("pthread_rwlock_timedrdlock", x_rw_timedrd),
    X("pthread_rwlock_timedwrlock", x_rw_timedwr), X("pthread_rwlockattr_init", x_rwlockattr_init),
    X("pthread_rwlockattr_setpshared", x_zero), X("pthread_rwlockattr_destroy", x_zero),
    X("isnan", x_isnan), X("putwc", x_weof), X("wcsftime", x_zero), // (Mortar: Fruit Ninja 2.x)
};

// names that are another call of ours (the 64-bit file calls are the same on arm64)
static const char *const aliases[][2] = {
    { "fstat64", "fstat" }, { "lstat64", "lstat" }, { "stat64", "stat" }, { "sendfile64", "sendfile" },
    { "sigsetjmp", "setjmp" }, { "siglongjmp", "longjmp" }, { "fseeko64", "fseeko" }, { "ftello64", "ftello" },
    { "fopen64", "fopen" }, { "mmap64", "mmap" }, { "ftruncate64", "ftruncate" }, { "open64", "open" },
    // BoringSSL's memory hooks (weak: when found it takes every block from them; a stub answering
    // 0 left its TLS with no memory - Genshin Impact: "Failed to connect to the server", 4204)
    { "OPENSSL_memory_alloc", "malloc" }, { "OPENSSL_memory_free", "free" },
    { "OPENSSL_memory_get_size", "malloc_usable_size" },
};
static char *x_env_list[] = { "PATH=/system/bin", "ANDROID_ROOT=/system", NULL };
static char **x_environ = x_env_list;

void *libc_extra_resolve(const char *name) {
    void *droid_resolve(const char *name);
    static int off = -1; // DROID_NO_EXTRA=1: none of these (the old stubs), to compare
    if (off < 0) off = getenv("DROID_NO_EXTRA") != NULL;
    if (off) return NULL;
    const char *only = getenv("DROID_EXTRA_SKIP"); // one name (or a comma list) left out, to find which
    if (only && strstr(only, name)) return NULL;
    if (!strcmp(name, "environ")) return &x_environ;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (!strcmp(aliases[i][0], name)) {
            void *p = linux_abi_resolve(aliases[i][1]);
            return p ? p : droid_resolve(aliases[i][1]);
        }
    return NULL;
}
