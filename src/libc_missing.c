// libc_missing.c — functions the console's libc doesn't have (their import slots come
// out empty: a call jumps to address 0). The title links with --wrap=<name>, so every
// call in the eboot (ours, Mesa's, the games' through linux_abi.c) comes here.
// The PC test runner's libc has them all: nothing here.

#ifndef DROID_PC

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "android_shim.h"

// No symbolic links to read in a title
ssize_t __wrap_readlink(const char *path, char *buf, size_t n) {
    (void)path; (void)buf; (void)n;
    errno = EINVAL;
    return -1;
}

// No terminals
int __wrap_isatty(int fd) {
    (void)fd;
    errno = ENOTTY;
    return 0;
}

// fnmatch: *, ?, [...] (with ! or ^, ranges), backslash escapes. flags (FreeBSD's numbers,
// the callers' own): FNM_NOESCAPE 1, FNM_PATHNAME 2, FNM_PERIOD 4.
#define F_NOESCAPE 1
#define F_PATHNAME 2
#define F_PERIOD 4

static int match_class(const char **pp, char c) {
    const char *p = *pp;
    int neg = *p == '!' || *p == '^';
    if (neg) p++;
    int hit = 0, first = 1;
    while (*p && (first || *p != ']')) {
        first = 0;
        char lo = *p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            char hi = p[1];
            p += 2;
            if (c >= lo && c <= hi) hit = 1;
        } else if (c == lo) {
            hit = 1;
        }
    }
    if (*p != ']') return -1; // not a class: '[' is literal
    *pp = p + 1;
    return hit != neg;
}

static int fnm(const char *p, const char *s, int flags, const char *start) {
    for (;;) {
        char pc = *p;
        if (!pc) return *s ? 1 : 0;
        if ((flags & F_PERIOD) && *s == '.' && (s == start || ((flags & F_PATHNAME) && s[-1] == '/')) && pc != '.')
            return 1;
        if (pc == '*') {
            while (*p == '*') p++;
            if (!*p) return (flags & F_PATHNAME) && strchr(s, '/') ? 1 : 0;
            for (const char *t = s; *t; t++) {
                if (fnm(p, t, flags & ~F_PERIOD, start) == 0) return 0;
                if ((flags & F_PATHNAME) && *t == '/') return 1;
            }
            return fnm(p, s + strlen(s), flags, start);
        }
        if (!*s) return 1;
        if (pc == '?') {
            if ((flags & F_PATHNAME) && *s == '/') return 1;
            p++;
            s++;
            continue;
        }
        if (pc == '[') {
            const char *q = p + 1;
            int r = match_class(&q, *s);
            if (r >= 0) {
                if (!r) return 1;
                p = q;
                s++;
                continue;
            }
        }
        if (pc == '\\' && !(flags & F_NOESCAPE) && p[1]) pc = *++p;
        if (pc != *s) return 1;
        p++;
        s++;
    }
}

int __wrap_fnmatch(const char *pattern, const char *s, int flags) {
    return fnm(pattern, s, flags, s) == 0 ? 0 : 1; // FNM_NOMATCH: 1
}

// mkstemp: XXXXXX replaced until a new file opens
int __wrap_mkstemp(char *tmpl) {
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint64_t seed = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)tmpl;
    for (int tries = 0; tries < 100; tries++) {
        for (int i = 0; i < 6; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            tmpl[n - 6 + i] = chars[(seed >> 33) % (sizeof(chars) - 1)];
        }
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

// ANGLE's imports the console's libc may not have (whether it does isn't known: ours either way).
// Its shader compiler folds these on constants; the wide-character ones are libc++'s streams.
#include <math.h>
#include <stdio.h>
#include <wchar.h>
float __wrap_coshf(float x) { return (expf(x) + expf(-x)) * 0.5f; }
float __wrap_acoshf(float x) { return logf(x + sqrtf(x * x - 1.0f)); }
float __wrap_asinhf(float x) {
    float a = fabsf(x), r = logf(a + sqrtf(a * a + 1.0f));
    return x < 0 ? -r : r;
}
float __wrap_atanhf(float x) { return 0.5f * logf((1.0f + x) / (1.0f - x)); }
wint_t __wrap_fgetwc(FILE *f) { (void)f; return WEOF; }
wint_t __wrap_fputwc(wchar_t c, FILE *f) { (void)c; (void)f; return WEOF; }
wint_t __wrap_ungetwc(wint_t c, FILE *f) { (void)c; (void)f; return WEOF; }
struct rusage;
int __wrap_getrusage(int who, struct rusage *u) {
    (void)who; (void)u;
    errno = ENOSYS;
    return -1;
}

// No users or groups in a title (ANGLE's C++ library asks for them: libc++'s filesystem)
struct passwd;
struct group;
struct passwd *getpwuid(uid_t uid) {
    (void)uid;
    errno = ENOENT;
    return NULL;
}
struct group *getgrgid(gid_t gid) {
    (void)gid;
    errno = ENOENT;
    return NULL;
}

#endif
