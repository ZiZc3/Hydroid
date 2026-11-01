#ifndef ARM_CPU_H
#define ARM_CPU_H

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs the ARM64 function at fn with up to 8 integer/pointer arguments
// (AArch64 calling convention: x0-x7) until it returns; the result is x0.
// *ok is 0 when the code hit something the translator couldn't run.
uint64_t arm_call(uint64_t fn, const uint64_t *args, int count, int *ok);
// up to 8 integer args, 8 float (s0-s7) and 16 on the stack
uint64_t arm_call_full(uint64_t fn, const uint64_t *x, int nx, const float *f, int nf, const uint64_t *stk, int nstk,
                       int *ok);

// An ARM64-callable address for the PS5 function fn ("svc #n; ret" stub;
// the same name always gets the same stub). fn NULL = missing import: the
// stub logs its name on the first call and returns 0.
uint64_t arm_thunk(const char *name, void *fn);
int arm_thunk_total(void);
uint64_t arm_thunk_set(const char *name, void *fn); // a thunk whose function comes later (or changes)
void arm_thunk_null_arg(const char *name, int idx);  // that argument reaches the host as 0
void arm_code_changed(uint64_t addr, uint64_t len);  // we patched ARM code: old translations dropped

// For the crash report: is addr in a JIT code cache, this thread's last PS5
// function called from ARM code, and how deep in ARM calls the thread is.
int arm_in_jit(uint64_t addr);
const char *arm_last_import(void);
// The thread's last PS5 calls from ARM code (i = 0 oldest); 0 = no more.
int arm_trace_get(int i, const char **name, uint64_t *lr, uint64_t *a0, uint64_t *a1, uint64_t *a2,
                  uint64_t *ret);
int arm_depth(void);
int arm_guest_sp_range(uint64_t *sp, uint64_t *top); // this thread's ARM stack: sp to top; 0 = none
uint64_t arm_guest_reg(int i);  // x0-x30 of this thread's ARM code now
void arm_crash_state(void);

// Signals for ARM code (arm_cpu.cpp): handlers (bionic's numbers, 64-bit masks), sending,
// masks, sigsuspend; the waits tell where they wait, so a signal can wake them.
int arm_sig_action(int sig, uint64_t handler, uint64_t mask, int flags, uint64_t *old_handler, uint64_t *old_mask,
                   int *old_flags); // handler ~0: only asking
int arm_sig_kill(pthread_t t, int sig); // 0, or an errno (ESRCH...)
int arm_sig_mask(int how, const uint64_t *set, uint64_t *old);
int arm_sig_suspend(uint64_t mask); // -1, EINTR, after a handler ran
void arm_sig_set_futex_waker(void (*fn)(int bucket));
void arm_sig_wait_futex(int bucket);           // -1: not waiting
void arm_sig_wait_cond(void *cond, void *mutex); // NULL: not waiting
void arm_sig_take(void);
void arm_sig_point(void);                      // a thread of ours between waits: its signals
void arm_thread_role(const char *role);          // what a thread of ours is (the report)                       // run them now (inside a blocking wait)
int arm_sig_waiting(void);                      // a signal to take now // a crash report: the ARM registers, stack and last calls of this thread
// Logs what every ARM thread is doing (which PS5 function it is in, from where).
void arm_threads_report(void);

// Logs the top PS5 functions by time spent in them (all threads) since the
// last call; elapsed_ms = wall time since then (calibrates the TSC).
void arm_profile_dump(double elapsed_ms, int top);
uint64_t arm_thread_blocked_cycles(void); // this thread's time in waits (rdtsc)

// soloader's resolver for an ARM64 library (src/arm_imports.c): functions
// become thunks, data (is_data) stays a plain pointer.
void *arm_import(const char *name, int is_data);

// A host function pointer that runs the ARM64 function guest (x86 trampoline;
// args rdi..r9 + 2 stack words -> x0-x7, xmm0-7 -> v0-v7; one per address).
// A thunk address gives back the PS5 function itself. NULL stays NULL.
void *arm_callback(uint64_t guest);

// An ARM64 thread: a PS5 thread running start(arg) in its own JIT + TLS.
int arm_pthread_create(pthread_t *thread, const pthread_attr_t *attr, uint64_t start, uint64_t arg);
void arm_pthread_exit(uint64_t value);

// Set when the game's library is ARM64: the layers (activity, looper,
// OpenSL ES) then bridge what the game hands them.
extern int droid_arm_game;

// When PS5 code calls ARM code with arg0 == from, the ARM code gets to
// instead (our JNIEnv / JavaVM -> their ARM tables).
void arm_swap_arg0(uint64_t from, uint64_t to);
// Java mode on ART: x0 of every ARM->host call goes through to_host, of every host->ARM
// callback through to_guest (the JNIEnv/JavaVM pointers differ on each side).
void arm_arg0_hooks(uint64_t (*to_host)(uint64_t), uint64_t (*to_guest)(uint64_t));
// This thunk returns a jboolean: the ARM code gets its low byte only (0 / 1)
void arm_thunk_ret8(const char *name);

// The JNIEnv* / JavaVM* an ARM64 game gets: FalsoJNI's tables as thunks.
void *arm_jni_env(void);
void *arm_java_vm(void);

#ifdef __cplusplus
}
#endif

#endif
