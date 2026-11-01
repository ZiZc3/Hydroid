// Test library: built by the NDK for x86_64 (phase 0) and AArch64 (phase 5,
// run in the JIT). droid_arm_imports() calls PS5 functions through imports.

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT int32_t droid_hello(int32_t a, int32_t b) { return a + b; }

EXPORT const char *droid_version(void) { return "Android test lib v0.2"; }

int droid_not_there(int x); // nobody exports it: a missing import

static char last[256];

static int format(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap); // an ARM64 va_list crosses over
    va_end(ap);
    return r;
}

// Each test sets one bit; the result says which ones passed.
EXPORT uint32_t droid_arm_imports(void) {
    uint32_t ok = 0;
    char buf[128];

    if (strlen("hello ps5") == 9) ok |= 1u << 0;

    char *p = malloc(1000);
    if (p) {
        memset(p, 'A', 999);
        p[999] = 0;
        if (strlen(p) == 999) ok |= 1u << 1;
        free(p);
    }

    snprintf(buf, sizeof(buf), "%d-%s-%.2f-%x", 42, "ps5", 1.5, 255);
    if (strcmp(buf, "42-ps5-1.50-ff") == 0) ok |= 1u << 2;
    strcpy(last, buf);

    // 11 arguments: x6, x7 and the guest stack
    snprintf(buf, sizeof(buf), "%d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8);
    if (strcmp(buf, "1 2 3 4 5 6 7 8") == 0) ok |= 1u << 3;

    format(buf, sizeof(buf), "%s %d %.1f %ld %c", "va", 7, 2.5, 1234567890123L, 'z');
    if (strcmp(buf, "va 7 2.5 1234567890123 z") == 0) ok |= 1u << 4;

    volatile float f = 16.0f;
    volatile double d = 2.0;
    if (sqrtf(f) == 4.0f && pow(d, 10.0) == 1024.0) ok |= 1u << 5;

    errno = 0;
    int fd = open("/app0/no-such-file", O_RDONLY);
    if (fd < 0 && errno != 0) ok |= 1u << 6;

    int a = 0, b = 0;
    if (sscanf("12 34", "%d %d", &a, &b) == 2 && a == 12 && b == 34) ok |= 1u << 7;

    if (droid_not_there(5) == 0) ok |= 1u << 8;

    __android_log_print(ANDROID_LOG_INFO, "arm64", "hello from ARM64: %d %s %.1f (%s)", 7, "x",
                        2.5, last);
    ok |= 1u << 9; // reached: the log call came back
    return ok;
}

// ---- 5c: PS5 code calling ARM code back, ARM threads ------------------------

#include <pthread.h>

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int once_runs;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void once_fn(void) { once_runs++; }

static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static long locked_count, atomic_count;
static int dtor_runs;
static pthread_key_t key;
static void key_dtor(void *p) {
    (void)p;
    __atomic_fetch_add(&dtor_runs, 1, __ATOMIC_SEQ_CST);
}

struct job {
    int id;
    long sum;
    uintptr_t tp;
    char text[32];
};

static void *worker(void *arg) {
    struct job *j = arg;
    j->tp = (uintptr_t)__builtin_thread_pointer();
    pthread_setspecific(key, j);
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&count_lock);
        locked_count++;
        pthread_mutex_unlock(&count_lock);
        __atomic_fetch_add(&atomic_count, 1, __ATOMIC_SEQ_CST);
        j->sum += i;
    }
    snprintf(j->text, sizeof(j->text), "thread %d %.1f", j->id, j->id * 0.5);
    return (void *)(intptr_t)(j->id + 100);
}

static void *exiter(void *arg) {
    (void)arg;
    pthread_exit((void *)77);
    return NULL;
}

EXPORT uint32_t droid_arm_callbacks(void) {
    uint32_t ok = 0;

    int v[8] = { 5, 3, 8, 1, 9, 2, 7, 4 };
    qsort(v, 8, sizeof(int), cmp_int); // PS5 qsort calls our ARM compare
    int sorted = 1;
    for (int i = 0; i < 8; i++) sorted &= v[i] == i + 1 + (i >= 5);
    if (sorted) ok |= 1u << 0;

    pthread_once(&once, once_fn);
    pthread_once(&once, once_fn);
    if (once_runs == 1) ok |= 1u << 1;

    pthread_key_create(&key, key_dtor);
    struct job jobs[4];
    pthread_t t[4];
    int created = 0;
    for (int i = 0; i < 4; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].id = i;
        if (pthread_create(&t[i], NULL, worker, &jobs[i]) == 0) created++;
    }
    if (created == 4) ok |= 1u << 2;
    int joined = 1;
    for (int i = 0; i < created; i++) {
        void *r = NULL;
        if (pthread_join(t[i], &r) != 0 || (intptr_t)r != i + 100) joined = 0;
    }
    if (joined && created == 4) ok |= 1u << 3;
    if (locked_count == 80000 && atomic_count == 80000) ok |= 1u << 4;
    int sums = 1;
    for (int i = 0; i < 4; i++) sums &= jobs[i].sum == 199990000L;
    if (sums && strcmp(jobs[3].text, "thread 3 1.5") == 0) ok |= 1u << 5;
    uintptr_t me = (uintptr_t)__builtin_thread_pointer();
    int distinct = me != 0;
    for (int i = 0; i < 4; i++) {
        distinct &= jobs[i].tp != 0 && jobs[i].tp != me;
        for (int k = 0; k < i; k++) distinct &= jobs[i].tp != jobs[k].tp;
    }
    if (distinct) ok |= 1u << 6;

    pthread_t e;
    void *er = NULL;
    if (pthread_create(&e, NULL, exiter, NULL) == 0 && pthread_join(e, &er) == 0 &&
        (intptr_t)er == 77)
        ok |= 1u << 7;
    if (dtor_runs == 4) ok |= 1u << 8;

    __android_log_print(ANDROID_LOG_INFO, "arm64",
                        "callbacks: sorted %d once %d threads %d/%d counts %ld/%ld \"%s\" dtors %d",
                        sorted, once_runs, created, joined, locked_count, atomic_count,
                        jobs[3].text, dtor_runs);
    return ok;
}
