// thread_place.c — the console's cores for the game: its main thread and its render thread
// each with a whole core to itself (no other thread allowed there; it may still use the rest)
// at a high priority, the game's other threads (Unity's job workers, audio) on the rest. Busy
// job threads no longer take turns with the two threads a frame waits for. XPSemu's pinning,
// the same calls. A game thread made by a placed one would share its core: each starts on the
// rest (thread_place_new). settings.txt cores=0 or DROID_NO_PLACE=1: off.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_settings.h"

void droid_log(const char *fmt, ...);
void arm_thread_role(const char *role);

#ifndef DROID_PC
int scePthreadGetaffinity(pthread_t thread, unsigned long long *mask);
int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
int scePthreadSetprio(pthread_t thread, int prio);
#endif

#define PLACE_PRIO 256 // the console's highest for a game thread (XPSemu: its CPU and GPU threads)

static int state;  // 0 not set up, 1 on, -1 off
static int active; // a thread placed: new threads go to the rest from then on
static unsigned long long all_cpus, rest_cpus;
static int core_cpu[2] = { -1, -1 }; // the main thread's CPU, the render thread's
static pthread_t placed[2];
static int is_placed[2];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const role_names[2] = { "main", "render" };

static int get_mask(unsigned long long *m) {
#ifdef DROID_PC
    cpu_set_t s;
    CPU_ZERO(&s);
    if (pthread_getaffinity_np(pthread_self(), sizeof(s), &s)) return -1;
    *m = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &s)) *m |= 1ull << i;
    return 0;
#else
    return scePthreadGetaffinity(pthread_self(), m);
#endif
}

static int set_mask(pthread_t t, unsigned long long m) {
#ifdef DROID_PC
    cpu_set_t s;
    CPU_ZERO(&s);
    for (int i = 0; i < 64; i++)
        if (m >> i & 1) CPU_SET(i, &s);
    return pthread_setaffinity_np(t, sizeof(s), &s);
#else
    return scePthreadSetaffinity(t, m);
#endif
}

static int set_prio(pthread_t t) {
#ifdef DROID_PC
    (void)t;
    return 0; // the PC: the same cores, its own priorities
#else
    return scePthreadSetprio(t, PLACE_PRIO);
#endif
}

// once, on the app's first thread: whole cores from the top for the two, the rest for the others
void thread_place_init(void) {
    if (state) return;
    state = -1;
    if (getenv("DROID_NO_PLACE")) {
        droid_log("cores: placement off (DROID_NO_PLACE)");
        return;
    }
    if (get_mask(&all_cpus) || !all_cpus) {
        droid_log("cores: no CPU mask, placement off");
        return;
    }
    unsigned long long reserved = 0;
    int found = 0, whole = 0;
    for (int k = 31; k >= 0; k--) {
        unsigned long long core = 3ull << (2 * k);
        if ((all_cpus & core) != core) continue;
        whole++;
        if (found < 2) {
            core_cpu[found++] = 2 * k;
            reserved |= core;
        }
    }
    rest_cpus = all_cpus & ~reserved;
    if (found < 2 || whole < 4) { // too few cores left for the job threads
        droid_log("cores: CPUs %#llx (%d whole cores), placement off", all_cpus, whole);
        return;
    }
    state = 1;
    droid_log("cores: CPUs %#llx: the main thread's core CPUs %d-%d, the render thread's %d-%d (priority %d), "
              "the others on %#llx (settings.txt cores=0: off)", all_cpus, core_cpu[0], core_cpu[0] + 1,
              core_cpu[1], core_cpu[1] + 1, PLACE_PRIO, rest_cpus);
}

// 0: the game's main thread, 1: its render thread
static void place(pthread_t t, int role, const char *why) {
    if (state != 1 || role < 0 || role > 1) return;
    if (!settings.cores) {
        static int said;
        if (!said++) droid_log("cores: placement off for this game (settings.txt cores=0)");
        return;
    }
    active = 1;
    pthread_mutex_lock(&lock);
    if (is_placed[role] && !pthread_equal(placed[role], t)) set_mask(placed[role], rest_cpus); // replaced
    if (is_placed[role] && pthread_equal(placed[role], t)) {
        pthread_mutex_unlock(&lock);
        return;
    }
    placed[role] = t;
    is_placed[role] = 1;
    pthread_mutex_unlock(&lock);
    // its own core and the rest: never fewer CPUs than before (a single CPU of its own made threads it
    // started, the drivers' among them, share it: the PC's Among Us went from 48 to 9 fps)
    const unsigned long long mask = 3ull << core_cpu[role] | rest_cpus;
    int rc = set_mask(t, mask);
    int rp = rc == 0 ? set_prio(t) : -1;
    droid_log("cores: %s thread (%s): CPUs %d-%d its own, and the others' %#llx (rc %#x), priority %d (rc %#x)",
              role_names[role], why, core_cpu[role], core_cpu[role] + 1, rest_cpus, rc, PLACE_PRIO, rp);
}

void thread_place_role(int role, const char *why) { place(pthread_self(), role, why); }

// a thread named (pthread_setname_np, prctl PR_SET_NAME): the engines' render threads by name
void thread_place_named(pthread_t t, const char *name) {
    if (!name) return;
    static int logged;
    if (logged < 64) {
        logged++;
        droid_log("threads: %#llx named '%.40s'", (unsigned long long)(uintptr_t)t, name);
    }
    if (pthread_equal(t, pthread_self())) { // its name in our thread reports
        void arm_warm_named(const char *name); // arm_cpu.cpp: Warm, its translate-ahead list by name
        arm_warm_named(name);
        static char names[96][24];
        static int n;
        pthread_mutex_lock(&lock);
        int i = n < 96 ? n++ : -1;
        pthread_mutex_unlock(&lock);
        if (i >= 0) {
            snprintf(names[i], sizeof(names[i]), "%s", name);
            arm_thread_role(names[i]);
        }
    }
    if (!strncmp(name, "UnityGfxDevice", 14) || !strcmp(name, "GLThread") || !strncmp(name, "RenderThread", 12))
        place(t, 1, name);
}

// a game thread starting (arm_cpu.cpp, guest_thread_main): on the rest - one made by a placed
// thread would share its core. (Not a pthread_create wrap: the console's platform layer wraps it
// already, for its threads' stacks.)
void thread_place_new(void) {
    if (active) set_mask(pthread_self(), rest_cpus);
}
// a helper thread of ours (arm_cpu.cpp's warm threads): on the rest from its start, placement on or
// not yet (they start before the game's main thread is placed, and would take its core)
void thread_place_rest(void) {
    if (state == 1 && settings.cores) set_mask(pthread_self(), rest_cpus);
}
