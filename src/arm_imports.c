// arm_imports.c — what an ARM64 library gets for its imports. Functions are
// "svc #n" thunks into the same PS5 code an x86-64 game uses (arm_cpu.cpp
// moves the registers); only calls that pass a va_list, or hand back a
// function pointer, need an ARM version here.

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>

#include "FalsoJNI.h"
#include "java_layer.h"

#include "android_shim.h"
#include "arm_cpu.h"
#include "arm_modules.h"
void thread_place_named(pthread_t t, const char *name); // thread_place.c

void *bionic_resolve(const char *name);
void *linux_abi_resolve(const char *name);

int droid_arm_game;
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);

// ---- an ARM64 va_list read the way va_arg reads it --------------------------

typedef struct {
    uint64_t stack, gr_top, vr_top;
    int32_t gr_offs, vr_offs;
} arm_va;

static uint64_t arm_next_gp(arm_va *a) {
    uint64_t v;
    if (a->gr_offs < 0) {
        memcpy(&v, (const void *)(a->gr_top + a->gr_offs), 8);
        a->gr_offs += 8;
    } else {
        memcpy(&v, (const void *)a->stack, 8);
        a->stack += 8;
    }
    return v;
}

static uint64_t arm_next_fp(arm_va *a) {
    uint64_t v;
    if (a->vr_offs < 0) {
        memcpy(&v, (const void *)(a->vr_top + a->vr_offs), 8);
        a->vr_offs += 16;
    } else {
        memcpy(&v, (const void *)a->stack, 8);
        a->stack += 8;
    }
    return v;
}

static double u64_to_double(uint64_t v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}

// ---- ... rebuilt as an x86-64 va_list (layout of struct __va_list_tag) ------

#define X86_MAX_OVER 256 // (GTA San Andreas: an sscanf of 70+ fields)

typedef struct {
    uint32_t gp_offset, fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
    uint64_t save[6 + 16]; // rdi..r9, then xmm0-7 (16 bytes each)
    uint64_t over[X86_MAX_OVER];
    int ngp, nfp, nover;
} x86_va;

static void push_gp(x86_va *x, uint64_t v) {
    if (x->ngp < 6) x->save[x->ngp++] = v;
    else if (x->nover < X86_MAX_OVER) x->over[x->nover++] = v;
}

static void push_fp(x86_va *x, uint64_t v) {
    if (x->nfp < 8) {
        x->save[6 + 2 * x->nfp] = v;
        x->save[6 + 2 * x->nfp + 1] = 0;
        x->nfp++;
    } else if (x->nover < X86_MAX_OVER) {
        x->over[x->nover++] = v;
    }
}

// The format string says which class each argument is: move them across in order.
static struct __va_list_tag *convert(x86_va *x, const char *fmt, const void *arm_ap, int scan) {
    arm_va a;
    memcpy(&a, arm_ap, sizeof(a));
    memset(x, 0, sizeof(*x));
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        if (scan) {
            int suppress = *p == '*';
            if (suppress) p++;
            while (*p >= '0' && *p <= '9') p++;
            while (*p && strchr("hlLjztq", *p)) p++;
            if (*p == '[') {
                p++;
                if (*p == '^') p++;
                if (*p == ']') p++;
                while (*p && *p != ']') p++;
            }
            if (!*p) break;
            if (!suppress) push_gp(x, arm_next_gp(&a)); // every scanf argument is a pointer
            continue;
        }
        while (*p && strchr("-+ #0'", *p)) p++;
        if (*p == '*') { push_gp(x, arm_next_gp(&a)); p++; }
        else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            if (*p == '*') { push_gp(x, arm_next_gp(&a)); p++; }
            else while (*p >= '0' && *p <= '9') p++;
        }
        while (*p && strchr("hlLjztq", *p)) p++;
        if (!*p) break;
        if (strchr("fFeEgGaA", *p)) push_fp(x, arm_next_fp(&a));
        else if (strchr("diouxXcCsSpn", *p)) push_gp(x, arm_next_gp(&a));
        else break;
    }
    x->gp_offset = 0;
    x->fp_offset = 48;
    x->overflow_arg_area = x->over;
    x->reg_save_area = x->save;
    return (struct __va_list_tag *)x;
}

// ---- the va_list calls, ARM side --------------------------------------------

static int arm_vsnprintf(char *buf, size_t n, const char *fmt, const void *ap) {
    x86_va x;
    return vsnprintf(buf, n, fmt, convert(&x, fmt, ap, 0));
}

int arm_vsnprintf_entry(char *buf, size_t n, const char *fmt, const void *ap) {
    return arm_vsnprintf(buf, n, fmt, ap);
}

static int arm_vsprintf(char *buf, const char *fmt, const void *ap) {
    x86_va x;
    return vsprintf(buf, fmt, convert(&x, fmt, ap, 0));
}

static int arm_vasprintf(char **out, const char *fmt, const void *ap) {
    x86_va x;
    return vasprintf(out, fmt, convert(&x, fmt, ap, 0));
}

static int arm_vprintf(const char *fmt, const void *ap) {
    x86_va x;
    return vprintf(fmt, convert(&x, fmt, ap, 0));
}

static int arm_vfprintf(FILE *f, const char *fmt, const void *ap) {
    // bionic's vfprintf: stdout/stderr go to android.log
    static int (*real)(FILE *, const char *, va_list);
    if (!real) real = (int (*)(FILE *, const char *, va_list))bionic_resolve("vfprintf");
    x86_va x;
    return real(f, fmt, convert(&x, fmt, ap, 0));
}

static int arm_vsscanf(const char *str, const char *fmt, const void *ap) {
    x86_va x;
    return vsscanf(str, fmt, convert(&x, fmt, ap, 1));
}

// sscanf(str, fmt, ...): its pointers as ARM passed them - x2-x7, then the caller's stack. The
// plain call carried 6 registers and 2 stack words over: GTA San Andreas reads surfinfo.dat lines
// of 30-odd fields, the rest were garbage and sscanf wrote to 0x1260. Read as an ARM va_list
// whose register area is x2-x7 (still the caller's in the JIT during the call)
static int arm_sscanf(const char *str, const char *fmt) {
    uint64_t sp = 0, top = 0, regs[6];
    for (int i = 0; i < 6; i++) regs[i] = arm_guest_reg(2 + i);
    if (!arm_guest_sp_range(&sp, &top)) return -1;
    arm_va a = { .stack = sp, .gr_top = (uint64_t)(uintptr_t)(regs + 6), .vr_top = 0, .gr_offs = -48, .vr_offs = 0 };
    x86_va x;
    struct __va_list_tag *v = convert(&x, fmt, &a, 1);
    static int told;
    static const char *last_fmt;
    if (told < 24 && fmt != last_fmt && x.ngp + x.nover > 4) { // each new long format once
        told++;
        last_fmt = fmt;
        droid_log("sscanf: \"%.80s\" with %d pointers (x2 %#llx, stack[0] %#llx, sp %#llx)", fmt, x.ngp + x.nover,
                  (unsigned long long)regs[0], x.nover ? (unsigned long long)x.over[0] : 0ull, (unsigned long long)sp);
    }
    return vsscanf(str, fmt, v);
}

// swprintf(buf, n, fmt, ...) with Android's 32-bit wchar_t (wide32.c, WIDE32_GAMES): the wide
// format walked here, each conversion made by the narrow snprintf (%ls / %S: a wide string, %lc /
// %C: a wide character, %s: a narrow one, as C has it), the result back as UTF-32 into buf (n
// characters with its 0; -1 when it doesn't fit, as C's). Its arguments the ARM way: x3-x7,
// v0-v7, then the caller's stack (as arm_sscanf reads them).
static int w32_put_utf8(uint32_t c, char *out) {
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
static int arm_swprintf(uint32_t *buf, size_t n, const uint32_t *fmt) {
    uint64_t sp = 0, top = 0, regs[5], vr[16];
    for (int i = 0; i < 5; i++) regs[i] = arm_guest_reg(3 + i);
    uint64_t arm_guest_vreg(int i); // arm_cpu.cpp: v0-v7's low 64 bits
    for (int i = 0; i < 8; i++) vr[2 * i] = arm_guest_vreg(i), vr[2 * i + 1] = 0;
    if (!arm_guest_sp_range(&sp, &top)) return -1;
    arm_va a = { .stack = sp, .gr_top = (uint64_t)(uintptr_t)(regs + 5), .vr_top = (uint64_t)(uintptr_t)(vr + 16),
                 .gr_offs = -40, .vr_offs = -128 };
    static char out[16384];
    size_t o = 0;
    char piece[1024];
#define W_ADD(text, len)                                                                              \
    do {                                                                                              \
        size_t l_ = (len);                                                                            \
        if (o + l_ >= sizeof(out)) l_ = sizeof(out) - 1 - o;                                          \
        memcpy(out + o, (text), l_);                                                                  \
        o += l_;                                                                                      \
    } while (0)
    for (const uint32_t *p = fmt; *p; p++) {
        if (*p != '%') {
            char u[4];
            W_ADD(u, (size_t)w32_put_utf8(*p, u));
            continue;
        }
        if (p[1] == '%') { W_ADD("%", 1); p++; continue; }
        // the narrow spec: flags, width, precision (a '*' takes its number now), length, conversion
        char spec[64] = "%";
        size_t s = 1;
        int precision = -1, longmod = 0;
        p++;
        while (*p && strchr("-+ #0'", (int)*p) && s < 40) spec[s++] = (char)*p++;
        if (*p == '*') { s += (size_t)snprintf(spec + s, sizeof(spec) - s, "%d", (int)arm_next_gp(&a)); p++; }
        else while (*p >= '0' && *p <= '9' && s < 40) spec[s++] = (char)*p++;
        if (*p == '.') {
            spec[s++] = '.';
            p++;
            if (*p == '*') { precision = (int)arm_next_gp(&a); s += (size_t)snprintf(spec + s, sizeof(spec) - s, "%d", precision); p++; }
            else { precision = 0; while (*p >= '0' && *p <= '9' && s < 50) { precision = precision * 10 + (int)(*p - '0'); spec[s++] = (char)*p++; } }
        }
        size_t mod_at = s;
        while (*p && strchr("hlLjztq", (int)*p) && s < 58) {
            if (*p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'q') longmod++;
            spec[s++] = (char)*p++;
        }
        if (!*p) break;
        const char conv = (char)*p;
        int len = 0;
        if (strchr("diouxX", conv)) {
            const uint64_t v = arm_next_gp(&a);
            s = mod_at; // the length as "ll" (64 bits from the register) or none (an int)
            if (longmod) { spec[s++] = 'l'; spec[s++] = 'l'; }
            spec[s++] = conv, spec[s] = 0;
            len = longmod ? snprintf(piece, sizeof(piece), spec, (long long)v) : snprintf(piece, sizeof(piece), spec, (int)v);
        } else if (strchr("fFeEgGaA", conv)) {
            spec[mod_at] = conv, spec[mod_at + 1] = 0;
            len = snprintf(piece, sizeof(piece), spec, u64_to_double(arm_next_fp(&a)));
        } else if (conv == 'p') {
            spec[mod_at] = 'p', spec[mod_at + 1] = 0;
            len = snprintf(piece, sizeof(piece), spec, (void *)(uintptr_t)arm_next_gp(&a));
        } else if ((conv == 's' && longmod) || conv == 'S') { // a wide string: UTF-8 (precision in characters)
            const uint32_t *w = (const uint32_t *)(uintptr_t)arm_next_gp(&a);
            char text[sizeof(piece)];
            size_t t = 0;
            for (int k = 0; w && w[k] && (precision < 0 || k < precision) && t + 4 < sizeof(text); k++) t += (size_t)w32_put_utf8(w[k], text + t);
            text[t] = 0;
            char sspec[64];
            memcpy(sspec, spec, mod_at); // width and flags kept, the precision done above
            char *dot = memchr(sspec, '.', mod_at);
            size_t sl = dot ? (size_t)(dot - sspec) : mod_at;
            sspec[sl] = 's', sspec[sl + 1] = 0;
            len = snprintf(piece, sizeof(piece), sspec, text);
        } else if ((conv == 'c' && longmod) || conv == 'C') {
            len = w32_put_utf8((uint32_t)arm_next_gp(&a), piece);
        } else if (conv == 's') {
            spec[mod_at] = 's', spec[mod_at + 1] = 0;
            const char *str = (const char *)(uintptr_t)arm_next_gp(&a);
            len = snprintf(piece, sizeof(piece), spec, str ? str : "(null)");
        } else if (conv == 'c') {
            spec[mod_at] = 'c', spec[mod_at + 1] = 0;
            len = snprintf(piece, sizeof(piece), spec, (int)arm_next_gp(&a));
        } else if (conv == 'n') {
            arm_next_gp(&a); // (not written: no game asked yet)
        } else {
            break;
        }
        if (len > 0) W_ADD(piece, (size_t)len < sizeof(piece) ? (size_t)len : sizeof(piece) - 1);
    }
#undef W_ADD
    out[o] = 0;
    // back to UTF-32
    size_t k = 0;
    for (size_t i = 0; i < o;) {
        unsigned c = (unsigned char)out[i];
        uint32_t v = c;
        int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (l > 1) {
            v = c & (0x7F >> l);
            for (int j = 1; j < l && i + (size_t)j < o; j++) v = (v << 6) | ((unsigned char)out[i + (size_t)j] & 0x3F);
        }
        i += (size_t)l;
        if (k + 1 >= n) { if (n) buf[n - 1] = 0; return -1; } // too long: as C's swprintf
        buf[k++] = v;
    }
    if (n) buf[k] = 0;
    static int told;
    if (told++ < 10) droid_log("swprintf: %zu characters", k);
    return (int)k;
}
void *arm_swprintf_fn(void) { return (void *)arm_swprintf; }

static int arm_android_log_vprint(int prio, const char *tag, const char *fmt, const void *ap) {
    x86_va x;
    return __android_log_vprint(prio, tag, fmt, convert(&x, fmt, ap, 0));
}

// ---- calls that hand back function pointers: ARM gets thunks ----------------

static void *arm_egl_get_proc_address(const char *name) {
    void *p = droid_resolve(name);
    if (!p) { // (a game may call what it got without checking: the names it didn't get)
        static int told;
        if (told++ < 60) droid_log("eglGetProcAddress(%s): not here, NULL", name);
    }
    return p ? (void *)arm_thunk(name, p) : NULL;
}

// ---- calls that take guest function pointers: PS5 code gets trampolines -------

typedef int (*cmp_fn)(const void *, const void *);

static void arm_qsort(void *base, size_t n, size_t size, uint64_t cmp) {
    qsort(base, n, size, (cmp_fn)arm_callback(cmp));
}

static void *arm_bsearch(const void *key, const void *base, size_t n, size_t size, uint64_t cmp) {
    return bsearch(key, base, n, size, (cmp_fn)arm_callback(cmp));
}

static int arm_pthread_once(int *once, uint64_t fn) {
    static int (*real)(int *, void (*)(void));
    if (!real) real = (int (*)(int *, void (*)(void)))bionic_resolve("pthread_once");
    return real(once, (void (*)(void))arm_callback(fn));
}

// The game's thread keys: ours, 256 of them (bionic has 128), their values in each thread's
// TLS block (tpidr_el0 - 4096, read and written by the ARM code above). Destructors run when a
// game thread ends (arm_keys_thread_end, from arm_cpu.cpp). DROID_HOST_KEYS=1: the host's keys.
#define GUEST_KEYS 256
static uint64_t key_dtor[GUEST_KEYS];
static unsigned char key_used[GUEST_KEYS];
static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;
static int arm_pthread_key_create(pthread_key_t *key, uint64_t dtor) {
    if (getenv("DROID_HOST_KEYS")) return pthread_key_create(key, (void (*)(void *))arm_callback(dtor));
    pthread_mutex_lock(&key_lock);
    for (int k = 0; k < GUEST_KEYS; k++) {
        if (key_used[k]) continue;
        key_used[k] = 1;
        key_dtor[k] = dtor;
        pthread_mutex_unlock(&key_lock);
        *(int32_t *)key = k; // bionic's pthread_key_t: an int
        return 0;
    }
    pthread_mutex_unlock(&key_lock);
    droid_log("arm: pthread_key_create: all %d keys taken", GUEST_KEYS);
    return EAGAIN;
}
static int arm_pthread_key_delete(int key) {
    if (getenv("DROID_HOST_KEYS")) return pthread_key_delete((pthread_key_t)key);
    if (key < 0 || key >= GUEST_KEYS) return EINVAL;
    pthread_mutex_lock(&key_lock);
    key_used[key] = 0;
    key_dtor[key] = 0;
    pthread_mutex_unlock(&key_lock);
    return 0;
}
// a game thread ends: its keys' destructors, as bionic (values set to 0 first, up to 4 rounds)
void arm_keys_thread_end(uint64_t *values) {
    if (!values || getenv("DROID_HOST_KEYS")) return;
    for (int round = 0; round < 4; round++) {
        int any = 0;
        for (int k = 0; k < GUEST_KEYS; k++) {
            uint64_t v = values[k], d = key_used[k] ? key_dtor[k] : 0;
            if (!v || !d) continue;
            values[k] = 0;
            ((void (*)(void *))arm_callback(d))((void *)v);
            any = 1;
        }
        if (!any) break;
    }
}

static int arm_atexit(uint64_t fn) {
    (void)fn;
    return 0; // the game never exits normally
}

typedef int32_t (*aaudio_data_fn)(void *, void *, void *, int32_t);
void AAudioStreamBuilder_setDataCallback(void *b, aaudio_data_fn cb, void *user);

static void arm_aaudio_set_data_callback(void *b, uint64_t cb, void *user) {
    AAudioStreamBuilder_setDataCallback(b, (aaudio_data_fn)arm_callback(cb), user);
}

int linux_scandir(const char *path, void ***out, void *keep, void *order); // linux_abi.c
static int arm_scandir(const char *path, void ***out, uint64_t keep, uint64_t order) {
    return linux_scandir(path, out, arm_callback(keep), arm_callback(order));
}


// ---- Java (FalsoJNI) for ARM64: the same tables, every entry a thunk ---------------
// Call*MethodV get an ARM va_list: the method's signature (seen at GetMethodID)
// says which argument is which class, so it becomes an x86-64 one.

static struct JNINativeInterface arm_jni_iface;
static JNIEnv arm_env_obj = &arm_jni_iface;
static struct JNIInvokeInterface arm_vm_iface;
static JavaVM arm_vm_obj = &arm_vm_iface;
#define HOST ((const struct JNINativeInterface *)jni)

#define MAX_SIGS 4096
static struct { jmethodID id; const char *sig; } sigs[MAX_SIGS];
static int nsigs;
static pthread_mutex_t sig_lock = PTHREAD_MUTEX_INITIALIZER;

static void remember_sig(jmethodID id, const char *sig) {
    if (!id || !sig) return;
    pthread_mutex_lock(&sig_lock);
    int i;
    for (i = 0; i < nsigs && sigs[i].id != id; i++) {}
    if (i < nsigs) {
        if (strcmp(sigs[i].sig, sig) != 0) sigs[i].sig = strdup(sig);
    } else if (nsigs < MAX_SIGS) {
        sigs[nsigs].id = id;
        sigs[nsigs].sig = strdup(sig);
        nsigs++;
    }
    pthread_mutex_unlock(&sig_lock);
}

static const char *sig_of(jmethodID id) {
    const char *s = NULL;
    pthread_mutex_lock(&sig_lock);
    for (int i = 0; i < nsigs && !s; i++)
        if (sigs[i].id == id) s = sigs[i].sig;
    pthread_mutex_unlock(&sig_lock);
    return s;
}

static struct __va_list_tag *convert_sig(x86_va *x, jmethodID id, const void *arm_ap) {
    arm_va a;
    memcpy(&a, arm_ap, sizeof(a));
    memset(x, 0, sizeof(*x));
    const char *s = sig_of(id);
    if (!s) {
        static int warned;
        if (!warned++) droid_log("java: V call on a method with no known signature");
        for (int i = 0; i < 8; i++) push_gp(x, arm_next_gp(&a));
    } else if ((s = strchr(s, '('))) {
        for (s++; *s && *s != ')';) {
            if (*s == '[' || *s == 'L') {
                while (*s == '[') s++;
                if (*s == 'L') {
                    const char *e = strchr(s, ';');
                    s = e ? e + 1 : s + strlen(s);
                } else if (*s) {
                    s++;
                }
                push_gp(x, arm_next_gp(&a));
                continue;
            }
            if (*s == 'F' || *s == 'D') push_fp(x, arm_next_fp(&a)); // float: promoted to double
            else push_gp(x, arm_next_gp(&a));
            s++;
        }
    }
    x->gp_offset = 0;
    x->fp_offset = 48;
    x->overflow_arg_area = x->over;
    x->reg_save_area = x->save;
    return (struct __va_list_tag *)x;
}

// Our Java layer's methods (java_layer.c) first; FalsoJNI's otherwise. Every form:
// Call<T>Method(...), ...V (an ARM va_list), ...A (jvalue[]); instance, static, nonvirtual.

#define MAX_ARGS 16

static void jv_from_arm(jmethodID m, const void *arm_ap, jvalue *out) {
    char k[MAX_ARGS];
    int n = java_arg_kinds(java_sig(m), k, MAX_ARGS);
    arm_va a;
    memcpy(&a, arm_ap, sizeof(a));
    for (int i = 0; i < n; i++) {
        out[i].j = 0;
        if (k[i] == 'F') out[i].f = (float)u64_to_double(arm_next_fp(&a));
        else if (k[i] == 'D') out[i].d = u64_to_double(arm_next_fp(&a));
        else if (k[i] == 'J' || k[i] == 'L') out[i].j = (jlong)arm_next_gp(&a);
        else out[i].i = (jint)arm_next_gp(&a);
    }
}

static void jv_from_host(jmethodID m, va_list ap, jvalue *out) {
    char k[MAX_ARGS];
    int n = java_arg_kinds(java_sig(m), k, MAX_ARGS);
    for (int i = 0; i < n; i++) {
        out[i].j = 0;
        if (k[i] == 'F') out[i].f = (float)va_arg(ap, double);
        else if (k[i] == 'D') out[i].d = va_arg(ap, double);
        else if (k[i] == 'J') out[i].j = va_arg(ap, jlong);
        else if (k[i] == 'L') out[i].l = va_arg(ap, jobject);
        else out[i].i = va_arg(ap, jint);
    }
}

#define ARM_JNI_T(N, T, F)                                                                           \
    static T arm_Call##N##Method(JNIEnv *e, jobject o, jmethodID m, ...) {                         \
        (void)e;                                                                                     \
        va_list ap;                                                                                  \
        va_start(ap, m);                                                                             \
        T r;                                                                                         \
        if (java_is_ours(m)) {                                                                       \
            jvalue a[MAX_ARGS];                                                                      \
            jv_from_host(m, ap, a);                                                                  \
            r = (T)java_call(m, o, a).F;                                                             \
        } else {                                                                                     \
            r = HOST->Call##N##MethodV(&jni, o, m, ap);                                              \
        }                                                                                            \
        va_end(ap);                                                                                  \
        return r;                                                                                    \
    }                                                                                                \
    static T arm_Call##N##MethodV(JNIEnv *e, jobject o, jmethodID m, const void *ap) {               \
        (void)e;                                                                                     \
        if (java_is_ours(m)) {                                                                       \
            jvalue a[MAX_ARGS];                                                                      \
            jv_from_arm(m, ap, a);                                                                   \
            return (T)java_call(m, o, a).F;                                                          \
        }                                                                                            \
        x86_va x;                                                                                    \
        return HOST->Call##N##MethodV(&jni, o, m, convert_sig(&x, m, ap));                           \
    }                                                                                                \
    static T arm_Call##N##MethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *a) {              \
        (void)e;                                                                                     \
        if (java_is_ours(m)) return (T)java_call(m, o, a).F;                                         \
        return HOST->Call##N##MethodA(&jni, o, m, a);                                                \
    }                                                                                                \
    static T arm_CallStatic##N##Method(JNIEnv *e, jclass c, jmethodID m, ...) {                    \
        (void)e;                                                                                     \
        va_list ap;                                                                                  \
        va_start(ap, m);                                                                             \
        T r;                                                                                         \
        if (java_is_ours(m)) {                                                                       \
            jvalue a[MAX_ARGS];                                                                      \
            jv_from_host(m, ap, a);                                                                  \
            r = (T)java_call(m, (jobject)c, a).F;                                                    \
        } else {                                                                                     \
            r = HOST->CallStatic##N##MethodV(&jni, c, m, ap);                                        \
        }                                                                                            \
        va_end(ap);                                                                                  \
        return r;                                                                                    \
    }                                                                                                \
    static T arm_CallStatic##N##MethodV(JNIEnv *e, jclass c, jmethodID m, const void *ap) {          \
        (void)e;                                                                                     \
        if (java_is_ours(m)) {                                                                       \
            jvalue a[MAX_ARGS];                                                                      \
            jv_from_arm(m, ap, a);                                                                   \
            return (T)java_call(m, (jobject)c, a).F;                                                 \
        }                                                                                            \
        x86_va x;                                                                                    \
        return HOST->CallStatic##N##MethodV(&jni, c, m, convert_sig(&x, m, ap));                     \
    }                                                                                                \
    static T arm_CallStatic##N##MethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) {         \
        (void)e;                                                                                     \
        if (java_is_ours(m)) return (T)java_call(m, (jobject)c, a).F;                                \
        return HOST->CallStatic##N##MethodA(&jni, c, m, a);                                          \
    }                                                                                                \
    static T arm_CallNonvirtual##N##Method(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) {     \
        (void)e;                                                                                     \
        va_list ap;                                                                                  \
        va_start(ap, m);                                                                             \
        T r;                                                                                         \
        if (java_is_ours(m)) {                                                                       \
            jvalue a[MAX_ARGS];                                                                      \
            jv_from_host(m, ap, a);                                                                  \
            r = (T)java_call(m, o, a).F;                                                             \
        } else {                                                                                     \
            r = HOST->CallNonvirtual##N##MethodV(&jni, o, c, m, ap);                                 \
        }                                                                                            \
        va_end(ap);                                                                                  \
        return r;                                                                                    \
    }                                                                                                \
    static T arm_CallNonvirtual##N##MethodV(JNIEnv *e, jobject o, jclass c, jmethodID m,             \
                                            const void *ap) {                                        \
        (void)e;                                                                                     \
        if (java_is_ours(m)) {                                                                       \
            jvalue a[MAX_ARGS];                                                                      \
            jv_from_arm(m, ap, a);                                                                   \
            return (T)java_call(m, o, a).F;                                                          \
        }                                                                                            \
        x86_va x;                                                                                    \
        return HOST->CallNonvirtual##N##MethodV(&jni, o, c, m, convert_sig(&x, m, ap));              \
    }                                                                                                \
    static T arm_CallNonvirtual##N##MethodA(JNIEnv *e, jobject o, jclass c, jmethodID m,             \
                                            const jvalue *a) {                                       \
        (void)e;                                                                                     \
        if (java_is_ours(m)) return (T)java_call(m, o, a).F;                                         \
        return HOST->CallNonvirtual##N##MethodA(&jni, o, c, m, a);                                   \
    }
ARM_JNI_T(Object, jobject, l)
ARM_JNI_T(Boolean, jboolean, z)
ARM_JNI_T(Byte, jbyte, b)
ARM_JNI_T(Char, jchar, c)
ARM_JNI_T(Short, jshort, s)
ARM_JNI_T(Int, jint, i)
ARM_JNI_T(Long, jlong, j)
ARM_JNI_T(Float, jfloat, f)
ARM_JNI_T(Double, jdouble, d)

// void: same shapes, no result
static void arm_CallVoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_host(m, ap, a);
        java_call(m, o, a);
    } else {
        HOST->CallVoidMethodV(&jni, o, m, ap);
    }
    va_end(ap);
}
static void arm_CallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, const void *ap) {
    (void)e;
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_arm(m, ap, a);
        java_call(m, o, a);
        return;
    }
    x86_va x;
    HOST->CallVoidMethodV(&jni, o, m, convert_sig(&x, m, ap));
}
static void arm_CallVoidMethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *a) {
    (void)e;
    if (java_is_ours(m)) java_call(m, o, a);
    else HOST->CallVoidMethodA(&jni, o, m, a);
}
static void arm_CallStaticVoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_host(m, ap, a);
        java_call(m, (jobject)c, a);
    } else {
        HOST->CallStaticVoidMethodV(&jni, c, m, ap);
    }
    va_end(ap);
}
static void arm_CallStaticVoidMethodV(JNIEnv *e, jclass c, jmethodID m, const void *ap) {
    (void)e;
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_arm(m, ap, a);
        java_call(m, (jobject)c, a);
        return;
    }
    x86_va x;
    HOST->CallStaticVoidMethodV(&jni, c, m, convert_sig(&x, m, ap));
}
static void arm_CallStaticVoidMethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) {
    (void)e;
    if (java_is_ours(m)) java_call(m, (jobject)c, a);
    else HOST->CallStaticVoidMethodA(&jni, c, m, a);
}
static void arm_CallNonvirtualVoidMethod(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_host(m, ap, a);
        java_call(m, o, a);
    } else {
        HOST->CallNonvirtualVoidMethodV(&jni, o, c, m, ap);
    }
    va_end(ap);
}
static void arm_CallNonvirtualVoidMethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, const void *ap) {
    (void)e;
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_arm(m, ap, a);
        java_call(m, o, a);
        return;
    }
    x86_va x;
    HOST->CallNonvirtualVoidMethodV(&jni, o, c, m, convert_sig(&x, m, ap));
}
static void arm_CallNonvirtualVoidMethodA(JNIEnv *e, jobject o, jclass c, jmethodID m, const jvalue *a) {
    (void)e;
    if (java_is_ours(m)) java_call(m, o, a);
    else HOST->CallNonvirtualVoidMethodA(&jni, o, c, m, a);
}

// constructors
static jobject arm_NewObject(JNIEnv *e, jclass c, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    jobject r;
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_host(m, ap, a);
        r = java_call(m, NULL, a).l;
    } else {
        r = HOST->NewObjectV(&jni, c, m, ap);
    }
    va_end(ap);
    return r;
}
static jobject arm_NewObjectV(JNIEnv *e, jclass c, jmethodID m, const void *ap) {
    (void)e;
    if (java_is_ours(m)) {
        jvalue a[MAX_ARGS];
        jv_from_arm(m, ap, a);
        return java_call(m, NULL, a).l;
    }
    x86_va x;
    return HOST->NewObjectV(&jni, c, m, convert_sig(&x, m, ap));
}
static jobject arm_NewObjectA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) {
    (void)e;
    if (java_is_ours(m)) return java_call(m, NULL, a).l;
    return HOST->NewObjectA(&jni, c, m, a);
}

// objects: ours know their class; nothing is freed (FalsoJNI's DeleteGlobalRef
// free()s, which breaks on objects that weren't malloc'd)
static jclass arm_GetObjectClass(JNIEnv *e, jobject o) {
    (void)e;
    return java_class_of(o);
}
static jclass arm_FindClass(JNIEnv *e, const char *name) {
    (void)e;
    jclass c = HOST->FindClass(&jni, name);
    java_note_class(c);
    return c;
}
static jboolean arm_IsInstanceOf(JNIEnv *e, jobject o, jclass c) {
    (void)e;
    return java_instance_of(o, c);
}
static void arm_DeleteRef(JNIEnv *e, jobject o) {
    (void)e; (void)o;
}

// fields: our classes' are per object (java_layer.c), the rest FalsoJNI's
// A field nobody knows: an ID anyway, as on a phone (Firebase gives up on a missing one).
// Each one its own (name, signature): an int reads 0; a String (or a CONSTANT_NAME of an
// Object type) its own name, never null (Unity's notifications plugin keys a map with its
// KEY_* constants: null was a NullReferenceException); other objects null. Writes go nowhere.
typedef struct {
    char name[64], sig[96];
    jobject text;
} stub_field;
#define MAX_STUB_FIELDS 1024
static stub_field stub_fields[MAX_STUB_FIELDS];
static int stub_field_count;
static pthread_mutex_t stub_field_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_stub_field(jfieldID f) {
    return (const stub_field *)f >= stub_fields && (const stub_field *)f < stub_fields + MAX_STUB_FIELDS;
}

static jfieldID make_stub_field(const char *name, const char *sig) {
    pthread_mutex_lock(&stub_field_lock);
    stub_field *found = NULL;
    for (int i = 0; i < stub_field_count && !found; i++)
        if (!strcmp(stub_fields[i].name, name) && !strcmp(stub_fields[i].sig, sig)) found = &stub_fields[i];
    if (!found && stub_field_count < MAX_STUB_FIELDS) {
        found = &stub_fields[stub_field_count++];
        snprintf(found->name, sizeof(found->name), "%s", name);
        snprintf(found->sig, sizeof(found->sig), "%s", sig);
    }
    if (!found) found = &stub_fields[MAX_STUB_FIELDS - 1];
    pthread_mutex_unlock(&stub_field_lock);
    return (jfieldID)found;
}

jobject java_signing_info(void), java_signatures(void); // java_layer.c: the APK's certificate

static jobject stub_field_object(jfieldID f) {
    stub_field *s = (stub_field *)f;
    if (!strcmp(s->name, "signingInfo")) return java_signing_info(); // PackageInfo's
    if (!strcmp(s->name, "signatures")) return java_signatures();
    int constant = 1;
    for (const char *c = s->name; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) constant = 0;
    if (!strcmp(s->sig, "Ljava/lang/String;") || (constant && !strcmp(s->sig, "Ljava/lang/Object;"))) {
        if (!s->text) s->text = jni->NewStringUTF(&jni, s->name);
        return s->text;
    }
    return NULL;
}
#define STUB_FIELD_FOR(name, sig) make_stub_field(name, sig)

static void note_stub_field(jclass c, const char *name, const char *sig, int is_static) {
    static int logged;
    if (logged++ < 60)
        droid_log("java: %sfield %s %s %s: unknown, a stub", is_static ? "static " : "",
                  java_is_string((jobject)c) || (uintptr_t)c < 0x10000 || (uintptr_t)c == 0x42424242 ? "?" : (const char *)c,
                  name, sig);
}

static jfieldID arm_GetFieldID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e;
    jfieldID f = java_field(c, name);
    if (!f) f = HOST->GetFieldID(&jni, c, name, sig);
    if (!f) {
        note_stub_field(c, name, sig, 0);
        f = STUB_FIELD_FOR(name, sig);
    }
    return f;
}
static jfieldID arm_GetStaticFieldID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e;
    jfieldID f = HOST->GetStaticFieldID(&jni, c, name, sig);
    if (!f) {
        note_stub_field(c, name, sig, 1);
        f = STUB_FIELD_FOR(name, sig);
    }
    return f;
}
static jint arm_GetIntField(JNIEnv *e, jobject o, jfieldID f) {
    (void)e;
    if (is_stub_field(f)) return 0;
    return java_is_our_field(f) ? java_get_field(o, f).i : HOST->GetIntField(&jni, o, f);
}
int droid_game_in(const char *list); // game_settings.c
#define RET8_GAMES "com.nekki.vector2" // jboolean JNI answers as 0 / 1 (arm_jni_setup)
// (Halfbrick's Mortar games only, where they were made; every other game as before: FalsoJNI's)
#define FIELD_GAMES "com.halfbrick."
static jboolean arm_GetBooleanField(JNIEnv *e, jobject o, jfieldID f) { // (Mortar's HttpClient.IsFinished)
    (void)e;
    if (!droid_game_in(FIELD_GAMES)) return HOST->GetBooleanField(&jni, o, f);
    if (is_stub_field(f)) return 0;
    return java_is_our_field(f) ? java_get_field(o, f).z : HOST->GetBooleanField(&jni, o, f);
}
static jlong arm_GetLongField(JNIEnv *e, jobject o, jfieldID f) { // (Mortar's HttpClient.RequestPointer)
    (void)e;
    if (!droid_game_in(FIELD_GAMES)) return HOST->GetLongField(&jni, o, f);
    if (is_stub_field(f)) return 0;
    return java_is_our_field(f) ? java_get_field(o, f).j : HOST->GetLongField(&jni, o, f);
}
// NewStringUTF(NULL): null, as Android's (Jetpack Joyride passes it one; strlen(NULL) crashed)
static jstring arm_NewStringUTF(JNIEnv *e, const char *s) {
    (void)e;
    return s ? HOST->NewStringUTF(&jni, s) : NULL;
}
static jobject arm_GetObjectField(JNIEnv *e, jobject o, jfieldID f) {
    (void)e;
    if (is_stub_field(f)) return stub_field_object(f); // not FalsoJNI's placeholder object (not memory)
    return java_is_our_field(f) ? java_get_field(o, f).l : HOST->GetObjectField(&jni, o, f);
}
static jobject arm_GetStaticObjectField(JNIEnv *e, jclass c, jfieldID f) {
    (void)e;
    return is_stub_field(f) ? stub_field_object(f) : HOST->GetStaticObjectField(&jni, c, f);
}
static void arm_SetIntField(JNIEnv *e, jobject o, jfieldID f, jint v) {
    (void)e;
    if (!java_is_our_field(f)) {
        HOST->SetIntField(&jni, o, f, v);
        return;
    }
    jvalue x;
    x.j = 0;
    x.i = v;
    java_set_field(o, f, x);
}
static void arm_SetObjectField(JNIEnv *e, jobject o, jfieldID f, jobject v) {
    (void)e;
    if (!java_is_our_field(f)) {
        HOST->SetObjectField(&jni, o, f, v);
        return;
    }
    jvalue x;
    x.l = v;
    java_set_field(o, f, x);
}

// Strings: FalsoJNI's, or (seen from the game) a raw C string that stood in
// for one — the text is answered instead of reading it as a JavaString.
static int raw_logged;
static void note_raw(const char *fn, jstring s) {
    if (raw_logged < 8) {
        raw_logged++;
        droid_log("java: %s on a raw C string \"%.60s\"", fn, java_text(s));
    }
}
static const char *arm_GetStringUTFChars(JNIEnv *e, jstring s, jboolean *copy) {
    (void)e;
    if (copy) *copy = JNI_FALSE;
    if (!s) return ""; // a null string: empty text, not NULL (a Java stub of ours answered null: Genshin's GME read it)
    if (java_is_string(s)) return HOST->GetStringUTFChars(&jni, s, copy);
    note_raw("GetStringUTFChars", s);
    return java_text(s);
}
static void arm_ReleaseStringUTFChars(JNIEnv *e, jstring s, const char *chars) {
    (void)e;
    if (java_is_string(s)) HOST->ReleaseStringUTFChars(&jni, s, chars);
}
static jsize arm_GetStringUTFLength(JNIEnv *e, jstring s) {
    (void)e;
    if (!s) return 0;
    return java_is_string(s) ? HOST->GetStringUTFLength(&jni, s) : (jsize)strlen(java_text(s));
}
static jsize arm_GetStringLength(JNIEnv *e, jstring s) {
    (void)e;
    if (!s) return 0;
    return java_is_string(s) ? HOST->GetStringLength(&jni, s) : (jsize)strlen(java_text(s));
}
static const jchar *arm_GetStringChars(JNIEnv *e, jstring s, jboolean *copy) {
    (void)e;
    if (copy) *copy = JNI_TRUE;
    if (!s) return NULL;
    if (java_is_string(s)) return HOST->GetStringChars(&jni, s, copy);
    note_raw("GetStringChars", s);
    const char *t = java_text(s);
    size_t n = strlen(t);
    jchar *u = malloc((n + 1) * sizeof(jchar)); // (ASCII) kept: the game may hold it
    for (size_t i = 0; i <= n; i++) u[i] = (unsigned char)t[i];
    return u;
}
static void arm_ReleaseStringChars(JNIEnv *e, jstring s, const jchar *chars) {
    (void)e;
    if (java_is_string(s)) HOST->ReleaseStringChars(&jni, s, chars);
}
static void arm_GetStringUTFRegion(JNIEnv *e, jstring s, jsize start, jsize len, char *buf) {
    (void)e;
    if (java_is_string(s)) {
        HOST->GetStringUTFRegion(&jni, s, start, len, buf);
        return;
    }
    memcpy(buf, java_text(s) + start, (size_t)len);
    buf[len] = 0;
}

// FalsoJNI finds methods by name only: an answer for a signature it never saw
// may be wrong (String.length() came back 0). Each such name is logged once.
static void note_by_name(jclass c, const char *name, const char *sig, jmethodID id, int is_static) {
    static char seen[256][96];
    static int n;
    char key[96];
    snprintf(key, sizeof(key), "%s%s", name, sig);
    for (int i = 0; i < n; i++)
        if (strcmp(seen[i], key) == 0) return;
    if (n < 256) snprintf(seen[n++], sizeof(seen[0]), "%s", key);
    const char *cls = java_is_string((jobject)c) ? "?" : ((uintptr_t)c > 0x10000 && (uintptr_t)c != 0x42424242 ? (const char *)c : "?");
    droid_log("java: %s%s.%s%s -> %s", is_static ? "static " : "", cls, name, sig,
              id ? "FalsoJNI (by name)" : "NOT FOUND");
}

static jmethodID arm_GetMethodID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e;
    jmethodID ours = java_method(c, name, sig);
    if (ours) return ours;
    jmethodID iface = java_dynamic_method(c, name, sig); // callbacks / listeners / Runnable
    if (iface) return iface;
    jmethodID id = HOST->GetMethodID(&jni, c, name, sig);
    note_by_name(c, name, sig, id, 0);
    if (!id) return java_stub_method(c, name, sig); // as on a phone: it exists (does nothing here)
    remember_sig(id, sig);
    return id;
}

static jmethodID arm_GetStaticMethodID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e;
    jmethodID ours = java_method(c, name, sig);
    if (ours) return ours;
    jmethodID id = HOST->GetStaticMethodID(&jni, c, name, sig);
    note_by_name(c, name, sig, id, 1);
    if (!id) return java_stub_method(c, name, sig);
    remember_sig(id, sig);
    return id;
}

// Direct ByteBuffers (FalsoJNI has none): java_layer.c's
static jobject arm_NewDirectByteBuffer(JNIEnv *e, void *address, jlong capacity) {
    (void)e;
    return java_new_direct_buffer(address, capacity);
}
static void *arm_GetDirectBufferAddress(JNIEnv *e, jobject buf) {
    (void)e;
    return java_direct_address(buf);
}
static jlong arm_GetDirectBufferCapacity(JNIEnv *e, jobject buf) {
    (void)e;
    return java_direct_capacity(buf);
}

static jobject arm_ToReflectedMethod(JNIEnv *e, jclass c, jmethodID id, jboolean is_static) {
    (void)e;
    jobject m = java_to_reflected(id);
    return m ? m : HOST->ToReflectedMethod(&jni, c, id, is_static);
}

static jmethodID arm_FromReflectedMethod(JNIEnv *e, jobject method) {
    (void)e;
    jmethodID id = java_from_reflected(method);
    return id ? id : HOST->FromReflectedMethod(&jni, method);
}

static jfieldID arm_FromReflectedField(JNIEnv *e, jobject field) {
    (void)e;
    jfieldID id = java_from_reflected_field(field);
    return id ? id : HOST->FromReflectedField(&jni, field);
}

// The same lookups for java_layer.c's ReflectionHelper (getMethodID / getFieldID)
jmethodID droid_jni_method_id(jclass c, const char *name, const char *sig, int is_static) {
    return is_static ? arm_GetStaticMethodID(NULL, c, name, sig) : arm_GetMethodID(NULL, c, name, sig);
}
jfieldID droid_jni_field_id(jclass c, const char *name, const char *sig, int is_static) {
    return is_static ? arm_GetStaticFieldID(NULL, c, name, sig) : arm_GetFieldID(NULL, c, name, sig);
}

// The game's natives are ARM code: what Java calls are trampolines.
static jint arm_RegisterNatives(JNIEnv *e, jclass c, const JNINativeMethod *m, jint n) {
    (void)e;
    JNINativeMethod *copy = malloc((size_t)(n > 0 ? n : 1) * sizeof(*copy));
    if (!copy) return JNI_ERR;
    for (jint i = 0; i < n; i++) {
        copy[i] = m[i];
        copy[i].fnPtr = m[i].fnPtr ? arm_callback((uint64_t)m[i].fnPtr) : NULL; // (an empty slot: kept empty)
    }
    return HOST->RegisterNatives(&jni, c, copy, n);
}

static jint arm_GetJavaVM(JNIEnv *e, JavaVM **vm) {
    (void)e;
    *vm = &arm_vm_obj;
    return JNI_OK;
}

static jint arm_AttachCurrentThread(JavaVM *vm, JNIEnv **env, void *args) {
    (void)vm; (void)args;
    *env = &arm_env_obj;
    return JNI_OK;
}

static jint arm_GetEnv(JavaVM *vm, void **env, jint version) {
    (void)vm; (void)version;
    if (!env) return JNI_EINVAL;
    *env = &arm_env_obj;
    return JNI_OK;
}

#define SET(table, field, fn) (table).field = (__typeof__((table).field))(fn)
#define SET_V(N)                                                                      \
    SET(arm_jni_iface, Call##N##Method, arm_Call##N##Method);                         \
    SET(arm_jni_iface, Call##N##MethodV, arm_Call##N##MethodV);                       \
    SET(arm_jni_iface, Call##N##MethodA, arm_Call##N##MethodA);                       \
    SET(arm_jni_iface, CallNonvirtual##N##Method, arm_CallNonvirtual##N##Method);     \
    SET(arm_jni_iface, CallNonvirtual##N##MethodV, arm_CallNonvirtual##N##MethodV);   \
    SET(arm_jni_iface, CallNonvirtual##N##MethodA, arm_CallNonvirtual##N##MethodA);   \
    SET(arm_jni_iface, CallStatic##N##Method, arm_CallStatic##N##Method);             \
    SET(arm_jni_iface, CallStatic##N##MethodV, arm_CallStatic##N##MethodV);           \
    SET(arm_jni_iface, CallStatic##N##MethodA, arm_CallStatic##N##MethodA)

static void thunk_all(void *table, size_t size, size_t skip, const char *tag) {
    void **p = table;
    for (size_t i = skip; i < size / sizeof(void *); i++) {
        if (!p[i]) continue;
        char name[32];
        snprintf(name, sizeof(name), "%s#%zu", tag, i);
        p[i] = (void *)arm_thunk(name, p[i]);
    }
}

#include "arm_jni_art.inc"

static void arm_jni_setup(void) {
    if (jni_ready) return;
    jni_ready = 1;
    arm_jni_iface = *HOST;
    SET_V(Object); SET_V(Boolean); SET_V(Byte); SET_V(Char); SET_V(Short); SET_V(Int);
    SET_V(Long); SET_V(Float); SET_V(Double); SET_V(Void);
    SET(arm_jni_iface, NewObject, arm_NewObject);
    SET(arm_jni_iface, NewObjectV, arm_NewObjectV);
    SET(arm_jni_iface, NewObjectA, arm_NewObjectA);
    SET(arm_jni_iface, GetObjectClass, arm_GetObjectClass);
    SET(arm_jni_iface, FindClass, arm_FindClass);
    SET(arm_jni_iface, GetStringUTFChars, arm_GetStringUTFChars);
    SET(arm_jni_iface, ReleaseStringUTFChars, arm_ReleaseStringUTFChars);
    SET(arm_jni_iface, GetStringUTFLength, arm_GetStringUTFLength);
    SET(arm_jni_iface, GetStringLength, arm_GetStringLength);
    SET(arm_jni_iface, GetStringChars, arm_GetStringChars);
    SET(arm_jni_iface, ReleaseStringChars, arm_ReleaseStringChars);
    SET(arm_jni_iface, GetStringUTFRegion, arm_GetStringUTFRegion);
    SET(arm_jni_iface, IsInstanceOf, arm_IsInstanceOf);
    SET(arm_jni_iface, DeleteGlobalRef, arm_DeleteRef);
    SET(arm_jni_iface, DeleteLocalRef, arm_DeleteRef);
    SET(arm_jni_iface, DeleteWeakGlobalRef, arm_DeleteRef);
    SET(arm_jni_iface, FromReflectedMethod, arm_FromReflectedMethod);
    SET(arm_jni_iface, FromReflectedField, arm_FromReflectedField);
    SET(arm_jni_iface, ToReflectedMethod, arm_ToReflectedMethod);
    SET(arm_jni_iface, NewDirectByteBuffer, arm_NewDirectByteBuffer);
    SET(arm_jni_iface, GetDirectBufferAddress, arm_GetDirectBufferAddress);
    SET(arm_jni_iface, GetDirectBufferCapacity, arm_GetDirectBufferCapacity);
    SET(arm_jni_iface, GetFieldID, arm_GetFieldID);
    SET(arm_jni_iface, GetStaticFieldID, arm_GetStaticFieldID);
    SET(arm_jni_iface, GetStaticObjectField, arm_GetStaticObjectField);
    SET(arm_jni_iface, GetIntField, arm_GetIntField);
    SET(arm_jni_iface, NewStringUTF, arm_NewStringUTF);
    SET(arm_jni_iface, GetBooleanField, arm_GetBooleanField);
    SET(arm_jni_iface, GetLongField, arm_GetLongField);
    SET(arm_jni_iface, GetObjectField, arm_GetObjectField);
    SET(arm_jni_iface, SetIntField, arm_SetIntField);
    SET(arm_jni_iface, SetObjectField, arm_SetObjectField);
    SET(arm_jni_iface, GetMethodID, arm_GetMethodID);
    SET(arm_jni_iface, GetStaticMethodID, arm_GetStaticMethodID);
    SET(arm_jni_iface, RegisterNatives, arm_RegisterNatives);
    SET(arm_jni_iface, GetJavaVM, arm_GetJavaVM);
    thunk_all(&arm_jni_iface, sizeof(arm_jni_iface), 4, "jni");
    // the jboolean answers, 0 / 1 only (x86 leaves the rest of rax as it was: on the PS5 Unity got
    // IsSameObject's garbage, "the same" every time, and Vector 2 aborted after its logo). The
    // games it's tested on first; the others as before.
    if (droid_game_in(RET8_GAMES)) {
        static const int bools[] = { 11, 24, 32, 37, 38, 39, 67, 68, 69, 96, 117, 118, 119, 146, 228 };
        for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
            char name[32];
            snprintf(name, sizeof(name), "jni#%d", bools[i]);
            arm_thunk_ret8(name);
        }
    }

    arm_vm_iface = *(const struct JNIInvokeInterface *)jvm;
    SET(arm_vm_iface, AttachCurrentThread, arm_AttachCurrentThread);
    SET(arm_vm_iface, AttachCurrentThreadAsDaemon, arm_AttachCurrentThread);
    SET(arm_vm_iface, GetEnv, arm_GetEnv);
    thunk_all(&arm_vm_iface, sizeof(arm_vm_iface), 3, "jvm");

    // Java calling the game (natives, JNI_OnLoad) passes our env/vm first.
    arm_swap_arg0((uint64_t)&jni, (uint64_t)&arm_env_obj);
    arm_swap_arg0((uint64_t)&jvm, (uint64_t)&arm_vm_obj);
}

void *arm_jni_env(void) {
    arm_jni_setup();
    return &arm_env_obj;
}

void *arm_java_vm(void) {
    arm_jni_setup();
    return &arm_vm_obj;
}

// ---- Linux-on-ARM64 calls: syscall numbers and aux values differ from the PS5 ----

// futex (Linux's wait/wake on an int; Unity's Baselib locks, bionic's too):
// wait sleeps only while *uaddr == val; one lock + condition per bucket of
// addresses, so a wake between the check and the sleep can't be lost.
#define FX_BUCKETS 256
static pthread_mutex_t fx_lock[FX_BUCKETS];
static pthread_cond_t fx_cond[FX_BUCKETS];
static int fx_waiters[FX_BUCKETS];
static pthread_once_t fx_once = PTHREAD_ONCE_INIT;

static void fx_init(void);
static void fx_init(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    for (int i = 0; i < FX_BUCKETS; i++) {
        pthread_mutex_init(&fx_lock[i], NULL);
        pthread_cond_init(&fx_cond[i], &a);
    }
}

static void ts_add(struct timespec *t, const struct timespec *d) {
    t->tv_sec += d->tv_sec;
    t->tv_nsec += d->tv_nsec;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

static unsigned fx_bucket(const int *a) { return (unsigned)(((uintptr_t)a >> 2) % FX_BUCKETS); }

static int fx_wake(unsigned b) {
    pthread_mutex_lock(&fx_lock[b]);
    int w = fx_waiters[b];
    if (w) pthread_cond_broadcast(&fx_cond[b]); // waiters re-check their word
    pthread_mutex_unlock(&fx_lock[b]);
    return w;
}

// FUTEX_WAKE_OP: *uaddr2 = op(*uaddr2, oparg) atomically; wake uaddr's waiters,
// and uaddr2's too when the old *uaddr2 passes the compare (glibc condvars use it).
static long fx_wake_op(int *uaddr, int val, int val2, int *uaddr2, int val3) {
    int op = (val3 >> 28) & 7, cmp = (val3 >> 24) & 15;
    int oparg = (val3 >> 12) & 0xfff, cmparg = val3 & 0xfff;
    if (oparg & 0x800) oparg |= ~0xfff; // sign extended
    if (cmparg & 0x800) cmparg |= ~0xfff;
    if ((val3 >> 28) & 8) oparg = 1 << (oparg & 31);
    int old = __atomic_load_n(uaddr2, __ATOMIC_SEQ_CST), nv;
    do {
        switch (op) {
        case 0: nv = oparg; break;            // SET
        case 1: nv = old + oparg; break;      // ADD
        case 2: nv = old | oparg; break;      // OR
        case 3: nv = old & ~oparg; break;     // ANDN
        default: nv = old ^ oparg; break;     // XOR
        }
    } while (!__atomic_compare_exchange_n(uaddr2, &old, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    int w = fx_wake(fx_bucket(uaddr));
    int pass;
    switch (cmp) {
    case 0: pass = old == cmparg; break;
    case 1: pass = old != cmparg; break;
    case 2: pass = old < cmparg; break;
    case 3: pass = old <= cmparg; break;
    case 4: pass = old > cmparg; break;
    default: pass = old >= cmparg; break;
    }
    int w2 = pass ? fx_wake(fx_bucket(uaddr2)) : 0;
    (void)val2;
    return (w < val ? w : val) + w2;
}

#define FUTEX_SIGNAL_FIRST_GAMES "com.nekki.vector"
static void fx_kick(int b) { // a signal for a thread waiting in bucket b: wake the bucket
    pthread_mutex_lock(&fx_lock[b]);
    pthread_cond_broadcast(&fx_cond[b]);
    pthread_mutex_unlock(&fx_lock[b]);
}

static void fx_setup(void) {
    fx_init();
    arm_sig_set_futex_waker(fx_kick);
}

int arm_in_sig_handler(void), arm_sig_log_on(void), arm_thread_index(void); // arm_cpu.cpp
static long arm_futex_real(int *uaddr, int op, int val, const struct timespec *ts, int *uaddr2, int val3);
static long arm_futex(int *uaddr, int op, int val, const struct timespec *ts, int *uaddr2, int val3) {
    if (!arm_sig_log_on()) return arm_futex_real(uaddr, op, val, ts, uaddr2, val3);
    const int cmd = op & 0x7f, in_h = arm_in_sig_handler();
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    // DROID_FUTEX_WATCH=<the low 12 bits, hex>: every call on words there (and the one after)
    static long watch = -2;
    if (watch == -2) watch = getenv("DROID_FUTEX_WATCH") ? strtol(getenv("DROID_FUTEX_WATCH"), NULL, 16) : -1;
    const int watched = watch >= 0 && (((uintptr_t)uaddr & 0xff8) == ((uintptr_t)watch & 0xff8));
    if (watched)
        droid_log("fx: thread %d op %d on %p val %d word %d%s", arm_thread_index(), cmd, (void *)uaddr, val,
                  uaddr ? *uaddr : 0, in_h ? " (in a handler)" : "");
    if (in_h) droid_log("sig: thread %d in its handler: futex op %d on %p (val %d, word %d)", arm_thread_index(), cmd,
                        (void *)uaddr, val, uaddr ? *uaddr : 0);
    long r = arm_futex_real(uaddr, op, val, ts, uaddr2, val3);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (watched) droid_log("fx: thread %d op %d on %p -> %ld after %.0f ms (word now %d)", arm_thread_index(), cmd,
                           (void *)uaddr, r, ms, uaddr ? *uaddr : 0);
    if (in_h || ms > 2000)
        droid_log("sig: thread %d futex op %d on %p -> %ld after %.0f ms%s", arm_thread_index(), cmd, (void *)uaddr, r, ms,
                  in_h ? " (in its handler)" : "");
    return r;
}
static long arm_futex_real(int *uaddr, int op, int val, const struct timespec *ts, int *uaddr2, int val3) {
    pthread_once(&fx_once, fx_setup);
    int cmd = op & 0x7f; // without FUTEX_PRIVATE_FLAG (128) / FUTEX_CLOCK_REALTIME (256)
    static unsigned char seen[16];
    if (cmd < 16 && !__atomic_exchange_n(&seen[cmd], 1, __ATOMIC_RELAXED))
        droid_log("arm: futex op %d first used (flags %#x)", cmd, op & ~0x7f);
    unsigned b = fx_bucket(uaddr);
    if (cmd == 5) return fx_wake_op(uaddr, val, (int)(intptr_t)ts, uaddr2, val3);
    if (cmd == 3 || cmd == 4) { // REQUEUE: wake both; waiters re-check (a spurious wake is allowed)
        int w = fx_wake(b);
        if (uaddr2) fx_wake(fx_bucket(uaddr2));
        return w < val ? w : val;
    }
    switch (cmd) {
    case 0:   // FUTEX_WAIT (relative timeout)
    case 9: { // FUTEX_WAIT_BITSET (absolute timeout)
        pthread_mutex_lock(&fx_lock[b]);
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
            pthread_mutex_unlock(&fx_lock[b]);
            errno = EAGAIN;
            return -1;
        }
        // FUTEX_SIGNAL_FIRST_GAMES: the bucket is told before the signal check. Else a GC's pause
        // signal sent between the check and the telling found no bucket to kick, and the thread
        // slept on with it pending: the GC waited for it for ever, the game froze (Vector's
        // 113 MB story download, ~2%, 10 threads "IN syscall(0x62) SIGNAL PENDING", 2026-11-01).
        static int sig_first = -1;
        if (sig_first < 0) sig_first = droid_game_in(FUTEX_SIGNAL_FIRST_GAMES) != 0;
        if (sig_first) arm_sig_wait_futex((int)b);
        if (arm_sig_waiting()) { // a signal first (its handler runs as this call returns)
            if (sig_first) arm_sig_wait_futex(-1);
            pthread_mutex_unlock(&fx_lock[b]);
            errno = EINTR;
            return -1;
        }
        fx_waiters[b]++;
        arm_sig_wait_futex((int)b);
        int rc;
        if (!ts && sig_first) { // (FUTEX_SIGNAL_FIRST_GAMES) in 100 ms slices: a GC's pause signal
            for (;;) {          // whose kick was missed is still seen within 100 ms
                struct timespec abs, slice = { 0, 100000000 };
                clock_gettime(CLOCK_MONOTONIC, &abs);
                ts_add(&abs, &slice);
                rc = pthread_cond_timedwait(&fx_cond[b], &fx_lock[b], &abs);
                if (rc != ETIMEDOUT) break;      // woken (a spurious wake is allowed)
                if (arm_sig_waiting()) { // the signal: EINTR below
                    static int told;
                    if (told++ < 30) droid_log("sig: thread %d: a futex wait saw its pending signal after a missed kick", arm_thread_index());
                    rc = 0;
                    break;
                }
                if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) { rc = 0; break; } // a wake we missed
            }
        } else if (!ts) {
            rc = pthread_cond_wait(&fx_cond[b], &fx_lock[b]);
        } else {
            struct timespec abs;
            clock_gettime(CLOCK_MONOTONIC, &abs);
            if (cmd == 0) {
                ts_add(&abs, ts);
            } else if (op & 256) { // an absolute CLOCK_REALTIME time: as monotonic
                struct timespec now_real, left;
                clock_gettime(CLOCK_REALTIME, &now_real);
                long long ns = (ts->tv_sec - now_real.tv_sec) * 1000000000LL + (ts->tv_nsec - now_real.tv_nsec);
                if (ns < 0) ns = 0;
                left.tv_sec = (time_t)(ns / 1000000000LL);
                left.tv_nsec = (long)(ns % 1000000000LL);
                ts_add(&abs, &left);
            } else {
                abs = *ts;
            }
            rc = pthread_cond_timedwait(&fx_cond[b], &fx_lock[b], &abs);
        }
        fx_waiters[b]--;
        arm_sig_wait_futex(-1);
        pthread_mutex_unlock(&fx_lock[b]);
        if (rc != ETIMEDOUT && arm_sig_waiting()) {
            errno = EINTR;
            return -1;
        }
        if (rc == ETIMEDOUT) {
            errno = ETIMEDOUT;
            return -1;
        }
        return 0;
    }
    case 1:    // FUTEX_WAKE
    case 10: { // FUTEX_WAKE_BITSET
        int w = fx_wake(b);
        return w < val ? w : val;
    }
    }
    droid_log("arm: futex op %d not handled", op);
    errno = ENOSYS;
    return -1;
}

// syscall(SYS_rt_sigaction / SYS_rt_sigprocmask) by number (V8 in Shadow Fight 3 / 4 and
// Vector: answered "not there", it stopped on a fatal check). The kernel's struct sigaction
// on arm64: handler, flags (8 bytes), restorer, mask - not bionic's order.
static long raw_sigaction(int sig, const unsigned char *act, unsigned char *old) {
    uint64_t handler = ~0ull, flags = 0, mask = 0, oh = 0, om = 0;
    int of = 0;
    if (act) {
        memcpy(&handler, act, 8);
        memcpy(&flags, act + 8, 8);
        memcpy(&mask, act + 24, 8);
    }
    if (arm_sig_action(sig, handler, mask, (int)flags, &oh, &om, &of) != 0) return -1;
    if (old) {
        uint64_t f64 = (uint64_t)(uint32_t)of, zero = 0;
        memcpy(old, &oh, 8);
        memcpy(old + 8, &f64, 8);
        memcpy(old + 16, &zero, 8);
        memcpy(old + 24, &om, 8);
    }
    return 0;
}

static long arm_syscall(long n, long a, long b, long c, long d, long e, long f) {
    (void)f;
    switch (n) {
    case 134: return raw_sigaction((int)a, (const unsigned char *)b, (unsigned char *)c); // rt_sigaction
    case 135: return arm_sig_mask((int)a, (const uint64_t *)b, (uint64_t *)c) == 0 ? 0 : -1; // rt_sigprocmask
    case 98: return arm_futex((int *)a, (int)b, (int)c, (const struct timespec *)d, (int *)e, (int)f); // futex
    case 122: return 0;                                // sched_setaffinity
    case 123: memset((void *)c, 0, (size_t)b); if (b) *(unsigned char *)c = 0xff; return 8; // sched_getaffinity
    case 124: return sched_yield();                    // sched_yield
    case 172: return getpid();                         // getpid
    case 178: return (long)((uintptr_t)pthread_self() & 0x7fffffff); // gettid
    case 278: arc4random_buf((void *)a, (size_t)b); return b; // getrandom
    }
    static unsigned char warned[512];
    if (n >= 0 && n < 512 && !warned[n]) {
        warned[n] = 1;
        droid_log("arm: syscall %ld not handled", n);
    }
    errno = ENOSYS;
    return -1;
}

// A raw Linux system call (svc #0 in the game's own code: packers and anti-cheat skip libc):
// by its arm64 number to the function our layer has for it; Linux's -errno on failure.
void *linux_abi_resolve(const char *name);
int droid_linux_errno(int e);
long arm_linux_syscall(long n, const uint64_t *a) {
    extern int droid_host_trace;
    void droid_trace_write(const char *line, int n); // arm_cpu.cpp: trace.txt, a write() a line
    if (droid_host_trace || n == 93 || n == 94 || (n >= 129 && n <= 131)) { // (exits and kills: always)
        static int lines;
        char line[200];
        const int k = snprintf(line, sizeof(line), "rawsys: [%#llx] %ld(%#llx, %#llx, %#llx, %#llx)\n",
                               (unsigned long long)(uintptr_t)pthread_self(), n, (unsigned long long)a[0],
                               (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
        if (lines++ < 200000) droid_trace_write(line, k);
    }
    static const struct { int nr; const char *fn; } calls[] = {
        { 17, "getcwd" }, { 23, "dup" }, { 25, "fcntl" }, { 29, "ioctl" }, { 34, "mkdirat" },
        { 46, "ftruncate" }, { 48, "faccessat" }, { 56, "openat" }, { 57, "close" }, { 61, "getdents64" },
        { 62, "lseek" }, { 63, "read" }, { 64, "write" }, { 65, "readv" }, { 66, "writev" },
        { 67, "pread64" }, { 68, "pwrite64" }, { 78, "readlinkat" }, { 79, "fstatat" }, { 80, "fstat" },
        { 101, "nanosleep" }, { 113, "clock_gettime" }, { 160, "uname" }, { 169, "gettimeofday" },
        { 198, "socket" }, { 203, "connect" }, { 215, "munmap" }, { 216, "mremap" }, { 222, "mmap" },
        { 226, "mprotect" }, { 227, "msync" }, { 233, "madvise" },
    };
    long r = -1;
    int handled = 0;
    switch (n) {
    case 93: case 94: exit((int)a[0]);         // exit / exit_group
    case 117: return 0;                        // ptrace: "not traced"
    case 129: case 130: case 131: return 0;    // kill / tkill / tgkill: nothing to do
    case 134: return raw_sigaction((int)a[0], (const unsigned char *)a[1], (unsigned char *)a[2]) == 0 ? 0 : -22;
    case 135: return arm_sig_mask((int)a[0], (const uint64_t *)a[1], (uint64_t *)a[2]) == 0 ? 0 : -22;
    case 167:                                  // prctl: PR_SET_NAME (thread_place.c)
        if (a[0] == 15) thread_place_named(pthread_self(), (const char *)a[1]);
        return 0;
    case 172: return getpid();
    case 174: case 175: case 176: case 177: return 0; // getuid & co.: root-less 0? an app's uid
    case 178: return (long)((uintptr_t)pthread_self() & 0x7fffffff);
    case 98: case 122: case 123: case 124: case 278: // futex, affinity, yield, getrandom
        r = arm_syscall(n, (long)a[0], (long)a[1], (long)a[2], (long)a[3], (long)a[4], (long)a[5]);
        handled = 1;
        break;
    }
    if (!handled) {
        const char *name = NULL;
        for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
            if (calls[i].nr == n) name = calls[i].fn;
        typedef long (*fn6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        fn6 f = NULL;
        // AT_FDCWD, or a full path (the folder then doesn't count: Hay Day's unpacker)
        if (name && !strcmp(name, "openat") &&
            ((int)a[0] == -100 || (a[1] > 0x10000 && *(const char *)a[1] == '/'))) {
            name = "open";
            static int paths;
            if (paths++ < 16) droid_log("arm: raw openat %s", (const char *)a[1]);
        }
        if (name) f = (fn6)linux_abi_resolve(name);
        if (!f && name) f = (fn6)droid_resolve(name);
        static unsigned char said[512];
        if (n >= 0 && n < 512 && !said[n]) {
            said[n] = 1;
            droid_log("arm: raw syscall %ld (%s)%s", n, name ? name : "?", f ? "" : ": not handled");
        }
        if (!f) return -38; // -ENOSYS
        errno = 0;
        r = name && !strcmp(name, "open") ? f(a[1], a[2], a[3], 0, 0, 0) : f(a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    if (r == -1 && errno) return -droid_linux_errno(errno);
    return r;
}

static unsigned long arm_getauxval(unsigned long type) {
    (void)type;
    return 0; // no HWCAP bits: atomics take the LDXR/STXR path
}

static int arm_system_property_get(const char *name, char *value) {
    (void)name;
    value[0] = 0;
    return 0;
}

static int arm_memfd_create(const char *name, unsigned flags) {
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
}

#define A(name, fn) { name, (void *)fn }
static const struct { const char *name; void *fn; } arm_table[] = {
    A("vsnprintf", arm_vsnprintf), A("vsprintf", arm_vsprintf), A("vasprintf", arm_vasprintf),
    A("vprintf", arm_vprintf), A("vfprintf", arm_vfprintf), A("vsscanf", arm_vsscanf), A("sscanf", arm_sscanf),
    A("__android_log_vprint", arm_android_log_vprint),
    A("eglGetProcAddress", arm_egl_get_proc_address),
    A("pthread_create", arm_pthread_create), A("pthread_exit", arm_pthread_exit),
    A("pthread_once", arm_pthread_once), A("pthread_key_create", arm_pthread_key_create),
    A("pthread_key_delete", arm_pthread_key_delete),
    A("qsort", arm_qsort), A("bsearch", arm_bsearch), A("atexit", arm_atexit),
    A("__cxa_thread_atexit_impl", arm_atexit),
    A("AAudioStreamBuilder_setDataCallback", arm_aaudio_set_data_callback),
    A("scandir", arm_scandir), A("scandir64", arm_scandir),
    A("syscall", arm_syscall), A("getauxval", arm_getauxval),
    A("__system_property_get", arm_system_property_get), A("memfd_create", arm_memfd_create),
};

// Some names link but the console's libc doesn't really have them (the import
// comes out NULL or garbage): only executable addresses go to ARM code.
int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, uint32_t *prot);

static int is_code(void *p) {
    void *start = NULL, *end = NULL;
    uint32_t prot = 0;
    return p && sceKernelQueryMemoryProtection(p, &start, &end, &prot) == 0 && (prot & 0x4);
}

// memcpy / memset up to 256 bytes as ARM code in the guest, bigger ones to the host's (its
// thunk's address in the slot after each routine). A host call costs far more than a small
// copy: Shadow Fight 2's C# AES copied 16 bytes 7.4 million times while it loaded (a
// 16-second frame). title/../scratchpad smallcopy.S, assembled with the NDK's clang:
static const uint32_t small_code[] = { // (sized by its words: a fixed [42] cut strcmp off)
    // copy: x0 dst, x1 src, x2 n
    0xf104005f, 0x540001c8, 0xaa0003e3, 0xf100405f, 0x540000a3, 0xa8c11424, 0xa8811464, 0xd1004042,
    0x17fffffb, 0xb40000a2, 0x38401424, 0x38001464, 0xd1000442, 0x17fffffc, 0xd65f03c0, 0x58000070,
    0xd61f0200, 0xd503201f, 0x00000000, 0x00000000, // copy_slot (words 18-19)
    // fill: x0 dst, w1 byte, x2 n
    0xf104005f, 0x54000208, 0xaa0003e3, 0x92401c24, 0xaa042084, 0xaa044084, 0xaa048084, 0xf100405f,
    0x54000083, 0xa8811064, 0xd1004042, 0x17fffffc, 0xb4000082, 0x38001464, 0xd1000442, 0x17fffffd,
    0xd65f03c0, 0x58000070, 0xd61f0200, 0xd503201f, 0x00000000, 0x00000000, // fill_slot (words 40-41)
    // strcmp (words 42-48), all in ARM code: a byte loop (il2cpp's class lookups by name made
    // 1.5 million host calls while Among Us started)
    0x38401402, 0x38401423, 0x7100045f, 0x7a432040, 0x54ffff80, 0x4b030040, 0xd65f03c0,
    // pthread_getspecific (words 49-56) / pthread_setspecific (57-65) / pthread_self (66-69):
    // the game's per-thread values at tpidr_el0 - 4096 (256 keys), its thread's id at
    // tpidr_el0 - 2048 (arm_cpu.cpp fills them): no host call. Unity's main thread made 19
    // million of these in a Vector run, 800 000 calls a second in its slow menus. asm/keys.S
    0xd53bd041, 0xd1400421, 0x7104001f, 0x54000062, 0xf8605820, 0xd65f03c0, 0xd2800000, 0xd65f03c0,
    0xd53bd042, 0xd1400442, 0x7104001f, 0x54000082, 0xf8205841, 0x52800000, 0xd65f03c0, 0x528002c0,
    0xd65f03c0,
    0xd53bd040, 0xd1200000, 0xf9400000, 0xd65f03c0,
    // setjmp (words 70-83) / longjmp (84-98): the ARM registers a jump keeps (x19-x30, sp, d8-d15)
    // in the game's jmp_buf, as bionic's. The host can't save ARM state from a host call: our
    // longjmp stopped the game (Candy Crush's image decoder, libpng's error path). asm/jmp.S
    0xa9005013, 0xa9015815, 0xa9026017, 0xa9036819, 0xa904701b, 0xa905781d, 0x910003e1, 0xf9003001,
    0x6d06a408, 0x6d07ac0a, 0x6d08b40c, 0x6d09bc0e, 0x52800000, 0xd65f03c0,
    0xa9405013, 0xa9415815, 0xa9426017, 0xa9436819, 0xa944701b, 0xa945781d, 0xf9403002, 0x9100005f,
    0x6d46a408, 0x6d47ac0a, 0x6d48b40c, 0x6d49bc0e, 0x7100003f, 0x1a9f1420, 0xd65f03c0,
    // memmove (words 99-128) up to 256 bytes, forward or backward as the two overlap; bigger to
    // the host's (memmove_slot: words 127-128). Sonic Dash's il2cpp made 3 million 1-byte
    // memmoves in one 300 ms hitch, each a host call. asm/memmove.S
    0xf104005f, 0x54000308, 0xaa0003e3, 0xcb010004, 0xeb02009f, 0x54000183, 0xf100405f, 0x540000a3,
    0xa8c11825, 0xa8811865, 0xd1004042, 0x17fffffb, 0xb4000182, 0x38401425, 0x38001465, 0xd1000442,
    0x17fffffc, 0x8b020021, 0x8b020063, 0xb40000a2, 0x385ffc25, 0x381ffc65, 0xd1000442, 0x17fffffc,
    0xd65f03c0, 0x58000070, 0xd61f0200, 0xd503201f, 0x00000000, 0x00000000,
};
#define ARM_MEMMOVE_GAMES "com.sega.sonicdash,com.sega.sonicboomandroid" // memmove in ARM code
#define ARM_JMP_GAMES "com.king.candycrushsaga" // setjmp / longjmp in ARM code
static int is_setjmp(const char *n) { return !strcmp(n, "setjmp") || !strcmp(n, "_setjmp") || !strcmp(n, "sigsetjmp"); }
static int is_longjmp(const char *n) { return !strcmp(n, "longjmp") || !strcmp(n, "_longjmp") || !strcmp(n, "siglongjmp"); }
void *droid_jit_alloc(size_t size); // arm_cpu.cpp
static void *small_in_guest(const char *name, uint64_t thunk) {
    static uint32_t *code;
    if (!code && (code = droid_jit_alloc(sizeof(small_code)))) {
        memcpy(code, small_code, sizeof(small_code));
        droid_log("arm: memcpy / memset up to 256 bytes run as ARM code (no host call)");
    }
    if (!code) return (void *)thunk;
    if (!strcmp(name, "strcmp")) return &code[42];
    if (!strcmp(name, "pthread_getspecific")) return &code[49];
    if (!strcmp(name, "pthread_setspecific")) return &code[57];
    if (!strcmp(name, "pthread_self")) return &code[66];
    if (is_setjmp(name)) return &code[70];
    if (is_longjmp(name)) return &code[84];
    if (!strcmp(name, "memmove")) {
        memcpy(&code[127], &thunk, 8);
        return &code[99];
    }
    int copy = !strcmp(name, "memcpy");
    memcpy(&code[copy ? 18 : 40], &thunk, 8);
    return &code[copy ? 0 : 20];
}

// What one call from ARM code into ours costs (once, at the first frame): ARM code calling an
// empty host function 200 000 times, less the same loop calling an empty ARM function.
// asm/bench.S: bench(n, fn) calls fn n times; nop at word 10
static const uint32_t bench_code[] = {
    0xa9be7bfd, 0xa90153f3, 0xaa0003f3, 0xaa0103f4, 0xd63f0280, 0xf1000673, 0x54ffffc1, 0xa94153f3,
    0xa8c27bfd, 0xd65f03c0, 0xd65f03c0,
};
static uint64_t bench_nop_host(void) { return 0; }
static double bench_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
void arm_bench_calls(void) {
    static int done;
    if (done++) return;
    uint32_t *c = droid_jit_alloc(sizeof(bench_code));
    if (!c) return;
    memcpy(c, bench_code, sizeof(bench_code));
    uint64_t thunk = arm_thunk("bench_nop", (void *)bench_nop_host);
    typedef uint64_t (*bench_fn)(uint64_t, uint64_t);
    bench_fn b = (bench_fn)arm_callback((uint64_t)c);
    const uint64_t n = 200000;
    b(1000, thunk); // translated first
    b(1000, (uint64_t)&c[10]);
    double t0 = bench_now();
    b(n, thunk);
    double t1 = bench_now();
    b(n, (uint64_t)&c[10]);
    double t2 = bench_now();
    droid_log("arm: a call from ARM code into ours costs %.0f ns (an ARM call %.1f ns)", ((t1 - t0) - (t2 - t1)) / n,
              (t2 - t1) / n);
}

// the APK scanner's question (tools/apk_scan.py, DROID_KNOWN_IMPORTS=<file>): of the names in
// the file, one a line, the ones we have - written to <file>.known. The same lookups as below.
void *vk_bridge_dlsym(const char *name); // vulkan_bridge.c: the vk* names a game asks dlsym for
int arm_imports_known(const char *in_path) {
    FILE *in = fopen(in_path, "r");
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s.known", in_path);
    FILE *out = in ? fopen(out_path, "w") : NULL;
    if (!out) {
        if (in) fclose(in);
        return 1;
    }
    char name[256];
    int n = 0, have = 0;
    while (fgets(name, sizeof(name), in)) {
        name[strcspn(name, "\r\n")] = 0;
        if (!*name) continue;
        n++;
        void *p = arm_modules_resolve_dl(name);
        for (size_t i = 0; i < sizeof(arm_table) / sizeof(arm_table[0]) && !p; i++)
            if (strcmp(arm_table[i].name, name) == 0) p = arm_table[i].fn;
        if (!p) p = linux_abi_resolve(name);
        if (!p) p = droid_resolve(name);
        if (!p && !strncmp(name, "vk", 2)) p = vk_bridge_dlsym(name);
        if (p) {
            fprintf(out, "%s\n", name);
            have++;
        }
    }
    fclose(in);
    fclose(out);
    fprintf(stderr, "known imports: %d of %d\n", have, n);
    void java_known_dump(FILE *f); // java_layer.c: the Java methods we answer, to <file>.java
    snprintf(out_path, sizeof(out_path), "%s.java", in_path);
    if ((out = fopen(out_path, "w"))) {
        java_known_dump(out);
        fclose(out);
    }
    return 0;
}

// The wrapped functions (callbacks made callable, Linux layouts): for dlsym too, which found
// the plain ones (Genshin Impact's AAudio from dlsym: its data callback called as PS5 code)
void *arm_table_find(const char *name) {
    for (size_t i = 0; i < sizeof(arm_table) / sizeof(arm_table[0]); i++)
        if (strcmp(arm_table[i].name, name) == 0) return arm_table[i].fn;
    return NULL;
}

void *arm_import(const char *name, int is_data) {
    void *p = arm_modules_resolve_dl(name); // dlopen & co: the APK's libraries
    if (!p) { // a 32-bit wchar_t's wide functions, for WIDE32_GAMES (wide32.c; the console's are 16-bit)
        void *wide32_resolve(const char *name);
        int wide32_on(void);
        p = !strcmp(name, "swprintf") ? (wide32_on() ? (void *)arm_swprintf : NULL) : wide32_resolve(name);
    }
    for (size_t i = 0; i < sizeof(arm_table) / sizeof(arm_table[0]) && !p; i++)
        if (strcmp(arm_table[i].name, name) == 0) p = arm_table[i].fn;
    if (!p) p = linux_abi_resolve(name);
    if (!p) p = droid_resolve(name);
    // data: as the loader says, and the media key names (untyped imports: they came out as functions)
    if (is_data || !strncmp(name, "AMEDIAFORMAT_KEY_", 17)) return p;
    if (p && !is_code(p)) {
        droid_log("import %s: %p is not code on this console", name, p);
        p = NULL;
    }
    if (!p) droid_log("missing import: %s (ARM stub returns 0)", name);
    uint64_t thunk = arm_thunk(name, p);
    // (strcmp too, words 42-48: Among Us then crashed at start, a bad method from Unity - not yet)
    const int host_keys = getenv("DROID_HOST_KEYS") != NULL; // the old way (host calls), to compare
    const int host_strcmp = getenv("DROID_HOST_STRCMP") != NULL;
    if (thunk && (!strcmp(name, "memcpy") || !strcmp(name, "memset") || (!strcmp(name, "strcmp") && !host_strcmp) ||
                  (!host_keys && (!strcmp(name, "pthread_getspecific") || !strcmp(name, "pthread_setspecific") ||
                                  !strcmp(name, "pthread_self"))) ||
                  ((is_setjmp(name) || is_longjmp(name)) && droid_game_in(ARM_JMP_GAMES)) ||
                  (!strcmp(name, "memmove") && droid_game_in(ARM_MEMMOVE_GAMES))))
        return small_in_guest(name, thunk);
    return (void *)thunk;
}
