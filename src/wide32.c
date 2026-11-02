// wide32.c — Android's wide-character functions with Android's wchar_t: 32 bits (UTF-32).
// The PS5's own C library has a 16-bit wchar_t (as Windows): its wcslen counted "P\0\0\0h\0\0\0"
// as 1 character, and the Infinity Blade III port's (iOS UE3) first text format ran off into
// its letters as a pointer. Hydroid's few wide answers before (libc_extra.c: wcscmp, wcscpy, ...)
// call the console's 16-bit ones too.
//
// Per game (WIDE32_GAMES, arm_imports.c's arm_import asks here first): every other game keeps
// what it had. Text is UTF-8 on the narrow side (Android's only locale).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "android_shim.h"

typedef uint32_t w32;

// ---- strings -------------------------------------------------------------------------------------
static size_t w_wcslen(const w32 *s) { size_t n = 0; while (s[n]) n++; return n; }
static size_t w_wcsnlen(const w32 *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
static int w_wcscmp(const w32 *a, const w32 *b) {
    while (*a && *a == *b) a++, b++;
    return *a < *b ? -1 : *a > *b;
}
static int w_wcsncmp(const w32 *a, const w32 *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
static w32 *w_wcscpy(w32 *d, const w32 *s) { w32 *r = d; while ((*d++ = *s++)) {} return r; }
static w32 *w_wcsncpy(w32 *d, const w32 *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static w32 *w_wcscat(w32 *d, const w32 *s) { w_wcscpy(d + w_wcslen(d), s); return d; }
static w32 *w_wcsncat(w32 *d, const w32 *s, size_t n) {
    w32 *e = d + w_wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
static w32 *w_wcschr(const w32 *s, w32 c) {
    for (;; s++) {
        if (*s == c) return (w32 *)s;
        if (!*s) return NULL;
    }
}
static w32 *w_wcsrchr(const w32 *s, w32 c) {
    const w32 *last = NULL;
    for (;; s++) {
        if (*s == c) last = s;
        if (!*s) return (w32 *)last;
    }
}
static w32 *w_wcsstr(const w32 *h, const w32 *n) {
    if (!*n) return (w32 *)h;
    size_t nl = w_wcslen(n);
    for (; *h; h++)
        if (*h == *n && !w_wcsncmp(h, n, nl)) return (w32 *)h;
    return NULL;
}
static int w_wcscoll(const w32 *a, const w32 *b) { return w_wcscmp(a, b); }
static int w_wcscoll_l(const w32 *a, const w32 *b, void *loc) { (void)loc; return w_wcscmp(a, b); }
static size_t w_wcsxfrm_l(w32 *d, const w32 *s, size_t n, void *loc) {
    (void)loc;
    size_t len = w_wcslen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c * sizeof(w32));
        d[c] = 0;
    }
    return len;
}

// ---- memory --------------------------------------------------------------------------------------
static w32 *w_wmemchr(const w32 *s, w32 c, size_t n) {
    for (; n; n--, s++)
        if (*s == c) return (w32 *)s;
    return NULL;
}
static int w_wmemcmp(const w32 *a, const w32 *b, size_t n) {
    for (; n; n--, a++, b++)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
static w32 *w_wmemcpy(w32 *d, const w32 *s, size_t n) { memcpy(d, s, n * sizeof(w32)); return d; }
static w32 *w_wmemmove(w32 *d, const w32 *s, size_t n) { memmove(d, s, n * sizeof(w32)); return d; }
static w32 *w_wmemset(w32 *d, w32 c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }

// ---- UTF-8 <-> UTF-32 ----------------------------------------------------------------------------
// one character from s (at most n bytes): its length, 0 for the NUL, -1 bad, -2 incomplete
static long utf8_get(const unsigned char *s, size_t n, w32 *out) {
    if (!n) return -2;
    unsigned c = s[0];
    if (c < 0x80) { *out = c; return c ? 1 : 0; }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!len || c >= 0xF8) return -1;
    if (n < (size_t)len) return -2;
    w32 v = c & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        v = (v << 6) | (s[i] & 0x3F);
    }
    *out = v;
    return len;
}
// c as UTF-8 into out (4 bytes room): its length
static int utf8_put(w32 c, char *out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static int w_mbtowc(w32 *pwc, const char *s, size_t n) {
    if (!s) return 0; // (no shift state)
    w32 c;
    long r = utf8_get((const unsigned char *)s, n, &c);
    if (r < 0) return -1;
    if (pwc) *pwc = c;
    return (int)r;
}
static size_t w_mbrtowc(w32 *pwc, const char *s, size_t n, void *ps) {
    (void)ps;
    if (!s) return 0;
    w32 c;
    long r = utf8_get((const unsigned char *)s, n, &c);
    if (r == -2) return (size_t)-2;
    if (r < 0) return (size_t)-1;
    if (pwc) *pwc = c;
    return (size_t)r;
}
static size_t w_mbsrtowcs(w32 *d, const char **src, size_t n, void *ps) {
    (void)ps;
    const unsigned char *s = (const unsigned char *)*src;
    size_t i = 0;
    while (!d || i < n) {
        w32 c;
        long r = utf8_get(s, 4, &c);
        if (r < 0) { *src = (const char *)s; return (size_t)-1; }
        if (d) d[i] = c;
        if (!r) { if (d) *src = NULL; return i; }
        s += r, i++;
    }
    *src = (const char *)s;
    return i;
}
static size_t w_mbstowcs(w32 *d, const char *s, size_t n) { const char *p = s; return w_mbsrtowcs(d, &p, n, NULL); }
static size_t w_wcrtomb(char *s, w32 c, void *ps) {
    (void)ps;
    char tmp[4];
    if (!s) return 1;
    int len = utf8_put(c, tmp);
    memcpy(s, tmp, (size_t)len);
    return (size_t)len;
}
// at most nwc characters of *src (all: (size_t)-1), at most n bytes into d (or counted)
static size_t w_wcsnrtombs(char *d, const w32 **src, size_t nwc, size_t n, void *ps) {
    (void)ps;
    const w32 *s = *src;
    size_t out = 0;
    for (size_t k = 0; k < nwc; k++, s++) {
        char tmp[4];
        int len = utf8_put(*s, tmp);
        if (!*s) {
            if (d && out < n) d[out] = 0;
            if (d) *src = NULL;
            return out;
        }
        if (d && out + (size_t)len > n) break;
        if (d) memcpy(d + out, tmp, (size_t)len);
        out += (size_t)len;
    }
    if (d) *src = s;
    return out;
}
static size_t w_wcsrtombs(char *d, const w32 **src, size_t n, void *ps) { return w_wcsnrtombs(d, src, (size_t)-1, n, ps); }
static size_t w_wcstombs(char *d, const w32 *s, size_t n) { const w32 *p = s; return w_wcsnrtombs(d, &p, (size_t)-1, n, NULL); }
static int w_btowc(int c) { return c >= 0 && c < 0x80 ? c : -1; } // (UTF-8: a byte above 0x7F is no character)
static int w_wctob(int c) { return c >= 0 && c < 0x80 ? c : -1; }

// ---- numbers: the ASCII start as narrow text, then the C library's own --------------------------
static size_t narrow_prefix(const w32 *s, char *buf, size_t room) {
    size_t i = 0;
    for (; i + 1 < room && s[i] && s[i] < 0x80; i++) buf[i] = (char)s[i];
    buf[i] = 0;
    return i;
}
#define W_STRTO(name, type, call)                                                                    \
    static type w_##name(const w32 *s, w32 **end, int base) {                                       \
        char buf[256], *e;                                                                           \
        narrow_prefix(s, buf, sizeof(buf));                                                          \
        type v = call(buf, &e, base);                                                                \
        if (end) *end = (w32 *)(s + (e - buf));                                                      \
        return v;                                                                                    \
    }
W_STRTO(wcstol, long, strtol)
W_STRTO(wcstoll, long long, strtoll)
W_STRTO(wcstoul, unsigned long, strtoul)
W_STRTO(wcstoull, unsigned long long, strtoull)
static double w_wcstod(const w32 *s, w32 **end) {
    char buf[256], *e;
    narrow_prefix(s, buf, sizeof(buf));
    double v = strtod(buf, &e);
    if (end) *end = (w32 *)(s + (e - buf));
    return v;
}
static float w_wcstof(const w32 *s, w32 **end) {
    char buf[256], *e;
    narrow_prefix(s, buf, sizeof(buf));
    float v = strtof(buf, &e);
    if (end) *end = (w32 *)(s + (e - buf));
    return v;
}

// ---- the table ---------------------------------------------------------------------------------
#define WIDE32_GAMES "com.ib3port.,com.ib2port.,com.ib1port." // the Infinity Blade ports (iOS UE3: wide text everywhere)
int droid_game_in(const char *list);
static const struct { const char *name; void *fn; } wide[] = {
    { "wcslen", (void *)w_wcslen }, { "wcsnlen", (void *)w_wcsnlen }, { "wcscmp", (void *)w_wcscmp },
    { "wcsncmp", (void *)w_wcsncmp }, { "wcscpy", (void *)w_wcscpy }, { "wcsncpy", (void *)w_wcsncpy },
    { "wcscat", (void *)w_wcscat }, { "wcsncat", (void *)w_wcsncat }, { "wcschr", (void *)w_wcschr },
    { "wcsrchr", (void *)w_wcsrchr }, { "wcsstr", (void *)w_wcsstr }, { "wcscoll", (void *)w_wcscoll },
    { "wcscoll_l", (void *)w_wcscoll_l }, { "wcsxfrm_l", (void *)w_wcsxfrm_l },
    { "wmemchr", (void *)w_wmemchr }, { "wmemcmp", (void *)w_wmemcmp }, { "wmemcpy", (void *)w_wmemcpy },
    { "wmemmove", (void *)w_wmemmove }, { "wmemset", (void *)w_wmemset },
    { "mbtowc", (void *)w_mbtowc }, { "mbrtowc", (void *)w_mbrtowc }, { "mbsrtowcs", (void *)w_mbsrtowcs },
    { "mbstowcs", (void *)w_mbstowcs }, { "wcrtomb", (void *)w_wcrtomb }, { "wcsnrtombs", (void *)w_wcsnrtombs },
    { "wcsrtombs", (void *)w_wcsrtombs }, { "wcstombs", (void *)w_wcstombs },
    { "btowc", (void *)w_btowc }, { "wctob", (void *)w_wctob },
    { "wcstol", (void *)w_wcstol }, { "wcstoll", (void *)w_wcstoll }, { "wcstoul", (void *)w_wcstoul },
    { "wcstoull", (void *)w_wcstoull }, { "wcstod", (void *)w_wcstod }, { "wcstof", (void *)w_wcstof },
};
void *wide32_resolve(const char *name) {
    static int on = -1;
    if (on < 0) {
        on = droid_game_in(WIDE32_GAMES) != 0;
        if (on) droid_log("wide32: the wide-character functions with a 32-bit wchar_t (WIDE32_GAMES)");
    }
    if (!on) return NULL;
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
        if (!strcmp(wide[i].name, name)) return wide[i].fn;
    return NULL;
}
int wide32_on(void) { return wide32_resolve("wcslen") != NULL; }
