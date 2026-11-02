// bionic_shim.c — Android's C library (bionic) as a game imports it, on the
// PS5's FreeBSD libc. Most names pass straight through; these differ:
// stdio's stdin/stdout/stderr (bionic's __sF), errno, pthread_once (4 bytes
// in bionic, 16 in FreeBSD), clock ids, dlopen/dlsym, and Android paths.

#include <errno.h>
#include <strings.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#ifdef DROID_PC
#include <sys/syscall.h>
#endif
#include <poll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <locale.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <utime.h>
#include <sys/time.h>

#include "game_settings.h"
#include "android_shim.h"
#include "arm_cpu.h"
#include "arm_modules.h"

#define DATA_DIR APP0 "/data"

// ---- stdio: bionic's __sF[3] are stdin, stdout, stderr ------------------

#define BIONIC_FILE_SIZE 152
static char bionic_sF[3][BIONIC_FILE_SIZE];
// newer bionic's stdin / stdout / stderr: variables holding &__sF[n] (older Unity uses them:
// Dead Trigger 2's libunity read a bad stdout and stopped in a constructor)
static void *bionic_std[3] = { bionic_sF[0], bionic_sF[1], bionic_sF[2] };

static int is_std(FILE *f) {
    return (char *)f >= (char *)bionic_sF && (char *)f < (char *)bionic_sF + sizeof(bionic_sF);
}

static int std_index(FILE *f) {
    return (int)(((char *)f - bionic_sF[0]) / BIONIC_FILE_SIZE);
}

// stdout/stderr go to android.log, a line at a time.
static char std_line[2][1024];
static size_t std_len[2];
static pthread_mutex_t std_lock = PTHREAD_MUTEX_INITIALIZER;

static void std_write(FILE *f, const char *data, size_t n) {
    int s = std_index(f) == 2 ? 1 : 0;
    pthread_mutex_lock(&std_lock);
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n' || std_len[s] == sizeof(std_line[0]) - 1) {
            std_line[s][std_len[s]] = 0;
            droid_log("[%s] %s", s ? "stderr" : "stdout", std_line[s]);
            std_len[s] = 0;
            if (data[i] == '\n') continue;
        }
        std_line[s][std_len[s]++] = data[i];
    }
    pthread_mutex_unlock(&std_lock);
}

static int b_vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!is_std(f)) return vfprintf(f, fmt, ap);
    char text[2048];
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    std_write(f, text, n < 0 ? 0 : (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return n;
}

static int b_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = b_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int b_fputs(const char *s, FILE *f) {
    if (!is_std(f)) return fputs(s, f);
    std_write(f, s, strlen(s));
    return 1;
}

static int b_fputc(int c, FILE *f) {
    if (!is_std(f)) return fputc(c, f);
    char ch = (char)c;
    std_write(f, &ch, 1);
    return c;
}

static size_t b_fwrite(const void *p, size_t size, size_t n, FILE *f) {
    if (!is_std(f)) return fwrite(p, size, n, f);
    std_write(f, p, size * n);
    return n;
}

static int b_fflush(FILE *f) {
    if (!f || is_std(f)) return 0;
    return fflush(f);
}

// Unbuffered (or a tiny buffer): bionic's fread still reads a big request in one go,
// FreeBSD's refills one byte at a time - a system call per byte (Unity: 0.4 MB/s).
// So files always get a 64 KB buffer; only the game's own big buffers are kept.
static int b_setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (is_std(f)) return 0;
    static int logged;
    if (logged++ < 8) droid_log("setvbuf(mode %d, %zu bytes%s)", mode, size, buf ? ", its buffer" : "");
    if (mode == _IONBF || size < 4096) return setvbuf(f, NULL, _IOFBF, 64 * 1024);
    return setvbuf(f, buf, mode, size);
}
static void b_setbuf(FILE *f, char *buf) {
    if (!is_std(f)) b_setvbuf(f, buf, buf ? _IOFBF : _IONBF, buf ? BUFSIZ : 0);
}
static void b_clearerr(FILE *f) {
    if (!is_std(f)) clearerr(f);
}

static int b_fileno(FILE *f) { return is_std(f) ? std_index(f) : fileno(f); }

static int b_fclose(FILE *f) {
    return is_std(f) ? 0 : fclose(f);
}

static int b_fscanf(FILE *f, const char *fmt, ...) {
    if (is_std(f)) return EOF;
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

// Android storage paths land in the game's own folder.
static const char *map_path(const char *path, char *buf, size_t size) {
    static const char *prefixes[] = { "/mnt/sdcard/", "/sdcard/", "/storage/emulated/0/",
                                      "/data/data/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(path, prefixes[i], n) == 0) {
            snprintf(buf, size, "%s/%s", game_dir, path + n);
            // the file's folder must exist
            char dir[512];
            snprintf(dir, sizeof(dir), "%s", buf);
            for (char *p = dir + strlen(game_dir) + 1; *p; p++) {
                if (*p != '/') continue;
                *p = 0;
                mkdir(dir, 0777);
                *p = '/';
            }
            return buf;
        }
    }
    return path;
}

int droid_proc_open(const char *path);
const char *droid_real_path(const char *path, char *buf, size_t n); // linux_abi.c

const char *droid_abs(const char *path, char *buf, size_t n); // linux_abi.c: relative paths from our cwd

static FILE *b_fopen(const char *path, const char *mode) {
    char ab[600];
    path = droid_abs(path, ab, sizeof(ab));
    char buf[512];
    const char *real = map_path(path, buf, sizeof(buf));
    int proc = droid_proc_open(path); // the PS5 has no /proc: made by us (linux_abi.c)
    if (proc != -2) return proc >= 0 ? fdopen(proc, "r") : NULL;
    char fast[512];
    void ib2_sixty(const char *path, int flags); // linux_abi.c: IB2's startup commands at 60 fps
    if (mode && mode[0] == 'r' && !strchr(mode, '+')) ib2_sixty(real, 0);
    FILE *f = fopen(droid_real_path(real, fast, sizeof(fast)), mode);
    droid_log("fopen(%s%s%s, %s) = %p", path, real != path ? " -> " : "", real != path ? real : "",
              mode, (void *)f);
    return f;
}

// ---- errno, pthread_once, clocks -----------------------------------------

// errno numbers: the game's bionic code compares with Linux's. The PS5's libc is
// FreeBSD's (EAGAIN 35, ETIMEDOUT 60, EINPROGRESS 36...): translated when the game
// asks for errno. The host's is then cleared, so a value the game writes stays its own.
int droid_linux_errno(int e) {
#ifdef __linux__
    return e;
#else
    static const unsigned char bsd_to_linux[] = {
        [11] = 35,  [35] = 11,  [36] = 115, [37] = 114, [38] = 88,  [39] = 89,  [40] = 90,  [41] = 91,
        [42] = 92,  [43] = 93,  [44] = 94,  [45] = 95,  [46] = 96,  [47] = 97,  [48] = 98,  [49] = 99,
        [50] = 100, [51] = 101, [52] = 102, [53] = 103, [54] = 104, [55] = 105, [56] = 106, [57] = 107,
        [58] = 108, [59] = 109, [60] = 110, [61] = 111, [62] = 40,  [63] = 36,  [64] = 112, [65] = 113,
        [66] = 39,  [68] = 87,  [69] = 122, [70] = 116, [71] = 66,  [77] = 37,  [78] = 38,  [82] = 43,
        [83] = 42,  [84] = 75,  [85] = 125, [86] = 84,  [89] = 74,  [90] = 72,  [91] = 67,  [92] = 71,
    };
    if (e > 0 && e < (int)sizeof(bsd_to_linux) && bsd_to_linux[e]) return bsd_to_linux[e];
    return e;
#endif
}

#ifdef __linux__
static int *b_errno(void) { return &errno; }
#else
static __thread int guest_errno;
static int *b_errno(void) {
    if (errno) {
        guest_errno = droid_linux_errno(errno);
        errno = 0;
    }
    return &guest_errno;
}
#endif

// Condition waits tell where they wait: a signal for the thread wakes it (a spurious wake,
// allowed), so the GC's pause reaches it. A timed wait's "timed out": Linux's number.
// The signal runs here with the wait's mutex given back, as Linux runs it inside the wait: a
// thread paused holding it made another one wait for it while the GC waited for that one
// (Genshin Impact: its GC finalizer paused with the lock Noelle Main wanted). Not while the
// thread holds another lock (arm_cpu.cpp's count: this mutex is one).
int arm_locks_held(void);
static void cond_take_signals(pthread_mutex_t *m) {
    if (!arm_sig_waiting() || arm_locks_held() > 1) return;
    pthread_mutex_unlock(m);
    arm_sig_take();
    pthread_mutex_lock(m);
}
static void mutex_from_initializer(pthread_mutex_t *m);

// Condition waits of our own (Vector 2 only so far): the game's mutex is given back for the
// whole wait, the thread waits on a private one (held only for a moment), and a signal for it
// runs its handler there, without the game's mutex - as Linux runs it inside the wait. With
// the console's / PC's own wait, the GC's pause signal could not wake it while another thread
// (paused by the same GC) held the game's mutex: Vector 2's IL2CPP thread-pool workers never
// answered and the game hung at its logo. signal / broadcast count up the condition's
// generation and wake its waiters (all: a spurious wake is allowed).
#define OWN_COND_GAMES "com.nekki.vector2"
extern const char *java_package;
static int own_conds(void) {
    static int on = -1;
    if (on < 0 && java_package) on = droid_game_in(OWN_COND_GAMES);
    return on > 0;
}
#define OC_N 64
static struct { pthread_mutex_t m; pthread_cond_t cv; unsigned gen; } oc[OC_N];
static pthread_once_t oc_once = PTHREAD_ONCE_INIT;
static void oc_init(void) {
    for (int i = 0; i < OC_N; i++) {
        pthread_mutex_init(&oc[i].m, NULL);
        pthread_cond_init(&oc[i].cv, NULL);
    }
}
static unsigned oc_slot(const void *c) { return (unsigned)(((uintptr_t)c >> 3) % OC_N); }
static int oc_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) {
    pthread_once(&oc_once, oc_init);
    const unsigned s = oc_slot(c);
    pthread_mutex_lock(&oc[s].m);
    const unsigned gen = oc[s].gen;
    pthread_mutex_unlock(m);
    int r = 0;
    while (oc[s].gen == gen) {
        if (arm_sig_waiting()) {
            pthread_mutex_unlock(&oc[s].m);
            arm_sig_take();
            pthread_mutex_lock(&oc[s].m);
            continue;
        }
        struct timespec now, until;
        clock_gettime(CLOCK_REALTIME, &now);
        if (t && (now.tv_sec > t->tv_sec || (now.tv_sec == t->tv_sec && now.tv_nsec >= t->tv_nsec))) {
            r = ETIMEDOUT;
            break;
        }
        until = now; // 50 ms at most: a missed wake costs that, never the whole wait
        until.tv_nsec += 50000000;
        if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
        if (t && (t->tv_sec < until.tv_sec || (t->tv_sec == until.tv_sec && t->tv_nsec < until.tv_nsec))) until = *t;
        arm_sig_wait_cond(&oc[s].cv, &oc[s].m);
        pthread_cond_timedwait(&oc[s].cv, &oc[s].m, &until);
        arm_sig_wait_cond(NULL, NULL);
    }
    pthread_mutex_unlock(&oc[s].m);
    while (pthread_mutex_trylock(m) != 0) { // the game's mutex again; a pause meanwhile still answered
        if (arm_sig_waiting()) arm_sig_take();
        else usleep(50);
    }
    return r ? droid_linux_errno(r) : 0;
}
static void oc_wake(pthread_cond_t *c) {
    pthread_once(&oc_once, oc_init);
    const unsigned s = oc_slot(c);
    pthread_mutex_lock(&oc[s].m);
    oc[s].gen++;
    pthread_cond_broadcast(&oc[s].cv);
    pthread_mutex_unlock(&oc[s].m);
}
static int b_cond_signal(pthread_cond_t *c) {
    if (own_conds()) {
        oc_wake(c);
        return 0;
    }
    return pthread_cond_signal(c);
}
static int b_cond_broadcast(pthread_cond_t *c) {
    if (own_conds()) {
        oc_wake(c);
        return 0;
    }
    return pthread_cond_broadcast(c);
}

static int b_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    mutex_from_initializer(m);
    if (own_conds()) return oc_wait(c, m, NULL);
    if (arm_sig_waiting()) { // a signal to take first: as a spurious wake
        cond_take_signals(m);
        return 0;
    }
    arm_sig_wait_cond(c, m);
    int r = pthread_cond_wait(c, m);
    if (r == EPERM) { // the mutex not held: bionic (and glibc) wait anyway and hand it back locked; the
        pthread_mutex_lock(m); // console said no at once and the thread spun (cocos2d-x's texture
        r = pthread_cond_wait(c, m); // loader: 4.5 million waits a second in Geometry Dash)
    }
    arm_sig_wait_cond(NULL, NULL);
    cond_take_signals(m);
    return r ? droid_linux_errno(r) : 0;
}
static int b_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) {
    mutex_from_initializer(m);
    if (own_conds()) return oc_wait(c, m, t);
    if (arm_sig_waiting()) {
        cond_take_signals(m);
        return 0;
    }
    arm_sig_wait_cond(c, m);
    int r = pthread_cond_timedwait(c, m, t);
    if (r == EPERM) { // (as above)
        pthread_mutex_lock(m);
        r = pthread_cond_timedwait(c, m, t);
    }
    arm_sig_wait_cond(NULL, NULL);
    cond_take_signals(m);
    return r ? droid_linux_errno(r) : 0;
}

// Locks and semaphores: a thread blocked in one still takes its signals (Linux runs a
// handler in the middle of the wait). Without that, a thread waiting for a lock when the GC
// paused the world never answered the pause, and the game froze (Subway Surfers at 0%).
// Uncontended: one try. Contended: waits of 2 ms, the signals taken between them.
static void wait_step(struct timespec *t) {
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_nsec += 2000000;
    if (t->tv_nsec >= 1000000000) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000;
    }
}
// A mutex the game never initialised, only set to bionic's static initializer: its first word
// holds the type in bits 14-15 (PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP 0x4000, the error-check
// one 0x8000; 0x2000 shared). Handed on as it is, glibc read 0x4000 as "locked, owner 0" and every
// lock failed (Genshin Impact's libMiHoYoMTRSDK: its main thread retried forever at "Preparing to
// download resources"); FreeBSD (the PS5) would take it for a pointer. Made a real one first.
static pthread_mutex_t static_init_lock = PTHREAD_MUTEX_INITIALIZER;
static void mutex_from_initializer(pthread_mutex_t *m) {
    const volatile uint32_t *w = (const volatile uint32_t *)(void *)m;
    if (!(w[0] & 0xC000) || (w[0] & ~0xE000u) || w[1] || w[2] || w[3]) return; // not one: as it is
    pthread_mutex_lock(&static_init_lock);
    const uint32_t first = w[0];
    if ((first & 0xC000) && !(first & ~0xE000u) && !w[1] && !w[2] && !w[3]) { // (another thread: done)
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, (first & 0xC000) == 0x4000 ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
        memset((void *)m, 0, 40); // bionic's mutex is 40 bytes (arm64)
        pthread_mutex_init(m, &a);
        pthread_mutexattr_destroy(&a);
        static int told;
        if (told++ < 8)
            droid_log("bionic: a %s mutex from its static initializer (%#x) made at %p",
                      (first & 0xC000) == 0x4000 ? "recursive" : "error-check", first, (void *)m);
    }
    pthread_mutex_unlock(&static_init_lock);
}
static int b_mutex_trylock(pthread_mutex_t *m) {
    mutex_from_initializer(m);
    int r = pthread_mutex_trylock(m);
    return r ? droid_linux_errno(r) : 0;
}
static int b_mutex_unlock(pthread_mutex_t *m) {
    mutex_from_initializer(m);
    int r = pthread_mutex_unlock(m);
    return r ? droid_linux_errno(r) : 0;
}
static int b_mutex_lock(pthread_mutex_t *m) {
    mutex_from_initializer(m);
    int r = pthread_mutex_trylock(m);
    if (r != EBUSY) return r ? droid_linux_errno(r) : 0;
    for (;;) {
        if (arm_sig_waiting() && !arm_locks_held()) arm_sig_take(); // (holding another lock: not now)
        struct timespec t;
        wait_step(&t);
        r = pthread_mutex_timedlock(m, &t);
        if (r != ETIMEDOUT) return r ? droid_linux_errno(r) : 0;
    }
}
static int b_rwlock_rdlock(pthread_rwlock_t *l) {
    int r = pthread_rwlock_tryrdlock(l);
    if (r != EBUSY) return r ? droid_linux_errno(r) : 0;
    for (;;) {
        if (arm_sig_waiting() && !arm_locks_held()) arm_sig_take(); // (holding another lock: not now)
        struct timespec t;
        wait_step(&t);
        r = pthread_rwlock_timedrdlock(l, &t);
        if (r != ETIMEDOUT) return r ? droid_linux_errno(r) : 0;
    }
}
// Unreal games with .NET (Wuthering Waves' Mono): no ICU here (Android's libicuuc / libicui18n),
// so .NET runs in invariant globalization mode instead of stopping ("Process terminated.").
static char *b_getenv(const char *name) {
    if ((droid_quirks & QUIRK_UNREAL) && name && !strcmp(name, "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT"))
        return (char *)"1";
    return getenv(name);
}

// try forms: missing before, they answered 0 without taking the lock (Wuthering Waves' game
// thread then waited forever on a write lock)
static int b_rwlock_tryrdlock(pthread_rwlock_t *l) {
    int r = pthread_rwlock_tryrdlock(l);
    return r ? droid_linux_errno(r) : 0;
}
static int b_rwlock_trywrlock(pthread_rwlock_t *l) {
    int r = pthread_rwlock_trywrlock(l);
    return r ? droid_linux_errno(r) : 0;
}
static int b_rwlock_wrlock(pthread_rwlock_t *l) {
    int r = pthread_rwlock_trywrlock(l);
    if (r != EBUSY) return r ? droid_linux_errno(r) : 0;
    for (;;) {
        if (arm_sig_waiting() && !arm_locks_held()) arm_sig_take(); // (holding another lock: not now)
        struct timespec t;
        wait_step(&t);
        r = pthread_rwlock_timedwrlock(l, &t);
        if (r != ETIMEDOUT) return r ? droid_linux_errno(r) : 0;
    }
}
// sem_wait on the host semaphore (the PC runner keeps it behind a pointer)
static int sem_wait_signals(sem_t *h) {
    if (sem_trywait(h) == 0) return 0;
    for (;;) {
        if (arm_sig_waiting() && !arm_locks_held()) arm_sig_take(); // (holding another lock: not now)
        struct timespec t;
        wait_step(&t);
        if (sem_timedwait(h, &t) == 0) return 0;
        if (errno != ETIMEDOUT && errno != EINTR) return -1;
        errno = 0;
    }
}

// sleeps that a signal cuts short, as on Linux (EINTR): the GC's pause reaches a thread
// sleeping in a spin (il2cpp's GC lock waits in nanosleep; Among Us froze, its pause
// pending on such a thread forever)
static int64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int sleep_signals(int64_t ns, struct timespec *rem) {
    int64_t end = mono_ns() + ns;
    for (;;) {
        int64_t left = end - mono_ns();
        if (arm_sig_waiting()) {
            arm_sig_take();
            if (left < 0) left = 0;
            if (rem) { rem->tv_sec = (time_t)(left / 1000000000LL); rem->tv_nsec = (long)(left % 1000000000LL); }
            errno = EINTR;
            return -1;
        }
        if (left <= 0) return 0;
        if (left > 2000000) left = 2000000;
        struct timespec step = { 0, (long)left };
        nanosleep(&step, NULL);
    }
}
static int b_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    return sleep_signals((int64_t)req->tv_sec * 1000000000LL + req->tv_nsec, rem);
}
static int b_usleep(unsigned us) { return sleep_signals((int64_t)us * 1000, NULL); }
static unsigned b_sleep(unsigned sec) {
    struct timespec rem = { 0, 0 };
    return sleep_signals((int64_t)sec * 1000000000LL, &rem) ? (unsigned)rem.tv_sec + (rem.tv_nsec > 0) : 0;
}

// strcasestr: not in every libc (the console's: a call to it was a jump to 0), and games import
// it; Hydroid's own code calls it as droid_strcasestr too
char *droid_strcasestr(const char *h, const char *n) {
    size_t nl = strlen(n);
    for (; *h; h++)
        if (!strncasecmp(h, n, nl)) return (char *)h;
    return nl ? NULL : (char *)h;
}

// pipe, logged (curl's name lookups start a thread with a pipe to wake
// it: on the console a game's every lookup failed without ever reaching getaddrinfo)
// malloc_usable_size: the block's real size. Missing, it was 0 to V8 (Vector, Shadow Fight 3
// / 4): its memory zones took that for their size and wrote past their 8 KB blocks, over our
// own heap (found by HeapGuard)
size_t malloc_usable_size(void *p);
// big blocks the console's malloc refused come from direct memory (linux_abi.c droid_big_alloc)
void *droid_big_alloc(size_t size);
size_t droid_big_size(void *p);
int droid_big_free(void *p);
static size_t b_malloc_usable_size(void *p) {
    if (!p) return 0;
    size_t big = droid_big_size(p);
    return big ? big : malloc_usable_size(p);
}
// memmove of our own (OWN_MEMMOVE_GAMES): the console's libc memmove called abort() on one of
// Candy Crush's copies when Play or Settings' close was pressed (trace.txt's CRASH lines: SIGABRT
// inside memmove, from the game's thread); glibc on the PC copies it. A plain overlap-safe copy;
// a size past 1 GB is logged first (a bad size would show here, not as a silent abort)
#define OWN_MEMMOVE_GAMES "com.king.candycrushsaga"
_Thread_local const void *droid_memmove_last[3]; // this thread's last own memmove (the crash report shows it)
__attribute__((no_builtin)) // (the loops below must not turn back into libc's memmove)
static void *b_memmove(void *d, const void *s, size_t n) {
    static int own = -1;
    if (own < 0 && java_package) own = droid_game_in(OWN_MEMMOVE_GAMES);
    if (own <= 0) return memmove(d, s, n);
    droid_memmove_last[0] = d, droid_memmove_last[1] = s, droid_memmove_last[2] = (const void *)n;
    if (n > ((size_t)1 << 62)) { // a negative size (0xff..fc: -4): a vector erase of its end() on the
        // console only; copied, it ran over all memory. Nothing is copied; trace.txt says from where
        void arm_call_from(char *out, size_t n);            // arm_cpu.cpp
        void droid_trace_write(const char *line, int n);    // arm_cpu.cpp
        static int told;
        if (told++ < 20) {
            char at[96], l[200];
            arm_call_from(at, sizeof(at));
            int k = snprintf(l, sizeof(l), "memmove: negative size %lld, %p <- %p, from %s\n", (long long)n, d, s, at);
            droid_trace_write(l, k);
            droid_log("%.*s", k - 1, l);
        }
        return d;
    }
    if (n > (1u << 30)) {
        static int told;
        if (told++ < 8) droid_log("memmove: %zu bytes %p <- %p (a bad size?)", n, d, s);
    }
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || !n) return d;
    if (dp < sp || dp >= sp + n) { // forward: no overlap ahead of the source
        while (n >= 8 && (((uintptr_t)dp | (uintptr_t)sp) & 7) == 0) {
            *(uint64_t *)dp = *(const uint64_t *)sp;
            dp += 8, sp += 8, n -= 8;
        }
        while (n--) *dp++ = *sp++;
    } else { // backward
        dp += n, sp += n;
        while (n >= 8 && (((uintptr_t)dp | (uintptr_t)sp) & 7) == 0) {
            dp -= 8, sp -= 8, n -= 8;
            *(uint64_t *)dp = *(const uint64_t *)sp;
        }
        while (n--) *--dp = *--sp;
    }
    return d;
}
static void *b_malloc(size_t n) {
    void *p = malloc(n);
    return p ? p : droid_big_alloc(n);
}
static void *b_calloc(size_t a, size_t b) {
    void *p = calloc(a, b);
    if (p || !b || a > (size_t)-1 / b) return p;
    return droid_big_alloc(a * b); // (direct memory comes cleared)
}
static void b_free(void *p) {
    if (!droid_big_free(p)) free(p);
}
static void *b_realloc(void *p, size_t n) {
    size_t big = droid_big_size(p);
    if (!big) {
        void *q = realloc(p, n);
        if (q || !n) return q;
        q = droid_big_alloc(n); // grown past what malloc gives: moved to a big block
        if (q && p) {
            memcpy(q, p, malloc_usable_size(p) < n ? malloc_usable_size(p) : n);
            free(p);
        }
        return q;
    }
    if (n <= big) return p;
    void *q = malloc(n);
    if (!q) q = droid_big_alloc(n);
    if (q) {
        memcpy(q, p, big);
        droid_big_free(p);
    }
    return q;
}
static int b_posix_memalign(void **out, size_t align, size_t n) {
    int r = posix_memalign(out, align, n);
    if (r == 0 || align > 0x10000) return r;
    void *p = droid_big_alloc(n);
    if (!p) return r;
    *out = p;
    return 0;
}

void net_nonblock_set(int fd, int on); // net_shim.c
void netspy_closed(int fd);             // net_shim.c: NetSpy's summary of a connection
int wb_close(int fd); // write_behind.c: a big file closed by its writer, after its writes
int vfile_owns(int fd); // vfile.c: a file read out of the .ipa (the Infinity Blade ports)
int vfile_close(int fd);
ssize_t vfile_read(int fd, void *buf, size_t n);
static ssize_t b_read(int fd, void *buf, size_t n) { return vfile_owns(fd) ? vfile_read(fd, buf, n) : read(fd, buf, n); }
static int b_close(int fd) {
    if (vfile_owns(fd)) return vfile_close(fd);
    if (wb_close(fd)) return 0;
    netspy_closed(fd);
    return close(fd);
}
static int b_pipe(int fds[2]) {
    int r = pipe(fds);
    int e = errno;
    if (r == 0) { net_nonblock_set(fds[0], 0); net_nonblock_set(fds[1], 0); } // reused numbers
    static int shown;
    if (shown < 20) {
        shown++;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        droid_log("sys: pipe -> %d (fds %d %d, errno %d, t=%lld ms)", r, r == 0 ? fds[0] : -1, r == 0 ? fds[1] : -1,
                  r ? e : 0, (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000);
    }
    errno = e;
    return r;
}

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

// bionic's pthread_once_t is an int: 0 = not run.
static int b_pthread_once(int *once, void (*fn)(void)) {
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) return 0;
    pthread_mutex_lock(&once_lock);
    if (*once == 0) {
        *once = 1;
        pthread_mutex_unlock(&once_lock);
        fn();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        return 0;
    }
    pthread_mutex_unlock(&once_lock);
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) sched_yield();
    return 0;
}

#ifdef __linux__
// PC test runner: glibc's sem_t is 32 bytes, bionic's 16 -> the host one lives
// on the heap, the game's 16 bytes hold a pointer to it.
static sem_t *host_sem(void *s) { return *(sem_t **)s; }
static int b_sem_init(void *s, int pshared, unsigned v) {
    sem_t *h = malloc(sizeof(sem_t));
    *(sem_t **)s = h;
    return sem_init(h, pshared, v);
}
static int b_sem_destroy(void *s) { return sem_destroy(host_sem(s)); }
static int b_sem_post(void *s) { return sem_post(host_sem(s)); }
static int b_sem_wait(void *s) { return sem_wait_signals(host_sem(s)); }
static int b_sem_trywait(void *s) { return sem_trywait(host_sem(s)); }
static int b_sem_timedwait(void *s, const struct timespec *t) { return sem_timedwait(host_sem(s), t); }
static int b_sem_getvalue(void *s, int *v) { return sem_getvalue(host_sem(s), v); }
#define SEM(name) B(#name, b_##name)
#else
static int b_sem_wait(sem_t *s) { return sem_wait_signals(s); }
#define SEM(name) P(name)
#endif

static int b_clock_gettime(int id, struct timespec *ts) {
    clockid_t real;
    switch (id) {
    case 0: real = CLOCK_REALTIME; break;
    case 2: real = CLOCK_PROCESS_CPUTIME_ID; break;
    case 3: real = CLOCK_THREAD_CPUTIME_ID; break;
    default: real = CLOCK_MONOTONIC; break; // MONOTONIC, RAW, COARSE, BOOTTIME
    }
    return clock_gettime(real, ts);
}

// ---- dynamic linking: the game asks for libraries by name ----------------

static void *b_dlopen(const char *name, int flags) {
    (void)flags;
    droid_log("dlopen(%s)", name ? name : "(self)");
    return (void *)1;
}

static void *b_dlsym(void *handle, const char *name) {
    (void)handle;
    void *p = droid_resolve(name);
    if (!p) droid_log("dlsym(%s): not found", name);
    return p;
}

static int b_dlclose(void *handle) { (void)handle; return 0; }
static const char *b_dlerror(void) { return NULL; }

static int b_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data) {
    (void)cb; (void)data;
    return 0; // C++ exceptions across the game: not yet
}

// ---- abort and logging ---------------------------------------------------

static void b_assert2(const char *file, int line, const char *fn, const char *msg) {
    droid_log("ASSERT %s:%d %s: %s", file, line, fn, msg);
    abort();
}

static void b_set_abort_message(const char *msg) { droid_log("abort message: %s", msg); }

// Memory read that can't fault: through a pipe (a bad address: EFAULT, not a crash)
static int safe_read(uint64_t addr, void *out, size_t n) {
    static int fds[2] = { -1, -1 };
    if (fds[0] < 0 && pipe(fds) != 0) return 0;
#ifdef DROID_PC // the bare call: HeapGuard's (AddressSanitizer's) write() checks the bytes it is to probe
    if (addr < 0x10000 || syscall(SYS_write, fds[1], (const void *)(uintptr_t)addr, n) != (long)n) return 0;
#else
    if (addr < 0x10000 || write(fds[1], (const void *)(uintptr_t)addr, n) != (ssize_t)n) return 0;
#endif
    return read(fds[0], out, n) == (ssize_t)n;
}

// An IL2CPP string (UTF-16 after a 16-byte header and its length) as ASCII
static void il2cpp_string(uint64_t s, char *out, size_t n) {
    int32_t len = 0;
    out[0] = 0;
    if (!s || !safe_read(s + 0x10, &len, 4) || len <= 0) return;
    size_t k = 0;
    for (int32_t i = 0; i < len && k + 1 < n; i++) {
        uint16_t c;
        if (!safe_read(s + 0x14 + 2 * (uint64_t)i, &c, 2)) break;
        out[k++] = c >= 32 && c < 127 ? (char)c : c == 10 ? '|' : '?'; // lines: |
    }
    out[k] = 0;
}

// An IL2CPP object's class name ("" if it doesn't look like one)
static void class_name(uint64_t obj, char *name, size_t n, char *ns, size_t nn) {
    uint64_t klass, name_p, ns_p;
    name[0] = 0;
    if (ns) ns[0] = 0;
    if (!obj || (obj & 7) || !safe_read(obj, &klass, 8) || !klass || (klass & 7)) return;
    if (!safe_read(klass + 0x10, &name_p, 8) || !safe_read(klass + 0x18, &ns_p, 8)) return;
    if (!safe_read(name_p, name, n - 1)) { name[0] = 0; return; }
    name[n - 1] = 0;
    if (ns && safe_read(ns_p, ns, nn - 1)) ns[nn - 1] = 0;
    else if (ns) ns[0] = 0;
}

// Why an IL2CPP game gave up: its C# exceptions are usually still on the ARM stack.
// An object whose class is named "*Exception": its type, its text fields (message,
// parameter name...) and where it was thrown (its native trace: library+offset).
void droid_log_managed_exceptions(void) {
    uint64_t sp, top;
    if (!arm_guest_sp_range(&sp, &top)) return;
    // where to look: the registers, the stack, and inside *EventArgs objects found there
    // (UnhandledExceptionEventArgs holds the exception)
    static uint64_t cand[8192 + 31 + 64];
    int n_cand = 0;
    for (int r = 0; r <= 30; r++) cand[n_cand++] = arm_guest_reg(r);
    for (uint64_t at = sp; at + 8 <= top && at < sp + 64 * 1024; at += 8)
        if (safe_read(at, &cand[n_cand], 8)) n_cand++;
    int base_n = n_cand;
    // a C++ exception being unwound (_Unwind_Exception, 0x20 bytes): IL2CPP's object after
    // it holds the C# exception (Il2CppExceptionWrapper); NULL there is logged
    static int null_logged;
    for (int i = 0; i < 31; i++) { // registers only: the unwinder passes it there
        uint64_t cls8 = 0, ex = 1;
        char cls[9] = { 0 };
        if (!safe_read(cand[i], &cls8, 8)) continue;
        memcpy(cls, &cls8, 8); // exception_class: CLNGC++ and a NUL (clang, C++)
        if (memcmp(cls, "CLNGC++", 7) != 0) continue;
        if (safe_read(cand[i] + 0x20, &ex, 8) && n_cand < (int)(sizeof(cand) / sizeof(cand[0])) - 4) cand[n_cand++] = ex;
        if (ex == 0 && null_logged++ < 10) droid_log("abort: a C++ exception (x%d) whose C# exception is NULL", i);
    }
    for (int i = 0; i < base_n && n_cand < (int)(sizeof(cand) / sizeof(cand[0])) - 4; i++) {
        char nm[96];
        class_name(cand[i], nm, sizeof(nm), NULL, 0);
        size_t l = strnlen(nm, sizeof(nm));
        if (l < 9 || strcmp(nm + l - 9, "EventArgs") != 0) continue;
        for (uint64_t off = 0x10; off <= 0x28; off += 8)
            if (safe_read(cand[i] + off, &cand[n_cand], 8)) n_cand++;
    }
    static uint64_t seen[64]; // each one logged once
    static int n_seen;
    for (int ci = 0; ci < n_cand && n_seen < 64; ci++) {
        uint64_t obj = cand[ci];
        char name[96], ns[96];
        class_name(obj, name, sizeof(name), ns, sizeof(ns));
        size_t len = strnlen(name, sizeof(name));
        if (len < 9 || strcmp(name + len - 9, "Exception") != 0) continue;
        int dup = 0;
        for (int i = 0; i < n_seen; i++) dup |= seen[i] == obj;
        if (dup) continue;
        seen[n_seen++] = obj;
        droid_log("abort: C# %s.%s (at %p)", ns, name, (void *)(uintptr_t)obj);
        // where: the return addresses on the stack now (the throw's callers)
        char chain[1800];
        int cl = 0, shown = 0;
        for (uint64_t at2 = sp; at2 + 8 <= top && at2 < sp + 32 * 1024 && shown < 48; at2 += 8) {
            uint64_t v;
            const char *mn = NULL;
            if (!safe_read(at2, &v, 8)) continue;
            so_module *m = arm_module_at(v, &mn, NULL);
            if (!m || !mn) continue;
            const char *shortn = strncmp(mn, "lib", 3) == 0 ? mn + 3 : mn;
            cl += snprintf(chain + cl, sizeof(chain) - cl, " %.6s+%llx", shortn,
                           (unsigned long long)(v - (uint64_t)(uintptr_t)m->base));
            shown++;
            if (cl >= (int)sizeof(chain) - 40) break;
        }
        if (shown) droid_log("abort:   stack:%s", chain);
        for (uint64_t off = 0x10; off < 0xc0; off += 8) {
            uint64_t f;
            char fname[64];
            if (!safe_read(obj + off, &f, 8)) break;
            class_name(f, fname, sizeof(fname), NULL, 0);
            if (strcmp(fname, "String") == 0) {
                char t[600];
                il2cpp_string(f, t, sizeof(t));
                if (t[0]) droid_log("abort:   +%#llx \"%s\"", (unsigned long long)off, t);
            } else if (strcmp(fname, "IntPtr[]") == 0) { // native_trace_ips
                uint64_t count = 0;
                safe_read(f + 0x18, &count, 8);
                for (uint64_t i = 0; i < count && i < 16; i++) {
                    uint64_t ip;
                    const char *mn = "?";
                    if (!safe_read(f + 0x20 + 8 * i, &ip, 8)) break;
                    so_module *m = arm_module_at(ip, &mn, NULL);
                    droid_log("abort:   trace %llu: %s+%#llx", (unsigned long long)i, mn,
                              (unsigned long long)(m ? ip - (uint64_t)(uintptr_t)m->base : ip));
                }
            } else if (len && fname[0]) {
                droid_log("abort:   +%#llx %s", (unsigned long long)off, fname);
            }
        }
    }
}

static void b_abort(void) {
    droid_log("game called abort()");
    droid_log_managed_exceptions();
    abort();
}

static void b_stack_chk_fail(void) {
    droid_log("stack corruption detected (__stack_chk_fail)");
    abort();
}

static void b_openlog(const char *ident, int opt, int facility) { (void)ident; (void)opt; (void)facility; }
static void b_closelog(void) {}

static void b_syslog(int prio, const char *fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    droid_log("[syslog %d] %s", prio, text);
}

static int b_cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    (void)fn; (void)arg; (void)dso;
    return 0; // the game never unloads
}

static void b_cxa_finalize(void *dso) { (void)dso; }

static void b_sincosf(float x, float *s, float *c) {
    *s = sinf(x);
    *c = cosf(x);
}

static void b_sincos(double x, double *s, double *c) {
    *s = sin(x);
    *c = cos(x);
}

// fread, timed: a slow one (over 30 ms) is logged with its size (the first 40).
static size_t b_fread(void *buf, size_t size, size_t n, FILE *f) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    size_t r = fread(buf, size, n, f);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    static int logged;
    if (ms > 30 && logged < 40) {
        logged++;
        droid_log("fread: %zu x %zu bytes took %.0f ms (fd %d, at %lld)", n, size, ms, fileno(f),
                  (long long)ftello(f));
    }
    return r;
}

// rand: bionic's RAND_MAX is 0x7fffffff, the console's libc's 0x7fff. GTA San Andreas scales
// rand() by 2^-31 and draws again until a point lies far enough: with the console's it never
// did (frame 1 looped forever in MobileMenu::Initialize). Our own 31-bit generator for all.
static uint64_t rand_state = 1;
static int b_rand(void) {
    uint64_t s = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    rand_state = s; // (unlocked, as a phone's: a race only repeats a value)
    return (int)(s >> 33);
}
static void b_srand(unsigned seed) { rand_state = seed; }

// ---- the table -----------------------------------------------------------

#define P(name) { #name, (void *)name }
#define B(name, fn) { name, (void *)fn }
static const struct { const char *name; void *fn; } table[] = {
    // stdio
    B("__sF", bionic_sF), B("stdin", &bionic_std[0]), B("stdout", &bionic_std[1]), B("stderr", &bionic_std[2]), B("fopen", b_fopen), B("fclose", b_fclose), B("fflush", b_fflush),
    B("fprintf", b_fprintf), B("vfprintf", b_vfprintf), B("fputs", b_fputs),
    B("fputc", b_fputc), B("putc", b_fputc), B("fwrite", b_fwrite), B("fscanf", b_fscanf),
    B("fread", b_fread), P(fgets), P(fseek), P(ftell), P(rewind), P(feof), P(ferror), P(fgetc),
    B("fileno", b_fileno), P(fseeko), P(ftello), P(getc), P(ungetc), P(perror), P(putchar),
    P(printf), P(puts), P(sprintf), P(snprintf), P(vsnprintf), P(vsprintf), P(vasprintf),
    P(asprintf), P(sscanf), P(vsscanf), P(remove), P(rename),
    // errno, threads, time
    B("__errno", b_errno), B("pthread_once", b_pthread_once),
    B("clock_gettime", b_clock_gettime),
    P(pthread_create), P(pthread_join), P(pthread_detach), P(pthread_self), P(pthread_equal),
    P(pthread_attr_init), P(pthread_attr_destroy), P(pthread_attr_setdetachstate),
    P(pthread_attr_setstacksize), P(pthread_mutex_init), P(pthread_mutex_destroy),
    B("pthread_mutex_lock", b_mutex_lock), B("pthread_mutex_trylock", b_mutex_trylock),
    B("pthread_mutex_unlock", b_mutex_unlock),
    // mutex / cond attributes and init: linux_abi.c (bionic keeps them as a value)
    SEM(sem_init), SEM(sem_destroy), SEM(sem_post), B("sem_wait", b_sem_wait), SEM(sem_trywait),
    SEM(sem_timedwait), SEM(sem_getvalue),
    P(pthread_cond_init), P(pthread_cond_destroy), B("pthread_cond_wait", b_cond_wait),
    B("pthread_cond_timedwait", b_cond_timedwait), B("pthread_cond_signal", b_cond_signal),
    B("pthread_cond_broadcast", b_cond_broadcast),
    P(pthread_rwlock_init), P(pthread_rwlock_destroy), B("pthread_rwlock_rdlock", b_rwlock_rdlock),
    B("pthread_rwlock_wrlock", b_rwlock_wrlock), B("pthread_rwlock_tryrdlock", b_rwlock_tryrdlock),
    B("pthread_rwlock_trywrlock", b_rwlock_trywrlock), P(pthread_rwlock_unlock), P(pthread_key_create),
    P(pthread_key_delete), P(pthread_getspecific), P(pthread_setspecific),
    P(sched_yield), B("usleep", b_usleep), B("nanosleep", b_nanosleep), P(time), P(gettimeofday),
    B("sleep", b_sleep),
    // dl
    B("dlopen", b_dlopen), B("dlsym", b_dlsym), B("dlclose", b_dlclose),
    B("dlerror", b_dlerror), B("dl_iterate_phdr", b_dl_iterate_phdr),
    // abort, logging, C++ runtime
    B("__assert2", b_assert2), B("android_set_abort_message", b_set_abort_message),
    B("abort", b_abort), B("__stack_chk_fail", b_stack_chk_fail), B("openlog", b_openlog), B("closelog", b_closelog),
    B("syslog", b_syslog), B("__cxa_atexit", b_cxa_atexit), B("__cxa_finalize", b_cxa_finalize),
    // memory and strings
    B("malloc", b_malloc), B("calloc", b_calloc), B("realloc", b_realloc), B("free", b_free),
    B("posix_memalign", b_posix_memalign), B("malloc_usable_size", b_malloc_usable_size),
    P(memcpy), B("memmove", b_memmove), P(memset), P(memcmp), P(memchr), P(strlen), P(strcmp),
    P(strncmp), P(strcpy), P(strncpy), P(strcat), P(strncat), P(strchr), P(strrchr),
    P(strstr), B("strcasestr", droid_strcasestr), P(strdup), P(strtol), P(strtoul), P(strtod), P(strtof), P(atoi), P(atof),
    P(strerror), B("rand", b_rand), B("srand", b_srand), P(qsort), B("getenv", b_getenv), P(exit), P(getpid),
    // files
    P(open), B("close", b_close), B("read", b_read), P(write), P(lseek), B("pipe", b_pipe), P(stat), P(fstat), P(unlink),
    P(mkdir), P(rmdir), P(access), P(chdir), P(chmod), P(getcwd), P(readlink), P(isatty),
    P(poll), P(select), P(munmap), P(setenv),
    // time, strings, ctype, wide chars, locale data
    P(gmtime_r), P(localtime_r), P(strftime), P(strncasecmp), P(strtoll), P(strtoull),
    P(strcoll), P(strxfrm), P(strerror_r), P(localeconv), P(islower), P(isupper), P(isxdigit),
    P(tolower), P(toupper), P(iswalpha), P(iswblank), P(iswcntrl), P(iswdigit), P(iswlower),
    P(iswprint), P(iswpunct), P(iswspace), P(iswupper), P(iswxdigit), P(towlower), P(towupper),
    P(btowc), P(wctob), P(mbtowc), P(wcslen), P(wcscoll), P(wcsxfrm), P(wcstod), P(wcstof),
    P(wcstol), P(wcstoll), P(wcstoul), P(wcstoull), P(wmemchr), P(wmemcmp), P(wmemcpy),
    P(wmemmove), P(wmemset), P(swprintf),
    P(atol), P(difftime), P(div), P(dup), P(dup2), P(fchmod), P(fdopen), P(ftruncate), P(futimens),
    P(getegid), P(geteuid), P(getuid), P(gethostname), P(gmtime), P(localtime), P(mktime),
    P(isalnum), P(isalpha), P(lrand48), P(srand48), P(memrchr), P(realpath), P(strcasecmp),
    P(strcspn), P(strlcpy), P(strnlen), P(strpbrk), P(strspn), P(strtok_r), P(symlink), P(link),
    P(truncate), P(unsetenv), P(utime), P(utimes),
    P(inet_addr),
    B("setvbuf", b_setvbuf), B("setbuf", b_setbuf), B("clearerr", b_clearerr),
    // math
    P(sin), P(cos), P(tan), P(sqrt), P(pow), P(exp), P(log), P(atan2), P(floor), P(ceil),
    P(fmod), P(sinf), P(cosf), P(tanf), P(sqrtf), P(powf), P(expf), P(logf), P(atan2f),
    P(floorf), P(ceilf), P(fmodf), P(fabsf), P(acosf), P(asinf), P(atanf), P(acos), P(asin),
    P(atan), P(cosh), P(sinh), P(tanh), P(exp2), P(exp2f), P(log10), P(log2f), P(ldexp),
    P(ldexpf), P(remainder), P(cbrtf), P(hypot), P(log10f), P(log2), P(logb), P(modf), P(modff),
    P(nearbyintf), P(scalbn),
    B("sincosf", b_sincosf), B("sincos", b_sincos),
};

void *bionic_resolve(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}
