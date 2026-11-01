// arm_trace.c — XTool, the debug tool: chosen functions of a game's own code, logged as they are
// called (their arguments) and as they return (their result). For bugs only one machine
// shows: the same list run on the PS5 and the PC, the logs compared.
//
// The list: xtool.txt in the game's folder (or APP0/data/xtool.txt for every game), a line
// per function:
//     libil2cpp.so+0x359897c SaveHash b s -> s 40
// library+offset (as Il2CppDumper's RVA or our logs give it) or library:symbol (an exported
// function, e.g. libil2cpp.so:il2cpp_raise_exception), a name for the log, one letter per
// argument register from x0 on, then "-> letter" for the result (optional; "-> i =1" also
// FORCES the result: the function runs, the caller gets 1), and how many calls to log
// (optional, 20). Letters: x hex, i int, l long, f float bits, s C# string, b C# byte[],
// B byte[] + our own MD5 of it, S the float register s/d at that place (a float argument), e C# exception (its type and message), c C string, m 64 bytes
// at the pointer (hex), g a 16-bit text (Rockstar's GXT), _ skip. "bt" on the line: each logged
// call's chain of callers too; "new": a call logged only when its line differs from the last one
// logged (a text drawn every frame); "if=word": only calls whose line has the word (no spaces in
// it: "if=hood"); "at=0x330": m shows 64 bytes at the register + 0x330. A library+offset inside a function traps there: x8-x30 by
// letter too (31 letters at most), named in the log.
// Every 10 s a line counts the calls of each listed function (past its log count as well).
// ("#" starts a comment.) xtool-names.txt beside it (tools/xtool-make.py names) gives C#
// names to the "from" of every call: "from LedgerManager.CheckHandler+0x48".
//
// How: the function's first instruction becomes "svc #0xF000+n" when its library loads. At
// the trap the arguments are logged, the instruction it replaced is carried out here (the
// usual first ones: stp/str/sub/add on sp, mov, hints), and the function runs on. For its
// result, the return address is pointed at a stub (svc #0xFFFD; br x16) that logs x0 and
// goes back. (A C++ exception through a traced function can't unwind past that stub: trace
// results only where none passes.)

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "android_shim.h"
#include "game_settings.h"

void *droid_jit_alloc(size_t size); // arm_cpu.cpp: memory the JIT runs as ARM code
uint64_t arm_guest_vreg(int i);      // arm_cpu.cpp: a vector register's low 64 bits (the S letter)
void *arm_module_at(uint64_t addr, const char **name, const char **path); // arm_modules.c

#define MAX_HOOKS 256
#define HOOK_SVC 0xF000
#define RET_SVC 0xFFFD

typedef struct {
    char lib[96], sym[96], name[64], args[32], ret; // args: x0 on, all 31 (a trap inside a function)
    uint64_t off, entry, force;
    uint32_t orig;
    int max, calls, ready, forced, backtrace, counted;
    int only_new, logged; // "new" on the line: a call logged only when it shows something new
    char only_if[32];     // "if=word": a call logged only when its line has the word in it
    uint64_t mem_at;      // "at=0x330": m shows the 64 bytes at register + this (a field deep in an object)
    unsigned last_shown; // (a hash of the last logged line)
} hook;

static hook hooks[MAX_HOOKS];
static int n_hooks, loaded;
static uint32_t *ret_stub;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// ---- reading the game's memory without faulting -----------------------------------------
static __thread int pfd[2] = { -1, -1 };
static int peek(uint64_t a, void *out, size_t n) {
    if (pfd[0] < 0 && pipe(pfd) != 0) return 0;
    if (a < 0x10000 || n > 4096 || write(pfd[1], (const void *)(uintptr_t)a, n) != (ssize_t)n) return 0;
    return read(pfd[0], out, n) == (ssize_t)n;
}


// MD5, ours (for "B": a byte[] with its MD5 worked out here, to check a game's own)
static void md5(const uint8_t *msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t b[64];
        for (int i = 0; i < 64; i++) {
            size_t q = off + i;
            b[i] = q < len ? msg[q] : q == len ? 0x80 : 0;
        }
        if (off + 64 == total) {
            uint64_t bits = (uint64_t)len * 8;
            for (int i = 0; i < 8; i++) b[56 + i] = (uint8_t)(bits >> (8 * i));
        }
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
        uint32_t A = h[0], B = h[1], C = h[2], D = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) F = (B & C) | (~B & D), g = i;
            else if (i < 32) F = (D & B) | (~D & C), g = (5 * i + 1) % 16;
            else if (i < 48) F = B ^ C ^ D, g = (3 * i + 5) % 16;
            else F = C ^ (B | ~D), g = (7 * i) % 16;
            uint32_t t = D;
            D = C;
            C = B;
            uint32_t x = A + F + K[i] + w[g];
            B = B + ((x << R[i]) | (x >> (32 - R[i])));
            A = t;
        }
        h[0] += A, h[1] += B, h[2] += C, h[3] += D;
    }
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
}

// "name lib+0x..." in the list: the game's UnityEngine.Object.get_name (Il2CppDumper's RVA); the
// letter n then shows a Unity object by its name (the game's own get_name, called here)
static char name_fn_lib[96];
static uint64_t name_fn_off, name_fn_at;
uint64_t arm_call(uint64_t fn, const uint64_t *args, int count, int *ok); // arm_cpu.cpp
static __thread uint64_t show_mem_at; // the hook being shown: its at=
static void show(char kind, uint64_t v, char *out, size_t n) {
    switch (kind) {
    case 'i': snprintf(out, n, "%d", (int)v); return;
    case 'l': snprintf(out, n, "%lld", (long long)v); return;
    case 'f': { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); snprintf(out, n, "%g", f); return; }
    case 's': { // System.String: length at +0x10, UTF-16 from +0x14
        int32_t len;
        if (!v) { snprintf(out, n, "null"); return; }
        if (!peek(v + 0x10, &len, 4) || len < 0) { snprintf(out, n, "<bad string %#llx>", (unsigned long long)v); return; }
        size_t at = 0;
        out[at++] = '"';
        for (int32_t i = 0; i < len && at + 6 < n; i++) {
            uint16_t c;
            if (!peek(v + 0x14 + 2 * (uint64_t)i, &c, 2)) break;
            out[at++] = c >= 32 && c < 127 ? (char)c : '?';
        }
        if (len > 0 && at + 6 >= n) at += snprintf(out + at, n - at, "...");
        snprintf(out + at, n - at, "\" (%d)", len);
        return;
    }
    case 'b': { // byte[]: length at +0x18, bytes from +0x20
        uint64_t len;
        if (!v) { snprintf(out, n, "null"); return; }
        if (!peek(v + 0x18, &len, 8) || len > (1ull << 31)) { snprintf(out, n, "<bad array %#llx>", (unsigned long long)v); return; }
        unsigned char b[24];
        size_t show_n = len < sizeof(b) ? (size_t)len : sizeof(b);
        int at = snprintf(out, n, "%llu bytes:", (unsigned long long)len);
        if (show_n && peek(v + 0x20, b, show_n))
            for (size_t i = 0; i < show_n && at + 4 < (int)n; i++) at += snprintf(out + at, n - at, " %02x", b[i]);
        return;
    }
    case 'B': { // byte[] and its MD5, worked out here
        uint64_t len;
        if (!v) { snprintf(out, n, "null"); return; }
        if (!peek(v + 0x18, &len, 8) || len > (64ull << 20)) { snprintf(out, n, "<bad array %#llx>", (unsigned long long)v); return; }
        uint8_t *all = malloc(len ? len : 1), d[16];
        size_t got = 0;
        while (all && got < len) { // in 4 KB steps (peek's most)
            size_t step = len - got < 4096 ? (size_t)(len - got) : 4096;
            if (!peek(v + 0x20 + got, all + got, step)) break;
            got += step;
        }
        if (!all || got < len) { free(all); snprintf(out, n, "%llu bytes (unreadable)", (unsigned long long)len); return; }
        md5(all, (size_t)len, d);
        int at = snprintf(out, n, "%llu bytes, our MD5 ", (unsigned long long)len);
        for (int i = 0; i < 16 && at + 3 < (int)n; i++) at += snprintf(out + at, n - at, "%02x", d[i]);
        if (len) at += snprintf(out + at, n - at, ", from");
        for (size_t i = 0; i < len && i < 12 && at + 4 < (int)n; i++) at += snprintf(out + at, n - at, " %02x", all[i]);
        free(all);
        return;
    }
    case 'n': { // a Unity object: its name, by the game's own Object.get_name(obj)
        if (!v) { snprintf(out, n, "null"); return; }
        if (!name_fn_at) { snprintf(out, n, "%#llx (no name line)", (unsigned long long)v); return; }
        const uint64_t args[2] = { v, 0 };
        int ok = 0;
        const uint64_t str = arm_call(name_fn_at, args, 2, &ok);
        char nm[120] = "";
        if (ok) show('s', str, nm, sizeof(nm));
        snprintf(out, n, "%#llx %s", (unsigned long long)v, ok ? nm : "(get_name failed)");
        return;
    }
    case 'e': { // Il2CppException: its class (klass->name, ->namespaze) and message (+0x18)
        uint64_t klass = 0, np = 0, nsp = 0, msg = 0;
        char cn[64] = "?", ns[64] = "", m[200] = "";
        if (!v) { snprintf(out, n, "null"); return; }
        if (peek(v, &klass, 8) && klass && peek(klass + 0x10, &np, 8) && peek(klass + 0x18, &nsp, 8)) {
            for (int i = 0; i < 63 && peek(np + i, &cn[i], 1) && cn[i]; i++) cn[i + 1] = 0;
            for (int i = 0; i < 63 && peek(nsp + i, &ns[i], 1) && ns[i]; i++) ns[i + 1] = 0;
        }
        if (peek(v + 0x18, &msg, 8) && msg) show('s', msg, m, sizeof(m));
        snprintf(out, n, "%s%s%s %s", ns, ns[0] ? "." : "", cn, m[0] ? m : "(no message)");
        return;
    }
    case 'm': { // 64 bytes at the pointer (+ the line's at=)
        v += show_mem_at;
        unsigned char b[64];
        if (!v || !peek(v, b, sizeof(b))) { snprintf(out, n, "%#llx (unreadable)", (unsigned long long)v); return; }
        int at = snprintf(out, n, "%#llx:", (unsigned long long)v);
        for (size_t i = 0; i < sizeof(b) && at + 4 < (int)n; i++) at += snprintf(out + at, n - at, "%s%02x", i % 16 ? " " : " | ", b[i]);
        return;
    }
    case 'g': { // a 16-bit text to its 0 (Rockstar's GXT strings: GxtChar*), '~' tokens as they are
        if (!v) { snprintf(out, n, "null"); return; }
        size_t at = 0;
        out[at++] = '"';
        for (uint64_t i = 0; at + 6 < n; i++) {
            uint16_t c;
            if (!peek(v + 2 * i, &c, 2)) { at += snprintf(out + at, n - at, "<unreadable>"); break; }
            if (!c) break;
            out[at++] = c >= 32 && c < 127 ? (char)c : '?';
        }
        snprintf(out + at, n - at, "\"");
        return;
    }
    case 'c': {
        if (!v) { snprintf(out, n, "null"); return; }
        size_t at = 0;
        out[at++] = '"';
        for (; at + 3 < n; at++) {
            char c;
            if (!peek(v + at - 1, &c, 1) || !c) break;
            out[at] = isprint((unsigned char)c) ? c : '?';
        }
        snprintf(out + at, n - at, "\"");
        return;
    }
    default: snprintf(out, n, "%#llx", (unsigned long long)v); return;
    }
}

// ---- the list ---------------------------------------------------------------------------
static void read_list_file(FILE *f);
static void read_list(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    read_list_file(f);
}

// RockTrace (tools.txt): Rockstar's ports (oswrapper: GTA San Andreas 2.11, the same engine as
// Vice City / III / LCS) watched without writing a list - its texts as they are made, joined and
// drawn (GXT, letter g), its help messages with their callers, its file opens and writes. For the
// text bugs a game shows on screen (a token lost on the way: GTA's "~w" crash) and its files
static const char rocktrace_list[] =
    "libGame.so:_ZN5CText3GetEPKc TextGet x c -> g 60\n"
    "libGame.so:_ZN4CHud14SetHelpMessageEPKcPsbbbj HelpMessage c g 40 bt\n"
    "libGame.so:_ZN15CWidgetHelpText17AddMessageToQueueEPsbtffj HelpQueue x g 40 bt\n"
    "libGame.so:_ZN9CMessages10AddMessageEPKcPsjtb AddMessage c g 40\n"
    "libGame.so:_ZN9CMessages20InsertStringInStringEPsS0_ InsertString g g 40\n"
    "libGame.so:_ZN9CMessages20InsertNumberInStringEPsiiiiiiS0_ InsertNumber g i i i i i i x 40\n"
    "libGame.so:_Z13GxtCharStrcpyPsPKs GxtStrcpy x g 40\n"
    "libGame.so:_Z14AsciiToGxtCharPKcPs AsciiToGxt c x 40\n"
    "libGame.so:_ZN5CFont11PrintStringEffPs PrintString g 200 new\n"
    "libGame.so:_ZN5CFont20ProcessCurrentStringEhffPs DrawText i g 40\n"
    "libGame.so:_Z11OS_FileOpen14OSFileDataAreaPPvPKc16OSFileAccessType FileOpen i x c i -> i 80\n"
    "libGame.so:_ZN8CFileMgr8OpenFileEPKcS1_ FileMgrOpen c c -> x 80\n"
    "libGame.so:_Z12OS_FileWritePvPKvi FileWrite x x i -> i 20\n";

// one line of a list ("#" a comment)
static void parse_line(char *line) {
    if (n_hooks >= MAX_HOOKS) return;
    {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char where[160], name[64] = "", rest[160] = "";
        if (sscanf(line, "%159s %63s %159[^\n]", where, name, rest) < 1) return;
        if (!strcmp(where, "name") && strchr(name, '+')) { // name libil2cpp.so+0x267F74C
            char *pl = strchr(name, '+');
            *pl = 0;
            snprintf(name_fn_lib, sizeof(name_fn_lib), "%s", name);
            name_fn_off = strtoull(pl + 1, NULL, 16);
            return;
        }
        char *plus = strchr(where, '+'), *colon = strstr(where, ".so:"), sym[96] = "";
        if (!plus && colon) { // library:symbol
            snprintf(sym, sizeof(sym), "%s", colon + 4);
            colon[3] = 0;
        } else if (!plus) {
            return;
        } else {
            *plus = 0;
        }
        uint64_t off = plus ? strtoull(plus + 1, NULL, 16) : 0;
        int dup = 0; // listed twice (the game's list and the one for every game)
        for (int i = 0; i < n_hooks; i++)
            if (hooks[i].off == off && !strcmp(hooks[i].lib, where) && !strcmp(hooks[i].sym, sym)) dup = 1;
        if (dup) return;
        hook *h = &hooks[n_hooks];
        memset(h, 0, sizeof(*h));
        snprintf(h->lib, sizeof(h->lib), "%s", where);
        snprintf(h->sym, sizeof(h->sym), "%s", sym);
        h->off = off;
        snprintf(h->name, sizeof(h->name), "%s", name[0] ? name : sym[0] ? sym : plus + 1);
        h->max = 20;
        int after_arrow = 0, a = 0;
        for (char *t = strtok(rest, " \t\r"); t; t = strtok(NULL, " \t\r")) {
            if (!strcmp(t, "->")) after_arrow = 1;
            else if (!strcmp(t, "bt")) h->backtrace = 1;
            else if (!strcmp(t, "new")) h->only_new = 1;
            else if (!strncmp(t, "at=", 3)) h->mem_at = strtoull(t + 3, NULL, 0);
            else if (!strncmp(t, "if=", 3)) {
                snprintf(h->only_if, sizeof(h->only_if), "%s", t + 3);
                h->only_new = 1; // (counted by what is logged, as "new")
            }
            else if (t[0] == '=' && after_arrow) { // a result to force
                h->force = strtoull(t + 1, NULL, 0);
                h->forced = 1;
            } else if (isdigit((unsigned char)t[0])) h->max = atoi(t);
            else if (after_arrow) h->ret = t[0];
            else if (a < (int)sizeof(h->args) - 1) h->args[a++] = t[0];
        }
        n_hooks++;
    }
}

static void read_list_file(FILE *f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) parse_line(line);
    fclose(f);
}

// a list in memory, line by line (the console's libc has no fmemopen)
static void read_list_text(const char *text) {
    while (*text) {
        char line[256];
        size_t n = strcspn(text, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) - 1 ? n : sizeof(line) - 1), text);
        parse_line(line);
        text += n + (text[n] == '\n');
    }
}

// C# names for addresses: tools/xtool-make.py names <script.json> > xtool-names.txt
typedef struct { uint64_t off; const char *name; } named;
static named *names;
static size_t n_names;
static char names_lib[96];
static void load_names(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t cap = 0;
    char line[400];
    while (fgets(line, sizeof(line), f)) {
        char lib[96], nm[300];
        unsigned long long off;
        if (sscanf(line, "%95s %llx %299[^\n]", lib, &off, nm) != 3) continue;
        if (!names_lib[0]) snprintf(names_lib, sizeof(names_lib), "%s", lib);
        if (n_names == cap) {
            cap = cap ? cap * 2 : 65536;
            named *nn = realloc(names, cap * sizeof(named));
            if (!nn) break;
            names = nn;
        }
        names[n_names].off = off;
        names[n_names].name = strdup(nm);
        n_names++;
    }
    fclose(f);
    if (n_names) droid_log("xtool: %zu C# names for %s", n_names, names_lib);
}
// "Class.Method+0x48" for an address in the named library (NULL: no name)
static const char *name_of(const char *lib, uint64_t off, char *out, size_t n) {
    if (!n_names || strcmp(lib, names_lib)) return NULL;
    size_t lo = 0, hi = n_names;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (names[mid].off <= off) lo = mid;
        else hi = mid;
    }
    if (names[lo].off > off || off - names[lo].off > 0x100000) return NULL;
    snprintf(out, n, "%s+%#llx", names[lo].name, (unsigned long long)(off - names[lo].off));
    return out;
}

static void place(uint64_t a, char *out, size_t n) {
    const char *lib = NULL;
    void *m = arm_module_at(a, &lib, NULL);
    char cs[200];
    if (m && name_of(lib, a - *(uint64_t *)m, cs, sizeof(cs))) snprintf(out, n, "%s", cs);
    else if (m) snprintf(out, n, "%s+%#llx", lib, (unsigned long long)(a - *(uint64_t *)m));
    else snprintf(out, n, "%#llx", (unsigned long long)a);
}

// the calls of every listed function, counted, a line every 10 s (what keeps running)
static void count_line(void) {
    static int64_t last;
    static pthread_mutex_t cl = PTHREAD_MUTEX_INITIALIZER;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = (int64_t)t.tv_sec;
    if (!last) last = now;
    if (now - last < 10 || pthread_mutex_trylock(&cl) != 0) return;
    last = now;
    char line[1000];
    int at = snprintf(line, sizeof(line), "xtool: calls in the last 10 s:"), any = 0;
    for (int i = 0; i < n_hooks && at < (int)sizeof(line) - 80; i++) {
        int c = __atomic_load_n(&hooks[i].calls, __ATOMIC_RELAXED), d = c - hooks[i].counted;
        hooks[i].counted = c;
        if (d > 0) {
            at += snprintf(line + at, sizeof(line) - at, " %s %d,", hooks[i].name, d);
            any = 1;
        }
    }
    if (any) droid_log("%s", line);
    pthread_mutex_unlock(&cl);
}

static void load_lists(void) {
    if (loaded) return;
    loaded = 1;
    char p[400];
    read_list(game_path(p, sizeof(p), "xtool.txt"));
    read_list(APP0 "/data/xtool.txt");
    if (droid_tool("RockTrace")) { // the built-in Rockstar list (above)
        read_list_text(rocktrace_list);
        droid_log("xtool: RockTrace on (Rockstar's texts, help messages, files)");
    }
    if (n_hooks) load_names(game_path(p, sizeof(p), "xtool-names.txt"));
    if (n_hooks) droid_log("xtool: %d function(s) listed", n_hooks);
}

// the first instructions we can carry out ourselves
static int emulable(uint32_t e) {
    return (e & 0xFFFFF01F) == 0xD503201F    // hint: nop, bti, paciasp...
        || (e & 0xFFC003E0) == 0xA98003E0    // stp x, x, [sp, #i]!
        || (e & 0xFFC003E0) == 0xA90003E0    // stp x, x, [sp, #i]
        || (e & 0xFFE00FE0) == 0xF8000FE0    // str x, [sp, #i]!
        || (e & 0xFF8003FF) == 0xD10003FF    // sub sp, sp, #i
        || (e & 0xFF800000) == 0x91000000    // add xd, xn|sp, #i (mov xd, sp; a mid-function trap's base + offset)
        || (e & 0xFFE0FFE0) == 0xAA0003E0;   // mov xd, xm (orr xd, xzr, xm)
}

// a library loaded (its constructors not run yet): its listed functions get their traps
void *so_sym(void *mod, const char *name); // soloader.c
void arm_trace_module(const char *lib, uint64_t base, void *mod) {
    pthread_mutex_lock(&lock);
    load_lists();
    if (name_fn_lib[0] && !strcmp(name_fn_lib, lib)) name_fn_at = base + name_fn_off;
    for (int i = 0; i < n_hooks; i++) {
        hook *h = &hooks[i];
        if (h->ready || strcmp(h->lib, lib)) continue;
        if (!ret_stub && (ret_stub = droid_jit_alloc(16))) {
            ret_stub[0] = 0xD4000001u | (RET_SVC << 5); // svc #0xFFFD
            ret_stub[1] = 0xD61F0200u;                   // br x16
        }
        if (h->sym[0]) { // an exported function
            uint64_t at = (uint64_t)(uintptr_t)so_sym(mod, h->sym);
            if (!at) {
                droid_log("xtool: %s has no %s: left out", lib, h->sym);
                h->ready = -1;
                continue;
            }
            h->off = at - base;
        }
        h->entry = base + h->off;
        memcpy(&h->orig, (void *)(uintptr_t)h->entry, 4);
        if (!emulable(h->orig)) {
            droid_log("xtool: %s (%s+%#llx) starts with %08x, an instruction the tool can't step: left out",
                      h->name, lib, (unsigned long long)h->off, h->orig);
            h->ready = -1;
            continue;
        }
        uint64_t page = h->entry & ~(uint64_t)0xFFF;
        mprotect((void *)(uintptr_t)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        uint32_t trap = 0xD4000001u | ((uint32_t)(HOOK_SVC + i) << 5);
        memcpy((void *)(uintptr_t)h->entry, &trap, 4);
        h->ready = 1;
        droid_log("xtool: %s at %s+%#llx (args %s%s%c)", h->name, lib, (unsigned long long)h->off,
                  h->args[0] ? h->args : "-", h->ret ? ", result " : "", h->ret ? h->ret : ' ');
    }
    pthread_mutex_unlock(&lock);
}

// ---- at the traps -----------------------------------------------------------------------
static void step(uint32_t e, uint64_t *x, uint64_t *sp) {
    // x[0..30]; register 31 is sp for these forms' base, xzr for stored values
#define XR(r) ((r) == 31 ? 0 : x[r])
    if ((e & 0xFFFFF01F) == 0xD503201F) return;
    if ((e & 0xFFC00000) == 0xA9800000 || (e & 0xFFC00000) == 0xA9000000) { // stp
        int64_t imm = ((int64_t)(int32_t)(((e >> 15) & 0x7F) << 25) >> 25) * 8;
        uint64_t a = *sp + imm, v[2] = { XR(e & 31), XR((e >> 10) & 31) };
        memcpy((void *)(uintptr_t)a, v, 16);
        if ((e & 0xFFC00000) == 0xA9800000) *sp = a;
        return;
    }
    if ((e & 0xFFE00C00) == 0xF8000C00) { // str pre-index
        int64_t imm = (int64_t)(int32_t)(((e >> 12) & 0x1FF) << 23) >> 23;
        uint64_t a = *sp + imm, v = XR(e & 31);
        memcpy((void *)(uintptr_t)a, &v, 8);
        *sp = a;
        return;
    }
    if ((e & 0xFF8003FF) == 0xD10003FF) { // sub sp, sp, #imm{, lsl 12}
        uint64_t imm = (e >> 10) & 0xFFF;
        if (e & (1u << 22)) imm <<= 12;
        *sp -= imm;
        return;
    }
    if ((e & 0xFF800000) == 0x91000000) { // add xd, xn|sp, #imm{, lsl 12} (31: sp both sides)
        uint64_t imm = (e >> 10) & 0xFFF;
        if (e & (1u << 22)) imm <<= 12;
        int d = e & 31, n = (e >> 5) & 31;
        uint64_t v = (n == 31 ? *sp : x[n]) + imm;
        if (d == 31) *sp = v;
        else x[d] = v;
        return;
    }
    if ((e & 0xFFE0FFE0) == 0xAA0003E0) { // mov xd, xm
        int d = e & 31;
        if (d != 31) x[d] = XR((e >> 16) & 31);
        return;
    }
#undef XR
}

static __thread struct { uint64_t lr; int id; } rstack[64];
static __thread int rdepth;

// svc #0xF000+id: log, step the replaced instruction, on into the function
int arm_trace_enter(int id, uint64_t *x, uint64_t *sp) {
    if (id < 0 || id >= n_hooks || hooks[id].ready != 1) return 0;
    hook *h = &hooks[id];
    int n = __atomic_fetch_add(&h->calls, 1, __ATOMIC_RELAXED);
    if (h->forced && n >= h->max && ret_stub && rdepth < 64) { // quiet now, but still forced
        rstack[rdepth].lr = x[30];
        rstack[rdepth].id = id;
        rdepth++;
        x[30] = (uint64_t)(uintptr_t)ret_stub;
    } else if (h->only_new ? h->logged < h->max : n < h->max) {
        char line[1200];
        int at = snprintf(line, sizeof(line), "xtool: %s(", h->name);
        for (int a = 0; h->args[a] && a < 31; a++) {
            if (h->args[a] == '_') continue;
            char v[300];
            char reg[8] = "";
            if (h->args[a] == 'S') { // the float register at this place: s<a> and d<a>
                uint64_t u = arm_guest_vreg(a);
                float f;
                double d;
                uint32_t lo = (uint32_t)u;
                memcpy(&f, &lo, 4);
                memcpy(&d, &u, 8);
                snprintf(reg, sizeof(reg), "s%d=", a);
                snprintf(v, sizeof(v), "%g (d %g)", f, d);
            } else {
                show_mem_at = h->mem_at;
                show(h->args[a], x[a], v, sizeof(v));
                show_mem_at = 0;
                // past x7 the register is named (a trap inside a function: its live registers)
                if (a > 7) snprintf(reg, sizeof(reg), "x%d=", a);
            }
            at += snprintf(line + at, sizeof(line) - at, "%s%s%s", at > (int)strlen(h->name) + 8 ? ", " : "", reg, v);
        }
        if (h->only_new) { // the same as the last logged call (a text drawn every frame): quiet
            unsigned hash = 2166136261u;
            for (const char *c = line; *c; c++) hash = (hash ^ (unsigned char)*c) * 16777619u;
            if (hash == h->last_shown || (h->only_if[0] && !strstr(line, h->only_if))) {
                step(h->orig, x, sp);
                count_line();
                return 1;
            }
            h->last_shown = hash;
            n = h->logged++;
        }
        // where it was called from (the return address: Class.Method+off or lib+off)
        char from[220];
        place(x[30], from, sizeof(from));
        snprintf(line + at, sizeof(line) - at, ") from %s%s", from, n + 1 == h->max ? " (the last one logged)" : "");
        droid_log("%s", line);
        if (h->backtrace) { // the callers' callers: the frame-pointer chain (x29: saved x29, x30)
            uint64_t fp = x[29];
            for (int depth = 1; depth <= 8 && fp; depth++) {
                uint64_t fr[2];
                if (!peek(fp, fr, 16) || !fr[1]) break;
                char w[220];
                place(fr[1], w, sizeof(w));
                droid_log("xtool:   %d. %s", depth, w);
                if (fr[0] <= fp) break; // the chain goes up the stack
                fp = fr[0];
            }
        }
        if ((h->ret || h->forced) && ret_stub && rdepth < 64) { // its result: back through our stub
            rstack[rdepth].lr = x[30];
            rstack[rdepth].id = id;
            rdepth++;
            x[30] = (uint64_t)(uintptr_t)ret_stub;
        }
    }
    step(h->orig, x, sp);
    count_line();
    return 1;
}

// svc #0xFFFD (the stub): log the result, x16 = where the function was called from
int arm_trace_return(uint64_t *x) {
    if (rdepth <= 0) return 0;
    rdepth--;
    hook *h = &hooks[rstack[rdepth].id];
    char v[300];
    show(h->ret ? h->ret : 'x', x[0], v, sizeof(v));
    if (h->forced) {
        if (h->calls <= h->max) droid_log("xtool: %s -> %s, forced to %#llx", h->name, v, (unsigned long long)h->force);
        x[0] = h->force;
    } else {
        droid_log("xtool: %s -> %s", h->name, v);
    }
    x[16] = rstack[rdepth].lr;
    return 1;
}
