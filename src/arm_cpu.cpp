// arm_cpu.cpp — ARM64 code from an Android game, run by dynarmic (suyu-emu/
// dynarmic, 0BSD) on the PS5's x86-64. Guest memory is our own memory: an ARM
// address is the same pointer here, so a game's .so is loaded once by soloader
// and both sides share every buffer.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <algorithm>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef DROID_PC
#include <sys/uio.h>
#include <sys/mman.h>
#endif
#include <fcntl.h>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "arm_cpu.h"

extern "C" void droid_log(const char *fmt, ...);
extern "C" void arm_keys_thread_end(std::uint64_t *values); // arm_imports.c: a thread's key destructors
extern "C" int64_t sceKernelGetDirectMemorySize(void);
extern "C" int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align,
                                             int type, int64_t *phys);
extern "C" int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *at,
                                                  size_t *size);
extern "C" int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys,
                                        size_t align);
extern "C" int sceKernelMprotect(const void *addr, size_t len, int prot);
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t *size);
extern "C" int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, std::uint32_t *prot);

// ---- the code cache dynarmic writes x86 into (patched block_of_code.cpp) ---

// Code cache ranges, for the crash report.
static std::uint64_t jit_ranges[512][2];
static std::atomic<int> jit_range_count{ 0 };

extern "C" int arm_in_jit(std::uint64_t addr) {
    for (int i = 0, n = jit_range_count.load(); i < n; i++)
        if (addr >= jit_ranges[i][0] && addr < jit_ranges[i][1]) return 1;
    return 0;
}

// range i of the code cache and our ARM stubs (thunks, callbacks): 0 past the last
extern "C" int arm_jit_range(int i, std::uint64_t *from, std::uint64_t *to) {
    if (i < 0 || i >= std::min(jit_range_count.load(), 512)) return 0;
    *from = jit_ranges[i][0];
    *to = jit_ranges[i][1];
    return 1;
}

extern "C" void *droid_va_hint(size_t bytes); // linux_abi.c: out of RADV's GPU window

// The code cache's memory type, measured once (the translator spends ~80% of a translation
// writing x86 there): writes into 1 MB of each type that maps read-write-execute, against the
// game's own memory. (tools.txt "JitBench": mortar_host.c runs it.)
extern "C" void arm_jit_bench(void) {
#if defined(__PROSPERO__)
    auto ns = [] {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (std::int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
    };
    auto run = [&](volatile std::uint8_t *m, const char *what) {
        const size_t n = 1 << 20;
        std::int64_t t0 = ns();
        for (int pass = 0; pass < 16; pass++) std::memset((void *)m, pass, n); // big writes
        std::int64_t t1 = ns();
        for (int pass = 0; pass < 4; pass++)
            for (size_t i = 0; i < n; i++) m[i] = (std::uint8_t)i; // byte writes, as Xbyak's db()
        std::int64_t t2 = ns();
        std::uint64_t sum = 0;
        for (size_t i = 0; i < n; i += 8) sum += *(volatile std::uint64_t *)(m + i);
        std::int64_t t3 = ns();
        droid_log("arm: JitBench %s: memset %.0f MB/s, byte writes %.0f MB/s, reads %.0f MB/s (%llu)", what,
                  16.0 * 1e9 / (double)(t1 - t0), 4.0 * 1e9 / (double)(t2 - t1), 1e9 / (double)(t3 - t2),
                  (unsigned long long)(sum & 1));
    };
    std::uint8_t *heap = static_cast<std::uint8_t *>(std::malloc(1 << 20));
    if (heap) run(heap, "game memory (malloc)");
    std::free(heap);
    const int types[] = { 12, 0, 1, 2, 3, 5, 8, 10 };
    for (int type : types) {
        int64_t phys = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), 1 << 20, 0x10000, type, &phys) != 0) {
            droid_log("arm: JitBench type %d: no memory of it", type);
            continue;
        }
        void *p = droid_va_hint(1 << 20);
        if (sceKernelMapDirectMemory(&p, 1 << 20, 0x03, 0, phys, 0x10000) != 0) {
            droid_log("arm: JitBench type %d: map failed", type);
            continue;
        }
        const int x = sceKernelMprotect(p, 1 << 20, 0x07);
        char what[64];
        snprintf(what, sizeof(what), "type %d (%s)", type, x == 0 ? "read-write-execute" : "no execute");
        run(static_cast<std::uint8_t *>(p), what);
    }
#endif
}

extern "C" void *droid_jit_alloc(size_t size) {
    size = (size + 0xFFFF) & ~size_t(0xFFFF);
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 0x10000, 12, &phys) != 0)
        return nullptr;
    void *p = droid_va_hint(size); // out of RADV's GPU window (linux_abi.c)
    if (sceKernelMapDirectMemory(&p, size, 0x03, 0, phys, 0x10000) != 0) return nullptr;
    if (sceKernelMprotect(p, size, 0x07) != 0) return nullptr;
    const int r = jit_range_count.fetch_add(1);
    if (r < 512) {
        jit_ranges[r][0] = reinterpret_cast<std::uint64_t>(p);
        jit_ranges[r][1] = reinterpret_cast<std::uint64_t>(p) + size;
    }
    // all the caches so far, and the console's direct memory still free (a game out of it: a
    // SIGBUS somewhere, no message)
    static std::atomic<std::uint64_t> jit_total{ 0 };
    const std::uint64_t total = jit_total.fetch_add(size) + size;
    int64_t free_at = 0;
    size_t free_size = 0;
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &free_at, &free_size);
    droid_log("arm: JIT code cache %zu KB at %p (caches %llu MB in all, direct memory free: %zu MB)", size >> 10, p,
              (unsigned long long)(total >> 20), free_size >> 20);
    return p;
}

extern "C" void droid_jit_free(void *p) { (void)p; } // lives as long as the title
namespace { extern std::atomic<std::uint64_t> translated_instructions; }
extern "C" std::uint64_t arm_translated_take(void) { return translated_instructions.exchange(0); } // since last asked
namespace { extern std::atomic<std::uint64_t> exclusive_calls; }
extern "C" std::uint64_t arm_exclusive_take(void) { return exclusive_calls.exchange(0); }
namespace { extern std::atomic<std::uint64_t> gc_stops; extern std::atomic<std::int64_t> gc_stop_slowest_ns; }
namespace { extern std::atomic<std::uint64_t> gc_collections; extern std::atomic<std::int64_t> gc_world_ns, gc_world_longest_ns; }
// the GC stops since last asked (threads stopped) and the slowest one's delay in ms
extern "C" std::uint64_t arm_gc_stops_take(double *slowest_ms) {
    *slowest_ms = double(gc_stop_slowest_ns.exchange(0)) / 1e6;
    return gc_stops.exchange(0);
}
// the collections since last asked, the time the game stood still for them, the longest (ms)
extern "C" std::uint64_t arm_gc_collections_take(double *stopped_ms, double *longest_ms) {
    *stopped_ms = double(gc_world_ns.exchange(0)) / 1e6;
    *longest_ms = double(gc_world_longest_ns.exchange(0)) / 1e6;
    return gc_collections.exchange(0);
}

// A thread's ARM stack: the console's direct memory, as the code caches. malloc's would come
// out of the small pool thread stacks and malloc share (Shadow Fight 3: 57 threads at 8 MB
// each, and the console refused the next thread, "Thread creation failed"). Never freed:
// the JITs and their stacks are kept for the next thread.
static std::uint8_t *stack_alloc(size_t size) {
    // 64 KB more above the top, mapped: on Android the thread's own data sits there, and code
    // that walks a thread's stack reads up to (and past) its top (Genshin Impact: a read at the
    // first byte past a stack's end, which was unmapped here)
    size = ((size + 0xFFFF) & ~size_t(0xFFFF)) + 0x10000;
    int64_t phys = 0;
    void *p = droid_va_hint(size);
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 0x10000, 12, &phys) == 0 &&
        sceKernelMapDirectMemory(&p, size, 0x03, 0, phys, 0x10000) == 0) {
        static std::atomic<int> told{ 0 };
        if (getenv("DROID_MM_LOG") && told.fetch_add(1) < 300) // where each stack is (a read past one's end)
            droid_log("mm: stack %p - %p (%zu KB)", p, static_cast<char *>(p) + size, size >> 10);
        return static_cast<std::uint8_t *>(p);
    }
    return new (std::nothrow) std::uint8_t[size];
}

// Calls a PS5 function with an ARM64 call's registers: x0-x5 -> rdi..r9,
// x6/x7 and 8 guest stack words -> the x86 stack, v0-v7 -> xmm0-7 (raw bits,
// so float, double and SIMD all pass). Android's AAPCS64 puts `...` args in
// registers by class too, so printf-style calls need nothing extra. Returns
// rax; xmm0 goes to *v0_out.
extern "C" std::uint64_t arm_host_call(void *fn, const std::uint64_t *x, const void *v,
                                       void *v0_out, const std::uint64_t *guest_sp);
asm(R"(
    .text
    .p2align 4
    .globl arm_host_call
    .type arm_host_call,@function
arm_host_call:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    sub $80, %rsp
    mov %rdi, %r11
    mov %rcx, %rbx
    mov 48(%rsi), %rax
    mov %rax, 0(%rsp)
    mov 56(%rsi), %rax
    mov %rax, 8(%rsp)
    mov 0(%r8), %rax
    mov %rax, 16(%rsp)
    mov 8(%r8), %rax
    mov %rax, 24(%rsp)
    mov 16(%r8), %rax
    mov %rax, 32(%rsp)
    mov 24(%r8), %rax
    mov %rax, 40(%rsp)
    mov 32(%r8), %rax
    mov %rax, 48(%rsp)
    mov 40(%r8), %rax
    mov %rax, 56(%rsp)
    mov 48(%r8), %rax
    mov %rax, 64(%rsp)
    mov 56(%r8), %rax
    mov %rax, 72(%rsp)
    movups 0(%rdx), %xmm0
    movups 16(%rdx), %xmm1
    movups 32(%rdx), %xmm2
    movups 48(%rdx), %xmm3
    movups 64(%rdx), %xmm4
    movups 80(%rdx), %xmm5
    movups 96(%rdx), %xmm6
    movups 112(%rdx), %xmm7
    mov %rsi, %r10
    mov 0(%r10), %rdi
    mov 8(%r10), %rsi
    mov 16(%r10), %rdx
    mov 24(%r10), %rcx
    mov 32(%r10), %r8
    mov 40(%r10), %r9
    mov $8, %eax
    call *%r11
    movups %xmm0, (%rbx)
    mov %rdx, 16(%rbx)
    lea -16(%rbp), %rsp
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size arm_host_call, .-arm_host_call
)");

namespace {

using Dynarmic::A64::Vector;
using VAddr = std::uint64_t;
std::atomic<std::uint64_t> translated_instructions{ 0 };
std::atomic<std::uint64_t> exclusive_calls{ 0 };

extern "C" long arm_linux_syscall(long n, const std::uint64_t *a); // arm_imports.c
extern "C" int arm_trace_enter(int id, std::uint64_t *x, std::uint64_t *sp); // arm_trace.c
extern "C" int arm_trace_return(std::uint64_t *x);
constexpr std::uint32_t SVC_RETURN = 0xFFFF; // ours: svc #0 is a game's raw Linux system call
constexpr size_t STACK_BYTES = 8 << 20;        // the main and the first busy threads' ARM stack (Android: 8 MB main)
constexpr size_t SMALL_STACK_BYTES = 2 << 20;  // the many others' (Android gives a thread 1 MB)
constexpr size_t NESTED_STACK_BYTES = 512 << 10; // ARM code called back from inside a PS5 call
// Translated x86, per JIT (real memory, all of it, up front). The first threads are the
// game's busy ones (Unity's main, render and loading threads): a big cache, so it doesn't
// fill (full: everything is translated again, a 1-3 s freeze in play). The many others
// (workers, timers, network) run little code: a small one. (Subway Surfers: 55 threads at
// 64 MB were 3.5 GB, and its main thread still filled its 64 MB after 3 minutes.)
constexpr size_t BIG_CACHE_BYTES = 256 << 20;
constexpr int BIG_CACHES = 4;
constexpr size_t CACHE_BYTES = 32 << 20;
// (Smaller caches for late threads were tried for Genshin Impact's 50+ threads: Unity's job
// workers start late and run much code - Among Us fell from 60 to 1 fps, every cache refilling.)
constexpr size_t NESTED_CACHE_BYTES = 16 << 20;
// (256: a thread can hold two, its own and a nested one; Angry Birds 2's ~60 threads used all of
// 128 and the GC's signal then had none - "no JIT left", the game stopped on its loading screen.
// The ones after the first 128 have small caches: the console's memory)
constexpr size_t MAX_CPUS = 256;
constexpr size_t FIRST_CPUS = 128;
constexpr size_t LATE_CACHE_BYTES = 8 << 20, LATE_NESTED_CACHE_BYTES = 4 << 20;
constexpr int MAX_DEPTH = 8;
// arm_host_call copies 8 words above the guest SP: keep them inside the stack.
constexpr size_t STACK_TOP_PAD = 256;
constexpr size_t TLS_WORDS = 1024;

// ---- imports: one "svc #n; ret" stub per PS5 function an ARM library calls ----

// Java mode on ART: the JNIEnv/JavaVM pointers at the ARM border (arm_arg0_hooks)
static std::uint64_t (*arg0_to_host)(std::uint64_t);
static std::uint64_t (*arg0_to_guest)(std::uint64_t);

struct Thunk {
    const char *name;
    void *fn; // nullptr: a missing import (logged on first call, returns 0)
    std::atomic<bool> warned;
    std::atomic<std::uint64_t> calls, cycles; // profile: time spent in this PS5 function
    int blocking = -1; // a wait (the thread isn't working in it); -1 not known yet
    int takes_lock = -1; // it takes a lock (mutex / rwlock lock): no signal right after it
    int null_arg = -1; // this argument is passed as 0 (a pointer to ARM callbacks the host can't call)
    int sret = -1;     // returns a big struct through x8 (mallinfo): x8 goes in as the first argument
    int ret8 = 0;      // returns a jboolean: only its low byte (x86 leaves the rest of rax as it was)
};

// This thread's time in waits (cond waits, futex, sleeps): work = the rest.
thread_local std::uint64_t blocked_cycles;
bool is_blocking(const char *n) {
    static const char *const waits[] = { "pthread_cond_wait", "pthread_cond_timedwait", "syscall", "usleep",
                                         "nanosleep", "sched_yield", "sem_wait", "sem_timedwait",
                                         "pthread_join", "epoll_wait", "poll", "select", "eglSwapBuffers" };
    for (const char *w : waits)
        if (std::strcmp(n, w) == 0) return true;
    return false;
}

constexpr int MAX_THUNKS = 8000; // 8 bytes each: one 64 KB page
Thunk thunks[MAX_THUNKS];
std::atomic<int> thunk_count{ 0 };
std::mutex thunk_lock;
std::uint32_t *stub_page;

// The address a guest function returns to: "svc #0" ends arm_call.
alignas(16) const std::uint32_t return_stub[4] = { 0xD41FFFE1, 0xD41FFFE1, 0xD41FFFE1, 0xD41FFFE1 }; // svc #0xffff

template <typename T> T load(VAddr a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof(T));
    return v;
}

template <typename T> void store(VAddr a, T v) {
    std::memcpy(reinterpret_cast<void *>(a), &v, sizeof(T));
}

thread_local const char *last_import;

static void where(std::uint64_t a, char *out, size_t n); // below: lib+offset
// the host-call trace (droid_host_trace: tap.txt's 4th number) goes to trace.txt in the app's
// folder (beside tap.txt) with a write() per line: what the kernel has survives the console
// killing the app with no crash report (the live log's and android.log's last lines are lost then)
#ifndef APP0
#define APP0 "/app0"
#endif
extern "C" void droid_trace_write(const char *line, int n);
static void trace_write(const char *line, int n) { droid_trace_write(line, n); }
extern "C" void droid_trace_write(const char *line, int n) {
    static int fd = -2;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    if (fd == -2) fd = open(APP0 "/trace.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0 && n > 0) (void)!write(fd, line, (size_t)n);
}
extern "C" int droid_host_trace = 0; // arm_modules.c: trace a library's init calls
static std::atomic<int> host_trace_lines{ 0 };

// AddressSanitizer's death callback: the crashing thread's last ARM->host call (thread_local
// last_import is still valid there), which names the host library function that faulted.
extern "C" void __sanitizer_set_death_callback(void (*)()) __attribute__((weak));
extern "C" void droid_log_flush(void); // app_main.c
static void asan_death_cb() {
    extern thread_local const char *last_import;
    droid_log("asan-death: last ARM->host call on this thread: %s", last_import ? last_import : "(none)");
    // this thread's last calls (the crashing one last), then the log out at once: the process ends
    const char *tname;
    std::uint64_t tlr, a0, a1, a2, ret;
    int n = 0;
    while (arm_trace_get(n, &tname, &tlr, &a0, &a1, &a2, &ret)) n++;
    for (int i = n > 8 ? n - 8 : 0; i < n && arm_trace_get(i, &tname, &tlr, &a0, &a1, &a2, &ret); i++)
        droid_log("asan-death:   %s(%#llx, %#llx, %#llx) from %#llx", tname, (unsigned long long)a0,
                  (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)tlr);
    droid_log_flush();
}
__attribute__((constructor)) static void asan_death_register() {
    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(asan_death_cb);
}

// Every thread that runs ARM code, for the "what is everyone doing" report.
struct ThreadInfo {
    std::atomic<const char *> call{ nullptr }; // the PS5 function it is in, or last called
    std::atomic<int> in_call{ 0 };
    std::atomic<std::uint64_t> lr{ 0 };       // called from (guest address)
    std::atomic<std::uint64_t> sp{ 0 };       // its ARM stack at the last host call
    std::atomic<std::uint64_t> calls{ 0 };
    std::atomic<std::uint64_t> args[4]; // the call's first arguments (a timed wait's timeout...)
    // signals (arm_sig_*): the host thread, what's pending, how deep in ARM code it is
    std::atomic<pthread_t> self{};
    std::atomic<int> tid{ 0 }; // the PC's thread id (who owns a lock: the report)
    std::atomic<std::uint64_t> pending{ 0 };
    std::atomic<int> depth{ 0 };
    std::atomic<void *> running{ nullptr }; // the Cpu running its ARM code now
    std::atomic<bool> claimed{ false }, alive{ false };
    // where it waits, so a signal can wake it: a futex bucket (arm_imports.c), a condition
    std::atomic<int> fx_bucket{ -1 };
    std::atomic<void *> wait_cond{ nullptr }, wait_mutex{ nullptr };
    std::mutex sig_m; // sigsuspend's wait
    std::condition_variable sig_cv;
    std::atomic<std::uint64_t> blocked{ 0 };   // its signal mask (a copy, for the report)
    std::atomic<const char *> role{ nullptr }; // a thread of ours: what it is (arm_thread_role)
    // the thread report's sample of ARM code running without calls: where it is
    std::atomic<int> sampled{ 0 };
    std::atomic<std::uint64_t> sample_pc{ 0 }, sample_lr{ 0 };
    std::atomic<std::uint64_t> translated{ 0 }; // ARM instructions it translated (the speed report)
    std::atomic<std::uint64_t> translate_ns{ 0 }; // the time that took (dynarmic's compile, timed)
    std::atomic<std::int64_t> stop_sent_ns{ 0 }; // a GC's stop signal sent to it, not yet taken
};

// The GC's stops (SIGPWR 30 from Unity's collector): how long a thread took to take it - the
// whole game waits for the slowest (a thread in a long loop of ARM code without calls)
std::atomic<std::uint64_t> gc_stops{ 0 };
std::atomic<std::int64_t> gc_stop_slowest_ns{ 0 };
// a whole collection: its first stop signal to its first restart (24): the game stood still
std::atomic<std::int64_t> gc_world_since{ 0 };
std::atomic<std::uint64_t> gc_collections{ 0 };
std::atomic<std::int64_t> gc_world_ns{ 0 }, gc_world_longest_ns{ 0 };
static std::int64_t mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr int MAX_THREAD_INFO = 256;
ThreadInfo thread_infos[MAX_THREAD_INFO];
std::atomic<int> thread_info_count{ 0 };
thread_local ThreadInfo *my_info;
thread_local std::uint64_t sig_blocked; // this thread's signal mask (bit n-1 = signal n)

// A slot per thread running ARM code; a guest thread that ends gives its own back. A thread
// that ran ARM code after that (its TLS destructors: free...) gives its slot back at its real
// end (a host key's destructor). A pthread id is used again by a later thread: a slot still
// holding it is a dead thread's, and a signal for the new thread must not go there (Among Us
// stuttered: the GC's pause sent to a dead slot, waiting for its retry).
static pthread_key_t slot_key;
static pthread_once_t slot_once = PTHREAD_ONCE_INIT;
static void slot_end(void *p) {
    auto *info = static_cast<ThreadInfo *>(p);
    if (!info || info == &thread_infos[MAX_THREAD_INFO - 1]) return;
    info->alive.store(false);
    info->running.store(nullptr);
    info->role.store(nullptr);
    info->claimed.store(false);
    if (info == my_info) my_info = nullptr; // on its own thread (the key destructor runs there)
}
static void slot_key_make() { pthread_key_create(&slot_key, slot_end); }

ThreadInfo *thread_info() {
    if (!my_info) {
        pthread_once(&slot_once, slot_key_make);
        const pthread_t me = pthread_self();
        const int seen = std::min(thread_info_count.load(), MAX_THREAD_INFO);
        for (int i = 0; i < seen; i++) // a dead thread's slot with our id: not alive any more
            if (thread_infos[i].alive.load() && pthread_equal(thread_infos[i].self.load(), me)) {
                thread_infos[i].alive.store(false);
                thread_infos[i].running.store(nullptr);
                thread_infos[i].claimed.store(false);
            }
        for (int i = 0; i < MAX_THREAD_INFO && !my_info; i++) {
            bool free_slot = false;
            if (thread_infos[i].claimed.compare_exchange_strong(free_slot, true)) {
                my_info = &thread_infos[i];
                int n = thread_info_count.load();
                while (n < i + 1 && !thread_info_count.compare_exchange_weak(n, i + 1)) {
                }
            }
        }
        if (!my_info) my_info = &thread_infos[MAX_THREAD_INFO - 1];
        my_info->pending.store(0);
        my_info->calls.store(0);
        my_info->self.store(pthread_self());
#ifdef DROID_PC
        my_info->tid.store((int)syscall(186)); // SYS_gettid (x86-64)
#endif
        my_info->blocked.store(0);
        my_info->role.store(nullptr);
        my_info->alive.store(true);
        pthread_setspecific(slot_key, my_info);
    }
    return my_info;
}

void thread_info_release() {
    if (!my_info || my_info == &thread_infos[MAX_THREAD_INFO - 1]) return;
    pthread_setspecific(slot_key, nullptr);
    slot_end(my_info);
    my_info = nullptr;
}

// ---- signals for ARM code (the GC pauses threads with them) -----------------------------
// sigaction keeps the game's handler; pthread_kill marks the signal pending on its thread
// and wakes it (stops its translated code, wakes its wait); the thread runs the handler
// itself, at a safe point: after a PS5 call returns, or where its code was stopped. Its
// frame (siginfo, ucontext with the registers) goes on the thread's own ARM stack below
// its sp, as a kernel's would: the GC finds the registers there when it scans the stack.
struct SigAct {
    std::atomic<std::uint64_t> handler{ 0 }, mask{ 0 };
    std::atomic<int> flags{ 0 };
};
SigAct sig_acts[65];
std::atomic<bool> sig_used{ false };
void (*futex_waker)(int bucket);
constexpr int L_SA_SIGINFO = 4, L_SA_NODEFER = 0x40000000;
constexpr unsigned L_SA_RESETHAND = 0x80000000u;

struct Cpu;

// ---- code the game rewrote (a JIT of its own: V8 in Vector / Shadow Fight 4) ----------
// ARM code that writes code flushes it with IC IVAU (a cache line) or IC IALLU (all). Our
// translations of what was there before are then wrong: every thread's translators drop
// those ranges (each thread has its own: the flushing thread's at once, the others' at
// their next stop - they're poked - or their next call into ARM code). Before: none did,
// V8's fresh code ran as the old one: a BRK, or a jump to nothing.
constexpr std::uint64_t INV_RING = 8192;
struct InvRange { std::uint64_t start, len; }; // len 0: everything
InvRange inv_ring[INV_RING];
std::atomic<std::uint64_t> inv_head{ 0 }; // ranges added so far
std::mutex inv_lock;
void deliver_signals(Cpu *cur);
// a lock call: 1 it takes one (0: taken), -1 it gives one back, 0 neither. (A cond wait gives
// its mutex back and takes it again: neither.)
static int takes_a_lock(const char *n) {
    static const char *const takes[] = { "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_timedlock",
                                         "pthread_mutex_lock_timeout_np", "pthread_rwlock_rdlock",
                                         "pthread_rwlock_wrlock", "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock",
                                         "pthread_rwlock_timedrdlock", "pthread_rwlock_timedwrlock",
                                         "pthread_spin_lock", "pthread_spin_trylock", nullptr };
    static const char *const gives[] = { "pthread_mutex_unlock", "pthread_rwlock_unlock", "pthread_spin_unlock", nullptr };
    // a cond wait: its mutex given back while it waits (on a phone the pause lands in there):
    // as it returns, the pause may run when that mutex is the only one held
    static const char *const waits[] = { "pthread_cond_wait", "pthread_cond_timedwait", "pthread_cond_timedwait_monotonic_np",
                                         "pthread_cond_timedwait_relative_np", "pthread_cond_timedwait_monotonic", nullptr };
    for (int i = 0; waits[i]; i++)
        if (!std::strcmp(n, waits[i])) return 2;
    for (int i = 0; takes[i]; i++)
        if (!std::strcmp(n, takes[i])) return 1;
    for (int i = 0; gives[i]; i++)
        if (!std::strcmp(n, gives[i])) return -1;
    return 0;
}
// the locks this thread holds (taken through our calls): the GC's pause waits for none
// miHoYo's games only (Noelle GC): every other game takes its signal at once, as before (Vector
// on the PS5: UnityMain held a lock while waiting for Firebase's, the pause never ran, the
// world stayed stopped)
thread_local int locks_held;
extern "C" int droid_quirks; // app_main.c, QUIRK_MIHOYO 1 (android_shim.h)
static inline int locks_defer(void) { return (droid_quirks & 1) ? locks_held : 0; }
extern "C" int arm_locks_held(void) { return locks_defer(); } // (bionic_shim.c's waits)

// Warm (translate-ahead, below): this thread's key and its once-only step
thread_local char warm_key[128], warm_base[96]; // "start function|name"; the start function
thread_local int warm_pending; // named: its list to translate at the next stop
extern "C" void arm_warm_ahead(void *cpu); // (below: its Cpu)

// The last PS5 calls of this thread, for the crash report.
struct TraceEntry {
    int idx;
    std::uint64_t lr, a0, a1, a2, ret;
};
// CallDiff (the debug tool): every host call written out, to line up a PC run with a console run
extern "C" int calldiff_on(void);
extern "C" void calldiff_log(const char *name, std::uint64_t lr, const std::uint64_t *x, std::uint64_t ret);
constexpr unsigned TRACE_N = 256;
thread_local TraceEntry trace_ring[TRACE_N];
thread_local unsigned trace_pos;

struct Cpu final : Dynarmic::A64::UserCallbacks {
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::uint64_t inv_seen = 0; // of inv_ring: applied up to here
    std::uint64_t tpidr_el0 = 0;
    std::uint64_t tpidrro_el0 = 0;
    struct { std::uint8_t *p; std::uint8_t *get() const { return p; } } stack{ nullptr };
    size_t stack_bytes = 0;
    bool failed = false;

    // A fault in fastmem lands here (dynarmic retries the access through these):
    // NULL-ish or non-canonical addresses stop the JIT with a report instead of
    // crashing the console.
    std::uint64_t bad_addr = 0;
    int bad_write = 0;
    bool bad(VAddr a, int write) {
        // only a fastmem fault gets here: on the console, an address not mapped at all is a
        // report with the game's registers too, not a crash of ours (V8 read its sandbox
        // where nothing was mapped: Vector, Shadow Fight 3 / 4 died in MemoryRead64)
        if (a >= 0x100000 && a < 0x0000800000000000ull && mapped(a)) return false; // nothing of ours below 1 MB
        if (!bad_addr) {
            bad_addr = a ? a : 1;
            bad_write = write;
        }
        failed = true;
        jit->HaltExecution();
        return true;
    }
    static bool mapped(VAddr a) {
#ifdef DROID_PC
        (void)a;
        return true;
#else
        // the regions the console said are mapped, kept: an instruction dynarmic sent to these
        // callbacks once comes back on every access - a kernel call each time was 1 fps (Vector)
        struct Region { std::uint64_t from, to; };
        static thread_local Region seen[16];
        static thread_local int next;
        for (const Region &r : seen)
            if (a >= r.from && a < r.to) return true;
        void *start = nullptr, *end = nullptr;
        std::uint32_t prot = 0;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void *>(a), &start, &end, &prot) != 0 || !(prot & 3))
            return false;
        seen[next++ & 15] = { reinterpret_cast<std::uint64_t>(start), reinterpret_cast<std::uint64_t>(end) };
        return true;
#endif
    }
    template <typename T> T rd(VAddr a) {
        if (heapguard_hit(a, sizeof(T), 0)) return T{};
        return bad(a, 0) ? T{} : load<T>(a);
    }
    template <typename T> void wr(VAddr a, T v) {
        if (heapguard_hit(a, sizeof(T), 1)) return;
        if (!bad(a, 1)) store(a, v);
    }
    bool heapguard_hit(VAddr a, size_t n, int write);
    std::uint8_t MemoryRead8(VAddr a) override { return rd<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(VAddr a) override { return rd<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(VAddr a) override { return rd<std::uint32_t>(a); }
    // the translator reads each instruction it translates through here: counted (a stutter
    // with many is new code being translated)
    std::optional<std::uint32_t> MemoryReadCode(VAddr a) override {
        translated_instructions.fetch_add(1, std::memory_order_relaxed);
        if (my_info) my_info->translated.fetch_add(1, std::memory_order_relaxed);
        return rd<std::uint32_t>(a);
    }
    std::uint64_t MemoryRead64(VAddr a) override { return rd<std::uint64_t>(a); }
    Vector MemoryRead128(VAddr a) override { return rd<Vector>(a); }
    void MemoryWrite8(VAddr a, std::uint8_t v) override { wr(a, v); }
    void MemoryWrite16(VAddr a, std::uint16_t v) override { wr(a, v); }
    void MemoryWrite32(VAddr a, std::uint32_t v) override { wr(a, v); }
    void MemoryWrite64(VAddr a, std::uint64_t v) override { wr(a, v); }
    void MemoryWrite128(VAddr a, Vector v) override { wr(a, v); }

    template <typename T> bool exclusive(VAddr a, T v, T expected) {
        exclusive_calls.fetch_add(1, std::memory_order_relaxed); // the speed report: atomics out of the JIT
        return __atomic_compare_exchange_n(reinterpret_cast<T *>(a), &expected, v, false,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive8(VAddr a, std::uint8_t v, std::uint8_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive16(VAddr a, std::uint16_t v, std::uint16_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive32(VAddr a, std::uint32_t v, std::uint32_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive64(VAddr a, std::uint64_t v, std::uint64_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive128(VAddr a, Vector v, Vector e) override {
        return __atomic_compare_exchange(reinterpret_cast<unsigned __int128 *>(a),
                                         reinterpret_cast<unsigned __int128 *>(&e),
                                         reinterpret_cast<unsigned __int128 *>(&v), false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    void InterpreterFallback(VAddr pc, size_t n) override {
        const std::uint32_t op = load<std::uint32_t>(pc);
        if ((op & 0xFFFFFFE0u) == 0xD53BE040u) { // mrs Xt, cntvct_el0: the timer, as cntpct
            // (a CPU probe in Among Us: its failing left a thread with every signal blocked,
            // the GC's pause waiting on it forever)
            const unsigned rt = op & 31;
            if (rt != 31) jit->SetRegister(rt, GetCNTPCT());
            jit->SetPC(pc + 4);
            static std::atomic<int> told{ 0 };
            if (told.fetch_add(1) == 0) droid_log("arm: cntvct_el0 read (the timer)");
            return;
        }
        droid_log("arm: no translation for %zu instruction(s) at %#llx (%08x)", n,
                  (unsigned long long)pc, load<std::uint32_t>(pc));
        failed = true;
        jit->HaltExecution();
    }

    void CallSVC(std::uint32_t swi) override {
        if (swi == SVC_RETURN) {
            jit->HaltExecution();
            return;
        }
        if ((swi >= 0xF000 && swi < 0xF100) || swi == 0xFFFD) { // the debug tool's traps (arm_trace.c, XTool)
            std::uint64_t x[31];
            for (int r = 0; r < 31; r++) x[r] = jit->GetRegister(r);
            std::uint64_t sp = jit->GetSP();
            const int done = swi == 0xFFFD ? arm_trace_return(x) : arm_trace_enter(int(swi - 0xF000), x, &sp);
            if (!done) {
                droid_log("arm: trace trap #%#x with nothing to do", swi);
                failed = true;
                jit->HaltExecution();
                return;
            }
            for (int r = 0; r < 31; r++) jit->SetRegister(r, x[r]);
            jit->SetSP(sp);
            return;
        }
        if (swi == 0) { // a raw Linux system call (packers, anti-cheat): x8 the number, x0-x5
            std::uint64_t a[6];
            for (int r = 0; r < 6; r++) a[r] = jit->GetRegister(r);
            jit->SetRegister(0, std::uint64_t(arm_linux_syscall(long(jit->GetRegister(8)), a)));
            return;
        }
        const int idx = int(swi) - 1;
        if (idx < 0 || idx >= thunk_count.load(std::memory_order_acquire)) {
            droid_log("arm: svc #%u not handled", swi);
            failed = true;
            jit->HaltExecution();
            return;
        }
        Thunk &t = thunks[idx];
        last_import = t.name;
        TraceEntry &te = trace_ring[trace_pos++ % TRACE_N];
        te.idx = idx;
        te.lr = jit->GetRegister(30);
        te.a0 = jit->GetRegister(0);
        te.a1 = jit->GetRegister(1);
        te.a2 = jit->GetRegister(2);
        te.ret = 0;
        if (!t.fn) {
            if (!t.warned.exchange(true)) droid_log("arm: missing import called: %s", t.name);
            jit->SetRegister(0, 0);
            return;
        }
        std::uint64_t x[8];
        Vector v[8];
        Vector v0[2]{}; // xmm0, then rdx
        for (int i = 0; i < 8; i++) {
            x[i] = jit->GetRegister(i);
            v[i] = jit->GetVector(i);
        }
        int trace_this = droid_host_trace && host_trace_lines.fetch_add(1) < 200000;
        if (trace_this) {
            char at[96];
            where(te.lr, at, sizeof(at));
            char line[320];
            const int n = std::snprintf(line, sizeof(line), "hostcall: [%#llx] %s(%#llx, %#llx, %#llx, %#llx) from %s\n",
                                        (unsigned long long)(uintptr_t)pthread_self(), t.name,
                                        (unsigned long long)x[0], (unsigned long long)x[1],
                                        (unsigned long long)x[2], (unsigned long long)x[3], at);
            trace_write(line, n);
        }
        if (arg0_to_host) x[0] = arg0_to_host(x[0]);
        if (t.null_arg >= 0 && t.null_arg < 8) x[t.null_arg] = 0; // e.g. Vulkan's pAllocator: ARM code
        if (t.sret < 0) t.sret = !std::strcmp(t.name, "mallinfo");
        if (t.sret) x[0] = jit->GetRegister(8);
        {
            static int trace = -1;
            if (trace < 0) trace = getenv("DROID_TRACE_HOST") != NULL;
            if (trace) droid_log("hc: %s", t.name);
        }
        ThreadInfo *info = thread_info();
        info->call.store(t.name, std::memory_order_relaxed);
        info->lr.store(te.lr, std::memory_order_relaxed);
        for (int a = 0; a < 4; a++) info->args[a].store(x[a], std::memory_order_relaxed);
        info->calls.fetch_add(1, std::memory_order_relaxed);
        info->sp.store(jit->GetSP(), std::memory_order_relaxed);
        info->in_call.store(1, std::memory_order_relaxed);
        const std::uint64_t t0 = __builtin_ia32_rdtsc();
        const std::uint64_t r = arm_host_call(t.fn, x, v, v0,
                                              reinterpret_cast<const std::uint64_t *>(jit->GetSP()));
        info->in_call.store(0, std::memory_order_relaxed);
        t.calls.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t spent = __builtin_ia32_rdtsc() - t0;
        t.cycles.fetch_add(spent, std::memory_order_relaxed);
        if (t.blocking < 0) t.blocking = is_blocking(t.name);
        if (t.blocking) blocked_cycles += spent;
        const std::uint64_t r8 = t.ret8 ? (r & 0xff) : r; // (ARM code reads the whole w0 / x0)
        te.ret = r8;
        if (calldiff_on()) calldiff_log(t.name, te.lr, x, r8);
        if (trace_this) {
            char line[160];
            const int n = std::snprintf(line, sizeof(line), "  [%#llx] %s -> %#llx\n",
                                        (unsigned long long)(uintptr_t)pthread_self(), t.name, (unsigned long long)r);
            trace_write(line, n);
            if (!std::strcmp(t.name, "access") || !std::strcmp(t.name, "stat") ||
                !std::strcmp(t.name, "open") || !std::strcmp(t.name, "openat")) {
                const char *path = reinterpret_cast<const char *>(x[0]);
                size_t n = strnlen(path, 240);
                droid_log("hostcall: %s path \"%s\" -> %lld", t.name, path, (long long)r);
            } else if (!std::strcmp(t.name, "vsnprintf")) {
                const char *out = reinterpret_cast<const char *>(x[0]);
                size_t n = strnlen(out, x[1] ? (size_t)x[1] : 1);
                droid_log("hostcall: vsnprintf out \"%s\"", out);
                (void)n;
            }
        }
        jit->SetRegister(0, r8);
        jit->SetRegister(1, v0[1][0]);
        jit->SetVector(0, v0[0]);
        // a signal is run here, as the call returns - but not while the thread holds a lock: the
        // GC's pause would hold it with the lock, and a thread the GC doesn't pause waits for the
        // lock while the GC waits for that thread (Genshin Impact: Noelle Main in
        // pthread_mutex_lock, the main thread waiting for Noelle, the world stopped). It runs as
        // the call that gives the last one back returns - or 200 ms on (a count gone wrong).
        if (t.takes_lock < 0) t.takes_lock = takes_a_lock(t.name);
        if ((t.takes_lock == 1 || t.takes_lock == -1) && (int)r == 0) locks_held += t.takes_lock;
        if (locks_held < 0) locks_held = 0;
        if (sig_used.load(std::memory_order_relaxed) && (info->pending.load(std::memory_order_acquire) & ~sig_blocked)) {
            const std::int64_t sent = info->stop_sent_ns.load(std::memory_order_relaxed);
            if (locks_defer() == 0 || (sent && mono_ns() - sent > 200000000)) deliver_signals(this);
        }
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A64::Exception e) override {
        using E = Dynarmic::A64::Exception;
        switch (e) { // hints (spin-wait loops): carry on, as yuzu does
        case E::WaitForInterrupt:
        case E::WaitForEvent:
        case E::Yield:
            // as cheap as on the phone: a pause, and only every 64th a real sched_yield (a
            // kernel call on the console). Unity's job workers spin "yield, any work?" a set
            // number of times before they sleep: each yield a sched_yield made that spin last
            // many times longer, half of all CPU samples in Vector / Shadow Fight 3, cores the
            // main and render threads needed. DROID_YIELD_SYSCALL=1: every one (the old way)
            if ((++yields & 63) == 0 || yield_syscall) sched_yield();
            else __builtin_ia32_pause();
            return;
        case E::SendEvent:
        case E::SendEventLocal:
            return;
        default:
            break;
        }
        droid_log("arm: exception %d at %#llx", (int)e, (unsigned long long)pc);
        failed = true;
        jit->HaltExecution();
    }

    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, VAddr va) override;
    void InstructionSynchronizationBarrierRaised() override;
    std::uint64_t inv_marked = 0; // IC lines this thread added since its last ISB
    std::uint32_t yields = 0;     // YIELD / WFE hints run (every 64th a sched_yield)
    const bool yield_syscall = getenv("DROID_YIELD_SYSCALL") != nullptr;

    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return ~std::uint64_t(0) >> 1; }
    std::uint64_t GetCNTPCT() override { return __builtin_ia32_rdtsc(); }
};

// ---- HeapGuard (the debug tool): the game's code writing over our memory ------------------
// PC runner built with HEAPGUARD=1 (AddressSanitizer), tools.txt / DROID_TOOLS "HeapGuard":
// fastmem off, every ARM load and store through the callbacks above, and each one checked
// against AddressSanitizer's map: a freed block or a block's guard bytes is the game's code
// writing where it mustn't (V8's games corrupted dynarmic's own tree). The first ones are
// logged with where in the game they come from, and the access is skipped.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DROID_ASAN 1
#endif
#endif
#ifdef DROID_ASAN
#include <sanitizer/asan_interface.h>
#endif
extern "C" int droid_tool(const char *name); // game_settings.c: the debug tools switched on
extern "C" const char *java_package;          // jni_impl.c (set before the game's code runs)
static bool heapguard_on() {
    static int on = -1;
    if (on < 0) {
        on = droid_tool("HeapGuard") || getenv("DROID_HEAPGUARD") != nullptr;
#ifndef DROID_ASAN
        if (on) droid_log("HeapGuard: this build has no AddressSanitizer (PC: HEAPGUARD=1 bash pc/build-pc.sh): off");
        on = 0;
#else
        if (on) droid_log("HeapGuard: on - every ARM memory access checked (slow)");
#endif
    }
    return on;
}
bool Cpu::heapguard_hit(VAddr a, size_t n, int write) {
#ifdef DROID_ASAN
    if (!__asan_region_is_poisoned(reinterpret_cast<void *>(a), n)) return false;
    static std::atomic<int> told{ 0 };
    if (told.fetch_add(1) < 20) {
        char at[96], lr[96];
        where(jit->GetPC(), at, sizeof(at));
        where(jit->GetRegister(30), lr, sizeof(lr));
        droid_log("HeapGuard: the game %s %zu bytes at %#llx - freed or guard memory of our heap - near %s "
                  "(called from %s)", write ? "WRITES" : "reads", n, (unsigned long long)a, at, lr);
        if (told.load() == 1) __asan_describe_address(reinterpret_cast<void *>(a)); // stdout.log: freed by whom, or past what
    }
    return true;
#else
    (void)a; (void)n; (void)write;
    return false;
#endif
}

// this translator: the ranges added since it last looked, dropped (neighbouring lines as one)
static void inv_apply(Cpu *c) {
    const std::uint64_t head = inv_head.load(std::memory_order_acquire);
    if (c->inv_seen == head) return;
    if (head - c->inv_seen > INV_RING) { // too many since: all of it
        c->jit->ClearCache();
        c->inv_seen = head;
        return;
    }
    std::uint64_t from = 0, to = 0;
    for (; c->inv_seen < head; c->inv_seen++) {
        const InvRange r = inv_ring[c->inv_seen % INV_RING];
        if (!r.len) { // everything
            c->jit->ClearCache();
            from = to = 0;
            c->inv_seen = head - 1;
            continue;
        }
        if (to && r.start <= to && r.start + r.len >= from) { // touching the one so far: joined
            from = std::min(from, r.start);
            to = std::max(to, r.start + r.len);
            continue;
        }
        if (to) c->jit->InvalidateCacheRange(from, to - from);
        from = r.start;
        to = r.start + r.len;
    }
    if (to) c->jit->InvalidateCacheRange(from, to - from);
}

// the other threads running ARM code: stopped a moment, to apply the list (run, below)
static void inv_kick_others() {
    const int n = std::min(thread_info_count.load(), MAX_THREAD_INFO);
    for (int i = 0; i < n; i++) {
        ThreadInfo &t = thread_infos[i];
        if (&t == my_info || !t.alive.load()) continue;
        if (void *c = t.running.load()) static_cast<Cpu *>(c)->jit->HaltExecution(Dynarmic::HaltReason::UserDefined4);
    }
}

void Cpu::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, VAddr va) {
    {
        std::lock_guard<std::mutex> lk(inv_lock);
        const std::uint64_t n = inv_head.load(std::memory_order_relaxed);
        if (op == Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU)
            inv_ring[n % INV_RING] = { va & ~std::uint64_t(63), 64 };
        else
            inv_ring[n % INV_RING] = { 0, 0 };
        inv_head.store(n + 1, std::memory_order_release);
    }
    static std::atomic<int> told{ 0 };
    if (told.fetch_add(1) == 0)
        droid_log("arm: the game rewrites its own code (a JIT of its own): translations dropped as it does");
    // not at once: a JIT flushes its new code a 64-byte line at a time (V8: 64 for a 4 KB
    // page), then an ISB, as ARM wants before running it. Each line stopped every thread (60
    // in Shadow Fight 3): the game's stutters. Now all at the ISB; a flush that never has
    // one goes after 256 lines
    if (++inv_marked >= 256) InstructionSynchronizationBarrierRaised();
}

void Cpu::InstructionSynchronizationBarrierRaised() {
    if (!inv_marked) return;
    inv_marked = 0;
    inv_apply(this); // this thread's (dynarmic applies it on leaving the block)
    inv_kick_others();
}

// our own patch of ARM code (etc_native.cpp): its old translations dropped by every translator
extern "C" void arm_code_changed(uint64_t addr, uint64_t len) {
    {
        std::lock_guard<std::mutex> lk(inv_lock);
        const std::uint64_t n = inv_head.load(std::memory_order_relaxed);
        inv_ring[n % INV_RING] = { addr, len ? len : 4 };
        inv_head.store(n + 1, std::memory_order_release);
    }
    inv_kick_others();
}

Dynarmic::ExclusiveMonitor monitor(MAX_CPUS);

// JITs are kept when a thread ends and handed to the next one: dynarmic
// can't share one between threads, and each owns its code cache.
std::mutex pool_lock;
std::vector<Cpu *> pools[2]; // [0] top level, [1] nested
size_t cpus_made;

size_t top_made; // top-level JITs made so far (the first ones get the big cache)

// the game's main thread: an 8 MB stack and the big code cache whatever came first (Genshin
// Impact starts dozens of threads while it loads: its main thread then got a 2 MB stack and ran
// out of it in its first frame)
thread_local bool want_big;

// The exclusive monitor kept exact for every JIT made from now on (game setting "atomics";
// miHoYo's Noelle GC lost marks without it: "marker find object not equal")
static bool exact_atomics;
extern "C" void arm_set_exact_atomics(int on) { exact_atomics = on != 0; }

constexpr size_t SHARED_CACHE_BYTES = 512 << 20; // (SharedJit: one for all)
extern "C" int droid_tool(const char *name);   // game_settings.c
extern "C" int droid_game_in(const char *list); // game_settings.c: a fix for these games only
// The faster translator (SharedJit, WarmStart, QuickFirst): for the games it was tested with;
// every other game as it worked before (tools.txt "SharedJit" / "WarmStart" / "QuickFirst" on,
// "NoSharedJit" ... off). On for every game it broke Shadow Fight 4 and Subway Surfers.
#define FAST_JIT_GAMES "com.halfbrick.jetpack,com.imangi.,com.kiloo.subwaysurf,com.rovio.baba,com.sega.sonicdash,com.sega.sonicboomandroid,com.king.candycrushsaga,com.robtopx.geometryjumplite,com.yodo1.crossyroad" // (Subway: tried 2026-10-27, almost no stutter; Angry Birds 2: tried 2026-10-27, works; Sonic Dash: trial 2026-10-27. Nekki: added untested 2026-10-29, Vector 1 broke: out. Geometry Dash Lite: trial 2026-10-31, its game app. Crossy Road: trial 2026-10-31, its game app)
static bool fast_jit_game() { return droid_game_in(FAST_JIT_GAMES) != 0; }
static bool shared_jit_on() {
    static int on = -1;
    if (on < 0) on = !droid_tool("NoSharedJit") && (fast_jit_game() || droid_tool("SharedJit")) ? 1 : 0;
    return on == 1;
}
// Parallel warm start: the warm threads translate in workshops of their own (dynarmic droid_workshop:
// own code cache and lock), WARM_WORKSHOPS at once, instead of 2 taking turns at the shared cache's
// one lock with the game. For the games it was tried with (tools.txt "ParallelWarm" on, "NoParallelWarm" off).
#define PARALLEL_WARM_GAMES "com.kiloo.subwaysurf,com.robtopx.geometryjumplite,com.yodo1.crossyroad,com.rovio.baba"
constexpr int WARM_WORKSHOPS = 4; // (6 on the PS5: 3.6x slower each, the game's loading threads squeezed)
constexpr size_t WORKSHOP_CACHE_BYTES = 128 << 20;
static thread_local int tl_workshop; // a warm thread's workshop number (make_cpu)
extern "C" void thread_place_rest(void); // thread_place.c: a helper thread off the main / render threads' cores

Cpu *make_cpu(bool nested) {
    size_t id;
    bool big = false;
    {
        std::lock_guard<std::mutex> lock(pool_lock);
        auto &pool = pools[nested];
        for (size_t i = pool.size(); i-- > 0;) {
            Cpu *c = pool[i];
            if (!nested && want_big && c->stack_bytes < STACK_BYTES) continue; // a small one: not for it
            pool.erase(pool.begin() + (long)i);
            return c;
        }
        if (cpus_made == MAX_CPUS - 1) return nullptr; // (the last slot: SharedJit's keeper)
        id = cpus_made++;
        // (a warm thread's workshop takes none of the big ones: they're for the game's busy threads)
        if (!nested) big = (tl_workshop == 0 && top_made++ < BIG_CACHES) || want_big;
    }
    auto *cpu = new Cpu;
    cpu->stack_bytes = nested ? NESTED_STACK_BYTES : big ? STACK_BYTES : SMALL_STACK_BYTES;
    cpu->stack.p = stack_alloc(cpu->stack_bytes);
    Dynarmic::A64::UserConfig config;
    config.callbacks = cpu;
    config.processor_id = id;
    config.global_monitor = &monitor;
    config.tpidr_el0 = &cpu->tpidr_el0;
    config.tpidrro_el0 = &cpu->tpidrro_el0;
    config.enable_cycle_counting = false;
    config.hook_isb = true; // the code a JIT flushed: dropped at its ISB (above)
    // the game's atomics (LDXR/STXR: IL2CPP, Unity, V8 lock and count with them) as a cmpxchg
    // in the translated code, not a call out to ours that did the same cmpxchg.
    // DROID_SLOW_ATOMICS=1: the calls (to compare)
    static const bool slow_atomics = getenv("DROID_SLOW_ATOMICS") != nullptr;
    config.fastmem_exclusive_access = !slow_atomics && !heapguard_on();
    config.wall_clock_cntpct = true;
    config.code_cache_size = nested ? NESTED_CACHE_BYTES : big ? BIG_CACHE_BYTES : CACHE_BYTES;
    if (id >= FIRST_CPUS && !big) config.code_cache_size = nested ? LATE_NESTED_CACHE_BYTES : LATE_CACHE_BYTES;
    // fastmem: guest address == host pointer, so loads/stores become plain x86
    // moves off a base of 0 (a fault falls back to the callbacks above).
    config.fastmem_pointer = 0;
    if (heapguard_on()) config.fastmem_pointer = std::nullopt; // HeapGuard: every access checked
    config.fastmem_address_space_bits = 64;
    config.silently_mirror_fastmem = false;
    // yuzu's "unsafe" CPU accuracy: games don't need bit-exact ARM float edge cases.
    // tools.txt "ExactFP": the exact ones (NaN rules, reciprocal estimates, fused multiply-add)
    // exact floating point (dynarmic's unsafe shortcuts off): tools.txt "ExactFP", or a game that needs
    // it - Temple Run 1's trees stood on the road with the shortcuts (its placement math)
    static const bool exact_fp = droid_tool("ExactFP") != 0 || (java_package && !strcmp(java_package, "com.imangi.templerun"));
    config.unsafe_optimizations = !exact_fp;
    if (!exact_fp)
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA |
                                Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                                Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                                Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    // the game's atomics (STXR) as a plain cmpxchg: with the global monitor each one also took
    // a lock shared by every thread and checked all MAX_CPUS (128) monitors in turn - Shadow
    // Fight 3 spent ~15% of its time in the atomic helpers (__aarch64_cas4_acq & co). As yuzu's
    // default ("Auto") accuracy. DROID_GLOBAL_MONITOR=1: the monitor (to compare)
    static const bool global_monitor = getenv("DROID_GLOBAL_MONITOR") != nullptr;
    if (!global_monitor && !exact_atomics && !slow_atomics) config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    // (tests: DROID_DYN_OFF = the IR passes to leave out, to weigh their translation cost against
    // the code they make: 1 GetSetElimination, 2 ConstProp, 4 MiscIROpt)
    static const int dyn_off = getenv("DROID_DYN_OFF") ? atoi(getenv("DROID_DYN_OFF")) : 0;
    if (dyn_off & 1) config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
    if (dyn_off & 2) config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
    if (dyn_off & 4) config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
    // the quick first translation (tools.txt "QuickFirst", after Ryujinx's tier 0): the IR passes
    // left out - ~15% quicker to translate (the stutter), the code a little slower
    static const bool quick_first = !droid_tool("NoQuickFirst") && (fast_jit_game() || droid_tool("QuickFirst"));
    if (quick_first)
        config.optimizations &= ~(Dynarmic::OptimizationFlag::GetSetElimination | Dynarmic::OptimizationFlag::ConstProp |
                                  Dynarmic::OptimizationFlag::MiscIROpt);
    // Guest addresses are host pointers: up to 48 bits.
    config.page_table_address_space_bits = 48;
    // One code cache for every thread (tools.txt "SharedJit", after Ryujinx's one translator per
    // process): code one thread translated runs on all of them (a game's 20-50 threads each
    // translated the same libil2cpp / libc++ code again). Its first Jit is a keeper's, never freed
    // (the shared emitter keeps that Jit's callbacks object for a few slow paths).
    if (shared_jit_on()) {
        config.droid_shared_code = true;
        config.code_cache_size = SHARED_CACHE_BYTES;
        static std::once_flag keeper_once;
        std::call_once(keeper_once, [&config] {
            Cpu *keeper = new Cpu; // (for good)
            Dynarmic::A64::UserConfig kc = config;
            kc.callbacks = keeper;
            kc.processor_id = MAX_CPUS - 1; // (a slot no game thread gets)
            kc.tpidr_el0 = &keeper->tpidr_el0;
            kc.tpidrro_el0 = &keeper->tpidrro_el0;
            keeper->jit = std::make_unique<Dynarmic::A64::Jit>(kc);
            droid_log("arm: one JIT code cache for every thread (%zu MB)", SHARED_CACHE_BYTES >> 20);
        });
        if (tl_workshop > 0) { // a warm thread's: its own code cache, translating in parallel
            config.droid_workshop = tl_workshop;
            config.code_cache_size = WORKSHOP_CACHE_BYTES;
        }
    }
    cpu->jit = std::make_unique<Dynarmic::A64::Jit>(config);
    return cpu;
}

// One per host thread that runs ARM code: its JITs by call depth and its
// bionic thread slots (tpidr_el0; slot 0 = self, slot 5 (+0x28) = stack guard).
struct ThreadState {
    Cpu *cpus[MAX_DEPTH];
    int depth;
    std::uint64_t *tls_block;
    std::uint64_t tpidr;
    bool exiting; // guest pthread_exit
    std::uint64_t exit_value;
    bool released; // a guest thread whose start routine is done
};
thread_local ThreadState ts;

const std::uint64_t stack_guard = (__builtin_ia32_rdtsc() * 0x9E3779B97F4A7C15ull) & ~std::uint64_t(0xFF);

void release_thread() {
    std::lock_guard<std::mutex> lock(pool_lock);
    for (int d = 0; d < MAX_DEPTH; d++) {
        if (!ts.cpus[d]) continue;
        pools[d > 0].push_back(ts.cpus[d]);
        ts.cpus[d] = nullptr;
    }
    delete[] ts.tls_block;
    ts.tls_block = nullptr;
    ts.tpidr = 0;
}

extern "C" void *arm_module_at(std::uint64_t addr, const char **name, const char **path);
extern "C" int arm_module_count(void); // arm_modules.c
#include "soloader.h"
extern "C" so_module *arm_module(int i);

static void where(std::uint64_t a, char *out, size_t n) {
    // our page of entries into PS5 functions (an svc and a ret each, 8 bytes): their names, the
    // 8 of a 64-byte spot (ArmProf's samples come by the spot)
    const std::uint64_t stubs = reinterpret_cast<std::uint64_t>(stub_page);
    if (stub_page && a >= stubs && a < stubs + MAX_THUNKS * 8) {
        int idx = int((a - stubs) / 8), count = thunk_count.load(std::memory_order_acquire);
        int at = std::snprintf(out, n, "PS5 calls:");
        for (int i = idx; i < idx + 8 && i < count && at < (int)n - 2; i++)
            at += std::snprintf(out + at, n - size_t(at), " %s", thunks[i].name);
        return;
    }
    const char *name = nullptr;
    void *m = arm_module_at(a, &name, nullptr);
    if (m) {
        // so_module starts with its base pointer
        std::snprintf(out, n, "%s+%#llx", name, (unsigned long long)(a - *static_cast<std::uint64_t *>(m)));
    } else {
        std::snprintf(out, n, "%#llx", (unsigned long long)a);
    }
}

static void report_state(Cpu *cpu, const char *what);

static void report_bad_access(Cpu *cpu) {
    report_state(cpu, cpu->bad_write ? "BAD write" : "BAD read");
}

// A crash report's ARM half (app_main.c's handler): this thread's registers, the
// return addresses on its stacks, its last calls. Nothing when it runs no ARM code.
extern "C" void arm_crash_state(void) {
    if (ts.depth > 0 && ts.cpus[ts.depth - 1]) report_state(ts.cpus[ts.depth - 1], "crash");
}

// This thread's ARM stack now: from its sp to its top. 0 = it runs no ARM code.
extern "C" int arm_guest_sp_range(std::uint64_t *sp, std::uint64_t *top) {
    if (ts.depth <= 0 || !ts.cpus[ts.depth - 1]) return 0;
    Cpu *c = ts.cpus[ts.depth - 1];
    *sp = c->jit->GetSP();
    *top = reinterpret_cast<std::uint64_t>(c->stack.get()) + c->stack_bytes;
    return *sp < *top;
}

// One of this thread's ARM registers now (x0-x30). 0 when it runs no ARM code.
extern "C" std::uint64_t arm_guest_reg(int i) {
    if (ts.depth <= 0 || !ts.cpus[ts.depth - 1] || i < 0 || i > 30) return 0;
    return ts.cpus[ts.depth - 1]->jit->GetRegister(i);
}

// The low 64 bits of one of this thread's ARM vector registers (v0-v31: s / d values), XTool's S
extern "C" std::uint64_t arm_guest_vreg(int i) {
    if (ts.depth <= 0 || !ts.cpus[ts.depth - 1] || i < 0 || i > 31) return 0;
    return ts.cpus[ts.depth - 1]->jit->GetVector(i)[0];
}

// This thread's ARM call chain: its lr, then the words on its stack that point into a library
// (return addresses; the game's allocators keep no frame pointers), its x19-x28 too
extern "C" void arm_guest_fp_chain(const char *why) {
    std::uint64_t sp, top;
    if (!arm_guest_sp_range(&sp, &top)) return;
    char at[96];
    where(arm_guest_reg(30), at, sizeof(at));
    droid_log("arm: %s - call chain: lr %s", why, at);
    for (int k = 19; k <= 28; k += 2)
        droid_log("arm:   x%d %#llx  x%d %#llx", k, (unsigned long long)arm_guest_reg(k), k + 1,
                  (unsigned long long)arm_guest_reg(k + 1));
    int shown = 0;
    for (std::uint64_t a = sp; a + 8 <= top && shown < 32; a += 8) {
        const std::uint64_t v = *reinterpret_cast<const std::uint64_t *>(a);
        const char *nm = nullptr;
        if (!arm_module_at(v, &nm, nullptr)) continue;
        where(v, at, sizeof(at));
        droid_log("arm:   +%#llx: %s", (unsigned long long)(a - sp), at);
        shown++;
    }
}

static void report_state(Cpu *cpu, const char *what) {
    auto &j = *cpu->jit;
    char pc[96], lr[96];
    where(j.GetPC(), pc, sizeof(pc));
    where(j.GetRegister(30), lr, sizeof(lr));
    droid_log("arm: %s at %#llx — stopped. guest pc %s (block after the access), lr %s, sp %#llx", what,
              (unsigned long long)cpu->bad_addr, pc, lr, (unsigned long long)j.GetSP());
    for (int i = 0; i < 31; i += 4) {
        char line[256];
        int n = 0;
        for (int k = i; k < i + 4 && k < 31; k++)
            n += std::snprintf(line + n, sizeof(line) - n, " x%-2d %#18llx", k, (unsigned long long)j.GetRegister(k));
        droid_log("arm:%s", line);
    }
    // the code before the return address (a packed library's code is only readable at run time):
    // the call that went wrong and how it got its target
    if (std::strstr(lr, ".so+")) {
        const std::uint32_t *c = reinterpret_cast<const std::uint32_t *>(j.GetRegister(30) - 0x60);
        for (int i = 0; i < 32; i += 8)
            droid_log("arm:   code lr%+d: %08x %08x %08x %08x %08x %08x %08x %08x", i * 4 - 0x60, c[i], c[i + 1],
                      c[i + 2], c[i + 3], c[i + 4], c[i + 5], c[i + 6], c[i + 7]);
    }
    // registers pointing into this thread's stack: the text there (a string being worked on is
    // often the cause), as ASCII and as UTF-16 (GTA's GXT text)
    {
        const std::uint64_t lo = reinterpret_cast<std::uint64_t>(cpu->stack.get()), hi = lo + cpu->stack_bytes;
        for (int k = 0; k < 31; k++) {
            const std::uint64_t r = j.GetRegister(k);
            if (r < lo || r + 96 > hi) {
#ifdef DROID_PC
                // PC: text a register points to anywhere readable (an engine's message on its heap)
                char buf[401];
                struct iovec l{buf, 400}, rv{reinterpret_cast<void *>(r), 400};
                if (r > 0x10000 && process_vm_readv(getpid(), &l, 1, &rv, 1, 0) == 400) {
                    buf[400] = 0;
                    int n = 0;
                    while (n < 400 && buf[n] >= 32 && buf[n] < 127) n++;
                    if (n >= 16) droid_log("arm:   x%-2d -> heap text \"%.*s\"", k, n, buf);
                }
#endif
                continue;
            }
            const std::uint8_t *b = reinterpret_cast<const std::uint8_t *>(r);
            char ascii[97], wide[49];
            for (int i = 0; i < 96; i++) ascii[i] = b[i] >= 32 && b[i] < 127 ? char(b[i]) : '.';
            ascii[96] = 0;
            for (int i = 0; i < 48; i++) {
                const unsigned c = b[2 * i] | b[2 * i + 1] << 8;
                wide[i] = c >= 32 && c < 127 ? char(c) : '.';
            }
            wide[48] = 0;
            droid_log("arm:   x%-2d -> \"%s\" | utf16 \"%s\"", k, ascii, wide);
            // a longer UTF-16 text there (an engine's fatal message: Unreal's FMalloc errors)
            // (from the first run of 8+ letters in the 48: a message may sit past a header)
            int at = 0, printable = 0;
            for (int i = 0; i < 48; i++) {
                printable = wide[i] != '.' ? printable + 1 : 0;
                if (printable == 8) { at = i - 7; break; }
                if (i == 47) printable = 0;
            }
            if (printable >= 8) {
                char longer[401];
                int n = 0;
                const std::uint8_t *s = b + 2 * at;
                for (; n < 400 && r + 2 * at + 2 * n + 2 <= hi; n++) {
                    const unsigned c = s[2 * n] | s[2 * n + 1] << 8;
                    if (!c) break;
                    longer[n] = c >= 32 && c < 127 ? char(c) : '.';
                }
                longer[n] = 0;
                droid_log("arm:   x%-2d utf16 text: \"%s\"", k, longer);
            }
        }
    }
    // The call chain: words on this thread's ARM stacks (the failing one, then
    // the outer ones) that point into a library — return addresses.
    for (int d = MAX_DEPTH - 1; d >= 0; d--) {
        Cpu *c = ts.cpus[d];
        if (!c) continue;
        const std::uint64_t lo = reinterpret_cast<std::uint64_t>(c->stack.get());
        const std::uint64_t top = lo + c->stack_bytes;
        const std::uint64_t from = c == cpu ? j.GetSP() : c->jit->GetSP();
        if (from < lo || from >= top) continue;
        droid_log("arm:   stack of depth %d (sp %#llx, %llu bytes used):", d, (unsigned long long)from,
                  (unsigned long long)(top - from));
        int shown = 0;
        for (std::uint64_t at = from; at + 8 <= top && shown < 24; at += 8) {
            std::uint64_t v;
            std::memcpy(&v, reinterpret_cast<const void *>(at), 8);
            const char *name = nullptr;
            if (!arm_module_at(v, &name, nullptr)) continue;
            char w[96];
            where(v, w, sizeof(w));
            droid_log("arm:     +%#llx: %s", (unsigned long long)(at - from), w);
            shown++;
        }
    }
    // The PS5 functions this thread called last (busy ones like memset left out),
    // file paths shown: what went wrong just before is usually here.
    static const char *const busy[] = {"memset", "memcpy", "memmove", "memcmp", "strlen", "malloc", "free",
                                       "calloc", "realloc", "strcmp", "strncmp", "pthread_mutex_lock",
                                       "pthread_mutex_unlock", "__memset_chk", "__memcpy_chk", "strchr",
                                       "posix_memalign", "memalign", "bsearch"};
    static const char *const pathy[] = {"open", "__open_2", "stat", "lstat", "access", "fopen", "opendir",
                                        "mkdir", "dlopen", "realpath", "readlink", "fstatat", "openat",
                                        "statfs", "remove", "rename", "unlink", "faccessat", "getenv"};
    const char *tname;
    std::uint64_t tlr, a0, a1, a2, ret;
    droid_log("arm:   last PS5 calls on this thread (oldest first, busy ones left out):");
    for (int i = 0; arm_trace_get(i, &tname, &tlr, &a0, &a1, &a2, &ret); i++) {
        bool skip = false;
        for (const char *b : busy) skip = skip || std::strcmp(tname, b) == 0;
        if (skip) continue;
        bool path = false;
        for (const char *b : pathy) path = path || std::strcmp(tname, b) == 0;
        char w[96];
        where(tlr, w, sizeof(w));
        if (path && a0 > 0x10000 && a0 < 0x800000000000ull)
            droid_log("arm:     %s(\"%.160s\", %#llx) -> %#llx  (from %s)", tname, reinterpret_cast<const char *>(a0),
                      (unsigned long long)a1, (unsigned long long)ret, w);
        else if (!std::strcmp(tname, "vsnprintf") && a2 > 0x10000 && a2 < 0x800000000000ull)
            // its format (an engine's assert message before its abort: Mono's)
            droid_log("arm:     vsnprintf(%#llx, %#llx, \"%.300s\") -> %#llx  (from %s)", (unsigned long long)a0,
                      (unsigned long long)a1, reinterpret_cast<const char *>(a2), (unsigned long long)ret, w);
        else
            droid_log("arm:     %s(%#llx, %#llx, %#llx) -> %#llx  (from %s)", tname, (unsigned long long)a0,
                      (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)ret, w);
    }
    cpu->bad_addr = 0;
}

// Runs guest code at fn with registers x0-x7 (and v0-v7 when v) until it
// returns. Nests: a PS5 function called from ARM may call ARM code again.
// arguments past the 8th (on the ARM stack, 8 bytes each, as AAPCS64 on Linux) for the next run
// on this thread: arm_call_full
static thread_local std::uint64_t pending_stack[16];
static thread_local int pending_stack_n;
bool run(std::uint64_t fn, const std::uint64_t *x, const Vector *v, Vector *v0_out, std::uint64_t *ret,
         std::uint64_t sp_at = 0) {
    if (ts.depth >= MAX_DEPTH) {
        droid_log("arm: calls nested too deep at %#llx", (unsigned long long)fn);
        return false;
    }
    Cpu *&slot = ts.cpus[ts.depth];
    if (!slot) slot = make_cpu(ts.depth > 0);
    if (!slot) {
        droid_log("arm: no JIT left (%zu made)", MAX_CPUS);
        return false;
    }
    if (!ts.tls_block) {
        ts.tls_block = new std::uint64_t[TLS_WORDS]();
        std::uint64_t *slots = ts.tls_block + TLS_WORDS / 2;
        slots[0] = reinterpret_cast<std::uint64_t>(slots);
        slots[5] = stack_guard;
        // the ARM code of pthread_self / pthread_getspecific (arm_imports.c): its thread's id at
        // tpidr - 2048, the game's key values from tpidr - 4096 (tls_block[0..255], zeroed)
        ts.tls_block[256] = reinterpret_cast<std::uint64_t>(pthread_self());
        ts.tpidr = reinterpret_cast<std::uint64_t>(slots);
    }
    Cpu *cpu = slot;
    auto &jit = *cpu->jit;
    for (int i = 0; i < 8; i++) {
        jit.SetRegister(i, x ? x[i] : 0);
        if (v) jit.SetVector(i, v[i]);
    }
    // One ARM stack per thread, as a real thread has (the GC scans it from sp to its top): a
    // call into ARM code from a PS5 function goes on below its caller's sp; only the outermost
    // uses the stack's top. (A signal handler: below the interrupted code, sp_at.)
    std::uint64_t sp;
    Cpu *caller = ts.depth > 0 ? ts.cpus[ts.depth - 1] : nullptr;
    if (sp_at) sp = sp_at & ~std::uint64_t(15);
    else if (caller) sp = (caller->jit->GetSP() - 1024) & ~std::uint64_t(15);
    else sp = (reinterpret_cast<std::uint64_t>(cpu->stack.get()) + cpu->stack_bytes - STACK_TOP_PAD) & ~std::uint64_t(15);
    if (pending_stack_n) { // the stack arguments at the callee's sp, in order
        sp -= (std::uint64_t(pending_stack_n) * 8 + 15) & ~std::uint64_t(15);
        for (int i = 0; i < pending_stack_n; i++) reinterpret_cast<std::uint64_t *>(sp)[i] = pending_stack[i];
        pending_stack_n = 0;
    }
    jit.SetSP(sp);
    jit.SetRegister(30, reinterpret_cast<std::uint64_t>(return_stub));
    jit.SetPC(fn);
    cpu->tpidr_el0 = ts.tpidr;
    cpu->failed = false;
    ThreadInfo *info = thread_info();
    void *was_running = info->running.load();
    info->running.store(cpu);
    ts.depth++;
    std::uint64_t pc = 0;
    bool returned = false;
    Dynarmic::HaltReason hr{};
    inv_apply(cpu); // code changed since this translator last ran
    for (;;) {
        hr = jit.Run();
        pc = jit.GetPC();
        returned = pc >= reinterpret_cast<std::uint64_t>(return_stub) &&
                   pc < reinterpret_cast<std::uint64_t>(return_stub) + sizeof(return_stub);
        if (cpu->failed || returned || ts.exiting) break;
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined5)) { // Warm: the thread is named, its list now
            arm_warm_ahead(cpu);
            if (hr == Dynarmic::HaltReason::UserDefined5) continue;
        }
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined4) ||
            Dynarmic::Has(hr, Dynarmic::HaltReason::CacheInvalidation)) { // code changed: on again
            for (int d = 0; d < ts.depth; d++)
                if (ts.cpus[d]) inv_apply(ts.cpus[d]);
            if (!Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2) &&
                !Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined3))
                continue;
        }
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined3)) { // the report's sample: on again
            info->sample_pc.store(pc);
            info->sample_lr.store(jit.GetRegister(30));
            info->sampled.store(1);
            if (!Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) continue;
        }
        if (!Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) break;
        // stopped for a signal: its handler, then on where it was - not while it holds a lock
        // (then: as its unlock returns, the call above), unless that took over 200 ms
        const std::int64_t sent = info->stop_sent_ns.load(std::memory_order_relaxed);
        if (locks_defer() == 0 || (sent && mono_ns() - sent > 200000000)) deliver_signals(cpu);
    }
    if (!returned && !cpu->failed && !ts.exiting) { // stopped for another reason: say where
        char at[96];
        where(pc, at, sizeof(at));
        droid_log("arm: ARM code stopped at %s (halt %#x), not returned", at, static_cast<unsigned>(hr));
    }
    ts.depth--;
    info->running.store(was_running);
    ts.tpidr = cpu->tpidr_el0;
    if (cpu->bad_addr) report_bad_access(cpu);
    if (ret) *ret = jit.GetRegister(0);
    if (v0_out) *v0_out = jit.GetVector(0);
    return !cpu->failed && (returned || ts.exiting);
}

// Linux arm64's signal frame (the parts a handler may read), on the guest stack
struct LinuxSigFrame {
    struct {
        std::int32_t signo, err, code, pad;
        std::int32_t pid, uid;
        std::uint8_t rest[104];
    } info;
    struct {
        std::uint64_t flags, link;
        std::uint64_t ss_sp;
        std::int32_t ss_flags, ss_pad;
        std::uint64_t ss_size;
        std::uint64_t sigmask;
        std::uint8_t pad[120];
        alignas(16) std::uint64_t fault_address;
        std::uint64_t regs[31];
        std::uint64_t sp, pc, pstate;
        alignas(16) std::uint8_t reserved[1024];
    } uc;
};

// DROID_SIG_LOG=1: every handler run, and the futex calls made inside one (a GC pause's
// handshake: who answered, on which word)
thread_local int in_sig_handler;
static bool sig_log() {
    static int on = -1;
    if (on < 0) on = getenv("DROID_SIG_LOG") != nullptr;
    return on;
}
extern "C" int arm_in_sig_handler(void) { return in_sig_handler; }
extern "C" int arm_sig_log_on(void) { return sig_log(); }
extern "C" int arm_thread_index(void) { return int(thread_info() - thread_infos); }

void deliver_signals(Cpu *cur) {
    ThreadInfo *info = thread_info();
    for (int rounds = 0; rounds < 64; rounds++) {
        const std::uint64_t ready = info->pending.load(std::memory_order_acquire) & ~sig_blocked;
        if (!ready) return;
        const int sig = __builtin_ctzll(ready) + 1;
        info->pending.fetch_and(~(std::uint64_t(1) << (sig - 1)));
        SigAct &a = sig_acts[sig];
        const std::uint64_t handler = a.handler.load();
        const int flags = a.flags.load();
        if (handler <= 1) continue; // default / ignored: nothing to run (no process to end here)
        auto &j = *cur->jit;
        const std::uint64_t at = (j.GetSP() - 256 - sizeof(LinuxSigFrame)) & ~std::uint64_t(15);
        auto *f = reinterpret_cast<LinuxSigFrame *>(at);
        std::memset(f, 0, sizeof(*f));
        f->info.signo = sig;
        f->info.code = -6; // SI_TKILL
        f->info.pid = getpid();
        f->uc.sigmask = sig_blocked;
        for (int r = 0; r < 31; r++) f->uc.regs[r] = j.GetRegister(r);
        f->uc.sp = j.GetSP();
        f->uc.pc = j.GetPC();
        f->uc.pstate = j.GetPstate();
        const std::uint64_t saved = sig_blocked;
        sig_blocked |= a.mask.load() | ((flags & L_SA_NODEFER) ? 0 : (std::uint64_t(1) << (sig - 1)));
        if (std::uint32_t(flags) & L_SA_RESETHAND) a.handler.store(0);
        if (sig == 30) { // a GC stop: how long it took to reach this thread
            const std::int64_t sent = info->stop_sent_ns.exchange(0, std::memory_order_relaxed);
            if (sent) {
                const std::int64_t took = mono_ns() - sent;
                gc_stops.fetch_add(1, std::memory_order_relaxed);
                std::int64_t m = gc_stop_slowest_ns.load(std::memory_order_relaxed);
                while (took > m && !gc_stop_slowest_ns.compare_exchange_weak(m, took)) {}
                static std::atomic<int> slow_told{ 0 };
                if (took > 20000000 && slow_told.fetch_add(1) < 40) {
                    char at[96];
                    where(j.GetPC(), at, sizeof(at));
                    droid_log("gc: thread %d took the GC's stop %.0f ms late (at %s; last call %s)",
                              int(info - thread_infos), took / 1e6, at, info->call.load() ? info->call.load() : "-");
                }
            }
        }
        static std::atomic<std::uint64_t> delivered{ 0 };
        const std::uint64_t d = delivered.fetch_add(1) + 1;
        if (d <= 4 || d % 2000 == 0)
            droid_log("arm: signal %d handled on its thread (%llu so far)", sig, (unsigned long long)d);
        const std::uint64_t x[8] = { std::uint64_t(sig), reinterpret_cast<std::uint64_t>(&f->info),
                                     reinterpret_cast<std::uint64_t>(&f->uc) };
        std::uint64_t r = 0;
        if (sig_log()) droid_log("sig: thread %d runs signal %d's handler", int(info - thread_infos), sig);
        in_sig_handler++;
        if (!run(handler, x, nullptr, nullptr, &r, at))
            droid_log("arm: signal %d's handler %#llx did not return", sig, (unsigned long long)handler);
        in_sig_handler--;
        // as Linux's sigreturn: what the handler changed in the saved context comes back (a GC
        // that moves objects rewrites the pointers it found in the registers)
        int changed = 0;
        for (int r = 0; r < 31; r++)
            if (f->uc.regs[r] != j.GetRegister(r)) {
                j.SetRegister(r, f->uc.regs[r]);
                changed++;
            }
        if (f->uc.sp != j.GetSP()) j.SetSP(f->uc.sp), changed++;
        if (f->uc.pc != j.GetPC()) j.SetPC(f->uc.pc), changed++;
        if (changed) {
            static std::atomic<int> told{ 0 };
            if (told.fetch_add(1) < 8)
                droid_log("arm: signal %d's handler changed %d saved registers: taken", sig, changed);
        }
        if (sig_log()) droid_log("sig: thread %d's signal %d handler returned", int(info - thread_infos), sig);
        sig_blocked = saved;
    }
}

void host_kick(int) {} // SIGUSR2 on a thread in a blocking call: the call returns EINTR

// ---- PS5 code calling ARM code: an x86 trampoline per guest function -------
//   mov r10, guest; mov r11, arm_callback_entry; jmp r11
// arm_callback_entry saves the x86 argument registers and runs the guest
// function with them (rdi..r9 -> x0-x5, 2 stack words -> x6/x7, xmm0-7 -> v0-v7).

std::mutex cb_lock;
std::unordered_map<std::uint64_t, void *> callbacks;
std::uint8_t *cb_page;
size_t cb_used;

struct GuestThread {
    std::uint64_t start, arg;
    std::uint64_t mask; // the creator's signal mask: a new thread starts with it, as on Linux
    std::uint8_t *host_stack; // ours (host_stack_get) when the console refused its own; else null
};

} // namespace

std::uint64_t swaps[4][2];
std::atomic<int> swap_count{ 0 };

extern "C" void arm_arg0_hooks(std::uint64_t (*to_host)(std::uint64_t), std::uint64_t (*to_guest)(std::uint64_t)) {
    arg0_to_host = to_host;
    arg0_to_guest = to_guest;
}

extern "C" void arm_swap_arg0(std::uint64_t from, std::uint64_t to) {
    const int n = swap_count.load();
    if (n == 4) return;
    swaps[n][0] = from;
    swaps[n][1] = to;
    swap_count.store(n + 1);
}

extern "C" std::uint64_t arm_callback_dispatch(std::uint64_t guest, std::uint64_t *regs,
                                               const std::uint64_t *stack) {
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 64) {
        char w[96];
        where(guest, w, sizeof(w));
        droid_log("arm: callback %s (%#llx, %#llx, %#llx) from PS5 code, depth %d, last call %s", w,
                  (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2],
                  ts.depth, last_import ? last_import : "(none)");
    }
    std::uint64_t x[8] = { regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], stack[0], stack[1] };
    for (int i = 0, n = swap_count.load(); i < n; i++)
        if (x[0] == swaps[i][0]) x[0] = swaps[i][1];
    if (arg0_to_guest) x[0] = arg0_to_guest(x[0]);
    Vector v[8];
    std::memcpy(v, regs + 6, sizeof(v));
    Vector v0{};
    std::uint64_t r = 0;
    if (!run(guest, x, v, &v0, &r)) droid_log("arm: callback %#llx did not return", (unsigned long long)guest);
    std::memcpy(regs + 22, &v0, sizeof(v0));
    // a key destructor after the thread's start routine: give the JIT back again
    if (ts.released && ts.depth == 0) release_thread();
    return r;
}

extern "C" void arm_callback_entry(void);
asm(R"(
    .text
    .p2align 4
    .globl arm_callback_entry
    .type arm_callback_entry,@function
arm_callback_entry:
    push %rbp
    mov %rsp, %rbp
    sub $192, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    movups %xmm0, 48(%rsp)
    movups %xmm1, 64(%rsp)
    movups %xmm2, 80(%rsp)
    movups %xmm3, 96(%rsp)
    movups %xmm4, 112(%rsp)
    movups %xmm5, 128(%rsp)
    movups %xmm6, 144(%rsp)
    movups %xmm7, 160(%rsp)
    mov %r10, %rdi
    mov %rsp, %rsi
    lea 16(%rbp), %rdx
    call arm_callback_dispatch
    movups 176(%rsp), %xmm0
    leave
    ret
    .size arm_callback_entry, .-arm_callback_entry
)");

extern "C" void *arm_callback(std::uint64_t guest) {
    if (!guest) return nullptr;
    // One of our own thunks: call the PS5 function directly.
    const auto stubs = reinterpret_cast<std::uint64_t>(stub_page);
    if (stub_page && guest >= stubs && guest < stubs + MAX_THUNKS * 8) {
        const int idx = int((guest - stubs) / 8);
        if (idx < thunk_count.load(std::memory_order_acquire) && thunks[idx].fn) return thunks[idx].fn;
    }
    std::lock_guard<std::mutex> lock(cb_lock);
    auto it = callbacks.find(guest);
    if (it != callbacks.end()) return it->second;
    if (!cb_page || cb_used + 32 > 0x10000) {
        cb_page = static_cast<std::uint8_t *>(droid_jit_alloc(0x10000));
        cb_used = 0;
        if (!cb_page) return nullptr;
    }
    std::uint8_t *p = cb_page + cb_used;
    cb_used += 32;
    const auto entry = reinterpret_cast<std::uint64_t>(&arm_callback_entry);
    p[0] = 0x49; p[1] = 0xBA; std::memcpy(p + 2, &guest, 8);  // mov r10, imm64
    p[10] = 0x49; p[11] = 0xBB; std::memcpy(p + 12, &entry, 8); // mov r11, imm64
    p[20] = 0x41; p[21] = 0xFF; p[22] = 0xE3;                  // jmp r11
    callbacks[guest] = p;
    return p;
}

// ---- guest threads ------------------------------------------------------------

static long long droid_now_ms() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Host stacks of our own, for when the console refuses a thread its stack (pthread_create
// EAGAIN after ~55 threads: Shadow Fight 3's lookups, "Cannot resolve destination host"):
// direct memory, handed back when the thread ends and given out again only seconds later
// (the thread still runs its last lines on it as it ends).
constexpr size_t HOST_STACK_BYTES = 2 << 20;
static std::mutex host_stack_lock;
static std::vector<std::pair<std::uint8_t *, std::int64_t>> host_stacks_free; // stack, freed at (ms)
static std::atomic<int> threads_alive{ 0 };
static std::uint8_t *host_stack_get() {
    {
        std::lock_guard<std::mutex> lk(host_stack_lock);
        const std::int64_t now = droid_now_ms();
        for (size_t i = 0; i < host_stacks_free.size(); i++)
            if (now - host_stacks_free[i].second > 3000) {
                std::uint8_t *st = host_stacks_free[i].first;
                host_stacks_free.erase(host_stacks_free.begin() + long(i));
                return st;
            }
    }
    return stack_alloc(HOST_STACK_BYTES);
}
static void host_stack_put(std::uint8_t *st) {
    std::lock_guard<std::mutex> lk(host_stack_lock);
    host_stacks_free.push_back({ st, droid_now_ms() });
}

extern "C" void thread_place_new(void); // thread_place.c: off the main and render threads' cores
static void *guest_thread_main(void *p) {
    thread_place_new();
    const GuestThread g = *static_cast<GuestThread *>(p);
    delete static_cast<GuestThread *>(p);
    threads_alive.fetch_add(1);
    const std::uint64_t x[8] = { g.arg };
    std::uint64_t r = 0;
    sig_blocked = g.mask;
    thread_info()->blocked.store(g.mask);
    static std::atomic<int> started{ 0 };
    const int nth = started.fetch_add(1);
    if (nth < 120) { // when each thread starts, and where (a lookup thread that never got far)
        char at[96];
        where(g.start, at, sizeof(at));
        droid_log("arm: thread %d starts at %s (t=%lld ms, pthread %#llx)", nth, at, (long long)(droid_now_ms()),
                  (unsigned long long)(uintptr_t)pthread_self()); // the id a pthread_join names
    }
    where(g.start, warm_base, sizeof(warm_base)); // (Warm: the same function next launch)
    std::snprintf(warm_key, sizeof(warm_key), "%s", warm_base);
    const bool ok = run(g.start, x, nullptr, nullptr, &r);
    if (nth < 120) droid_log("arm: thread %d ended (t=%lld ms)", nth, (long long)(droid_now_ms()));
    if (ts.exiting) r = ts.exit_value;
    else if (!ok) droid_log("arm: thread %#llx stopped (bad code)", (unsigned long long)g.start);
    // its keys' destructors, before its TLS goes. released only after: with it set, the first
    // destructor's return gave the JIT and the key values back, and the loop read them freed
    // (Genshin Impact: "malloc(): unsorted double linked list corrupted" as threads ended)
    arm_keys_thread_end(ts.tls_block);
    ts.released = true;
    release_thread();
    thread_info_release();
    threads_alive.fetch_sub(1);
    if (g.host_stack) host_stack_put(g.host_stack);
    return reinterpret_cast<void *>(r);
}

// set by a host (gameactivity_host.c) just before the game makes its drawing thread: that one
// gets 16 MB, as unity_host's render thread. The console's Vulkan driver compiles shaders on the
// thread that draws; on 2 MB Candy Crush died with no crash report at each new screen's shaders
extern "C" int arm_big_stack_next = 0;
extern "C" int arm_pthread_create(pthread_t *thread, const pthread_attr_t *attr, std::uint64_t start,
                                  std::uint64_t arg) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    const bool big = arm_big_stack_next != 0;
    arm_big_stack_next = 0;
    pthread_attr_setstacksize(&a, big ? 16 << 20 : 2 << 20); // host side: dynarmic + our functions
    if (big) droid_log("arm: this thread gets a 16 MB host stack (it draws)");
    int detach = PTHREAD_CREATE_JOINABLE;
    if (attr) pthread_attr_getdetachstate(attr, &detach);
    pthread_attr_setdetachstate(&a, detach);
    auto *g = new GuestThread{ start, arg, sig_blocked, nullptr };
    static const bool own_always = getenv("DROID_OWN_STACKS") != nullptr; // PC tests: the fallback, always
    int rc = own_always ? EAGAIN : pthread_create(thread, &a, guest_thread_main, g);
    static std::atomic<int> made{ 0 }, failed{ 0 };
    if (rc != 0) { // refused (the console: its stack): once more on a stack of ours
        size_t flex = 0;
        sceKernelAvailableFlexibleMemorySize(&flex);
        if (failed.load() < 20)
            droid_log("arm: pthread_create refused: %d (%d threads alive, %d made; flexible memory free %zu KB): "
                      "again on our own stack", rc, threads_alive.load(), made.load(), flex >> 10);
        std::uint8_t *st = host_stack_get();
        if (st && pthread_attr_setstack(&a, st, HOST_STACK_BYTES) == 0) {
            g->host_stack = st;
            rc = pthread_create(thread, &a, guest_thread_main, g);
            if (rc != 0) {
                host_stack_put(st);
                g->host_stack = nullptr;
            }
        }
    }
    pthread_attr_destroy(&a);
    if (rc != 0) {
        delete g;
        if (failed.fetch_add(1) < 20) // a game's helper thread that never starts (curl's lookups...)
            droid_log("arm: pthread_create failed: %d (after %d threads made)", rc, made.load());
    } else {
        made.fetch_add(1);
    }
    return rc;
}

extern "C" void arm_pthread_exit(std::uint64_t value) {
    ts.exiting = true;
    ts.exit_value = value;
    if (ts.depth > 0) ts.cpus[ts.depth - 1]->jit->HaltExecution();
}

// a call with up to 8 integer / pointer arguments, up to 8 float ones (s0..s7) and up to 16 on
// the stack (GameActivity's onTouchEventNative has 17)
extern "C" uint64_t arm_call_full(uint64_t fn, const uint64_t *x, int nx, const float *f, int nf, const uint64_t *stk,
                                  int nstk, int *ok) {
    std::uint64_t xr[8] = {};
    for (int i = 0; i < nx && i < 8; i++) xr[i] = x[i];
    Vector v[8] = {};
    for (int i = 0; i < nf && i < 8; i++) {
        std::uint32_t bits;
        std::memcpy(&bits, &f[i], 4);
        v[i][0] = bits;
    }
    pending_stack_n = nstk < 16 ? nstk : 16;
    for (int i = 0; i < pending_stack_n; i++) pending_stack[i] = stk[i];
    std::uint64_t r = 0;
    const bool good = run(fn, xr, v, nullptr, &r);
    if (ok) *ok = good;
    return r;
}

extern "C" uint64_t arm_call(uint64_t fn, const uint64_t *args, int count, int *ok) {
    std::uint64_t x[8] = {};
    for (int i = 0; i < count && i < 8; i++) x[i] = args[i];
    std::uint64_t r = 0;
    const bool good = run(fn, x, nullptr, nullptr, &r);
    if (ok) *ok = good;
    return r;
}

extern "C" uint64_t arm_thunk(const char *name, void *fn) {
    std::lock_guard<std::mutex> lock(thunk_lock);
    const int n = thunk_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (std::strcmp(thunks[i].name, name) == 0) return reinterpret_cast<std::uint64_t>(&stub_page[2 * i]);
    if (!stub_page) stub_page = static_cast<std::uint32_t *>(droid_jit_alloc(MAX_THUNKS * 8));
    if (!stub_page || n == MAX_THUNKS) {
        droid_log("arm: no room for import %s", name);
        return 0;
    }
    thunks[n].name = strdup(name);
    thunks[n].fn = fn;
    stub_page[2 * n] = 0xD4000001u | (std::uint32_t(n + 1) << 5); // svc #(n+1)
    stub_page[2 * n + 1] = 0xD65F03C0u;                             // ret
    thunk_count.store(n + 1, std::memory_order_release);
    return reinterpret_cast<std::uint64_t>(&stub_page[2 * n]);
}

extern "C" int arm_thunk_total(void) { return thunk_count.load(); }

// A thunk's PS5 function set later (Vulkan's exist once the driver is up); 0 = no thunk.
extern "C" uint64_t arm_thunk_set(const char *name, void *fn) {
    uint64_t at = arm_thunk(name, fn);
    std::lock_guard<std::mutex> lock(thunk_lock);
    const int n = thunk_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (std::strcmp(thunks[i].name, name) == 0) {
            if (fn) thunks[i].fn = fn;
            break;
        }
    return at;
}

// This thunk returns a jboolean: the ARM code gets 0 / 1 only, as from a phone's libart. x86 sets al
// and leaves the rest of rax: Unity read IsSameObject's garbage as "the same" (Vector 2 on the PS5).
extern "C" void arm_thunk_ret8(const char *name) {
    std::lock_guard<std::mutex> lock(thunk_lock);
    const int n = thunk_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (std::strcmp(thunks[i].name, name) == 0) thunks[i].ret8 = 1;
}

// Argument idx (0-7) of this thunk's calls goes to the host as 0.
extern "C" void arm_thunk_null_arg(const char *name, int idx) {
    std::lock_guard<std::mutex> lock(thunk_lock);
    const int n = thunk_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (std::strcmp(thunks[i].name, name) == 0) thunks[i].null_arg = idx;
}

extern "C" const char *arm_last_import(void) { return last_import ? last_import : "(none)"; }

// i = 0 is the oldest of the last TRACE_N calls. 0 = no entry.
extern "C" int arm_trace_get(int i, const char **name, std::uint64_t *lr, std::uint64_t *a0,
                             std::uint64_t *a1, std::uint64_t *a2, std::uint64_t *ret) {
    const unsigned n = trace_pos < TRACE_N ? trace_pos : TRACE_N;
    if (i < 0 || unsigned(i) >= n) return 0;
    const TraceEntry &e = trace_ring[(trace_pos - n + unsigned(i)) % TRACE_N];
    *name = thunks[e.idx].name;
    *lr = e.lr;
    *a0 = e.a0;
    *a1 = e.a1;
    *a2 = e.a2;
    *ret = e.ret;
    return 1;
}

extern "C" int arm_depth(void) { return ts.depth; }

// One line per ARM thread: inside which PS5 function (blocked?) or running ARM
// code, called from where, and how many calls it made since the last report.
extern "C" void arm_threads_report(void) {
    static std::uint64_t last_calls[MAX_THREAD_INFO];
    const int n = std::min(thread_info_count.load(), MAX_THREAD_INFO);
    droid_log("arm: thread report (%d threads)", n);
    for (int i = 0; i < n; i++) {
        ThreadInfo &t = thread_infos[i];
        const std::uint64_t calls = t.calls.load();
        const char *call = t.call.load();
        char from[96];
        where(t.lr.load(), from, sizeof(from));
        const char *role = t.role.load();
        const char *state = t.in_call.load() ? "" : !t.running.load() ? " [outside ARM code: in our C code]" : "";
        const unsigned long long sig_pending = t.pending.load();
        char sig_text[80] = "";
        if (sig_pending)
            std::snprintf(sig_text, sizeof(sig_text), " SIGNAL PENDING %#llx (blocked %#llx)", sig_pending,
                          (unsigned long long)t.blocked.load());
        char owner_text[64] = "";
#ifdef DROID_PC
        // waiting for a mutex: its owner (glibc keeps the owner's thread id at +8)
        if (t.in_call.load() && call && !std::strcmp(call, "pthread_mutex_lock") && t.args[0].load()) {
            const int owner = *reinterpret_cast<const int *>(t.args[0].load() + 8);
            int who = -1;
            for (int k = 0; k < n; k++)
                if (thread_infos[k].tid.load() == owner) who = k;
            std::snprintf(owner_text, sizeof(owner_text), " LOCK OWNER tid %d = thread %d", owner, who);
        }
#endif
        droid_log("arm:   thread %2d%s%s%s: %s %s(%#llx, %#llx, %#llx, %#llx) (from %s), %llu calls since last report%s%s%s"
                  " [pthread %#llx]", i,
                  role ? " (" : "", role ? role : "", role ? ")" : "", t.in_call.load() ? "IN" : "last",
                  call ? call : "(none)", (unsigned long long)t.args[0].load(), (unsigned long long)t.args[1].load(),
                  (unsigned long long)t.args[2].load(), (unsigned long long)t.args[3].load(), from,
                  (unsigned long long)(calls - last_calls[i]), state, sig_text, owner_text,
                  (unsigned long long)(uintptr_t)t.self.load()); // a pthread_join names it so
        // a waiting thread: the return addresses on its ARM stack name its caller
        if (t.in_call.load() && t.sp.load() && call &&
            (!std::strcmp(call, "usleep") || !std::strcmp(call, "nanosleep") || !std::strcmp(call, "poll") ||
             !std::strcmp(call, "epoll_wait") || !std::strcmp(call, "select") || !std::strcmp(call, "pthread_cond_wait") ||
             !std::strcmp(call, "pthread_cond_timedwait") || !std::strcmp(call, "futex") || !std::strcmp(call, "read"))) {
            int shown = 0;
            for (int off = 0; off < 4096 && shown < 8; off += 8) {
                std::uint64_t v = *reinterpret_cast<const std::uint64_t *>(t.sp.load() + off);
                char sbuf[96];
                where(v, sbuf, sizeof(sbuf));
                if (!std::strchr(sbuf, '+')) continue;
                droid_log("arm:     +%#x: %s", off, sbuf);
                shown++;
            }
        }
        last_calls[i] = calls;
        // running ARM code without calls (a spin?): where, sampled
        void *c = t.running.load();
        if (!t.in_call.load() && c && t.alive.load() && &t != my_info) {
            t.sampled.store(0);
            static_cast<Cpu *>(c)->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
            for (int k = 0; k < 50 && !t.sampled.load(); k++) usleep(1000);
            if (t.sampled.load()) {
                char at[96], back[96];
                where(t.sample_pc.load(), at, sizeof(at));
                where(t.sample_lr.load(), back, sizeof(back));
                droid_log("arm:     thread %2d runs at %s (lr %s)", i, at, back);
            }
        }
    }
}

// This thread's ARM stack (its top-level JIT's): lowest address, size.
extern "C" std::uint64_t arm_guest_stack(std::uint64_t *size) {
    Cpu *c = ts.cpus[0];
    if (!c) {
        *size = 0;
        return 0;
    }
    *size = c->stack_bytes;
    return reinterpret_cast<std::uint64_t>(c->stack.get());
}

// The PS5 functions ARM code spent the most time in since the last call.
extern "C" std::uint64_t arm_thread_blocked_cycles(void) { return blocked_cycles; }

extern "C" void arm_profile_dump(double elapsed_ms, int top) {
    static std::uint64_t prev_calls[MAX_THUNKS], prev_cycles[MAX_THUNKS];
    static std::uint64_t prev_tsc;
    static double tsc_per_ms;
    const std::uint64_t tsc = __builtin_ia32_rdtsc();
    if (prev_tsc && elapsed_ms > 0) tsc_per_ms = double(tsc - prev_tsc) / elapsed_ms;
    prev_tsc = tsc;
    const int n = thunk_count.load();
    static int order[MAX_THUNKS];
    static std::uint64_t dcyc[MAX_THUNKS], dcalls[MAX_THUNKS];
    std::uint64_t total = 0, total_calls = 0;
    for (int i = 0; i < n; i++) {
        const std::uint64_t c = thunks[i].calls.load(), y = thunks[i].cycles.load();
        dcalls[i] = c - prev_calls[i];
        dcyc[i] = y - prev_cycles[i];
        prev_calls[i] = c;
        prev_cycles[i] = y;
        order[i] = i;
        total += dcyc[i];
        total_calls += dcalls[i];
    }
    if (tsc_per_ms <= 0) return; // first call: calibration only
    std::sort(order, order + n, [](int a, int b) { return dcyc[a] > dcyc[b]; });
    droid_log("prof: %.0f ms in PS5 functions (%llu calls) over %.0f ms, all threads", total / tsc_per_ms,
              (unsigned long long)total_calls, elapsed_ms);
    for (int k = 0; k < top && k < n && dcyc[order[k]]; k++) {
        const int i = order[k];
        droid_log("prof:   %-28s %8llu calls %8.1f ms", thunks[i].name, (unsigned long long)dcalls[i],
                  dcyc[i] / tsc_per_ms);
    }
}

// ---- signals: the C side (linux_abi.c's sigaction, pthread_kill, sigsuspend, masks) ------

extern "C" int arm_sig_action(int sig, std::uint64_t handler, std::uint64_t mask, int flags, std::uint64_t *old_handler,
                              std::uint64_t *old_mask, int *old_flags) {
    if (sig < 1 || sig > 64) {
        errno = EINVAL;
        return -1;
    }
    SigAct &a = sig_acts[sig];
    if (old_handler) *old_handler = a.handler.load();
    if (old_mask) *old_mask = a.mask.load();
    if (old_flags) *old_flags = a.flags.load();
    if (handler != ~std::uint64_t(0)) { // ~0: only asking
        a.mask.store(mask);
        a.flags.store(flags);
        a.handler.store(handler);
        if (handler > 1 && !sig_used.exchange(true)) {
            struct sigaction k;
            std::memset(&k, 0, sizeof(k));
            k.sa_handler = host_kick; // no SA_RESTART: a blocking call returns EINTR
            sigaction(SIGUSR2, &k, nullptr);
            droid_log("arm: signals for ARM code on (first handler: signal %d)", sig);
        }
        static std::atomic<int> told{ 0 };
        if (handler > 1 && told.fetch_add(1) < 16)
            droid_log("arm: signal %d handler %#llx (flags %#x)", sig, (unsigned long long)handler, flags);
    }
    return 0;
}

extern "C" void arm_sig_set_futex_waker(void (*fn)(int bucket)) { futex_waker = fn; }

extern "C" int arm_sig_kill(pthread_t t, int sig) {
    if (sig < 0 || sig > 64) return EINVAL;
    ThreadInfo *ti = nullptr;
    const int n = std::min(thread_info_count.load(), MAX_THREAD_INFO);
    for (int i = 0; i < n && !ti; i++)
        if (thread_infos[i].alive.load() && pthread_equal(thread_infos[i].self.load(), t)) ti = &thread_infos[i];
    if (sig != 0) { // the first sends: who to whom (a GC pause that never ends: which thread)
        static std::atomic<int> sends{ 0 };
        if (sends.fetch_add(1) < 120)
            droid_log("arm: signal %d sent by thread %d to thread %d%s", sig, int(thread_info() - thread_infos),
                      ti ? int(ti - thread_infos) : -1, ti ? "" : " (no such thread: ESRCH)");
    }
    if (!ti) return ESRCH;
    if (sig == 0) return 0;
    if (sig_acts[sig].handler.load() <= 1) return 0; // no handler: nothing happens
    if (sig == 30) {
        const std::int64_t now = mono_ns();
        std::int64_t unset = 0; // the first send of this pause (a resend keeps the time: the 200 ms runs)
        ti->stop_sent_ns.compare_exchange_strong(unset, now, std::memory_order_relaxed);
        std::int64_t none = 0;
        gc_world_since.compare_exchange_strong(none, now); // the first stop of this collection
    } else if (sig == 24) {
        const std::int64_t since = gc_world_since.exchange(0);
        if (since) { // the first restart: the collection is done
            const std::int64_t took = mono_ns() - since;
            gc_collections.fetch_add(1, std::memory_order_relaxed);
            gc_world_ns.fetch_add(took, std::memory_order_relaxed);
            std::int64_t m = gc_world_longest_ns.load(std::memory_order_relaxed);
            while (took > m && !gc_world_longest_ns.compare_exchange_weak(m, took)) {}
            static std::atomic<int> told{ 0 };
            if (took > 50000000 && told.fetch_add(1) < 60)
                droid_log("gc: a collection stopped the game for %.0f ms (run by thread %d)", took / 1e6,
                          int(thread_info() - thread_infos));
        }
    }
    ti->pending.fetch_or(std::uint64_t(1) << (sig - 1));
    {
        std::lock_guard<std::mutex> lk(ti->sig_m);
        ti->sig_cv.notify_all();
    }
    if (ti == my_info) return 0; // ours: on the way out of this call
    if (void *c = ti->running.load()) static_cast<Cpu *>(c)->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    if (ti->in_call.load()) {
        const int b = ti->fx_bucket.load();
        if (b >= 0 && futex_waker) futex_waker(b);
        void *cond = ti->wait_cond.load(), *m = ti->wait_mutex.load();
        for (int k = 0; cond && m && k < 50; k++) { // its condition: woken (a spurious wake is allowed)
            if (pthread_mutex_trylock(static_cast<pthread_mutex_t *>(m)) == 0) {
                if (ti->wait_cond.load() == cond) pthread_cond_broadcast(static_cast<pthread_cond_t *>(cond));
                pthread_mutex_unlock(static_cast<pthread_mutex_t *>(m));
                break;
            }
            if (ti->wait_cond.load() != cond) break;
            usleep(100);
            if (k == 49) {
                static std::atomic<int> told{ 0 };
                if (told.fetch_add(1) < 20)
                    droid_log("arm: signal %d to thread %d: its wait's mutex %p stayed held, not woken", sig,
                              int(ti - thread_infos), m);
            }
        }
        pthread_kill(t, SIGUSR2); // a sleep, poll, read...: EINTR
    }
    return 0;
}

extern "C" int arm_sig_mask(int how, const std::uint64_t *set, std::uint64_t *old) {
    if (old) *old = sig_blocked;
    if (set) {
        const std::uint64_t keep = ~((std::uint64_t(1) << 8) | (std::uint64_t(1) << 18)); // SIGKILL, SIGSTOP
        if (how == 0) sig_blocked |= *set & keep;
        else if (how == 1) sig_blocked &= ~*set;
        else if (how == 2) sig_blocked = *set & keep;
        else {
            errno = EINVAL;
            return -1;
        }
        thread_info()->blocked.store(sig_blocked);
        static std::atomic<int> masks{ 0 };
        if (masks.fetch_add(1) < 200)
            droid_log("arm: thread %d mask how %d set %#llx -> %#llx", int(thread_info() - thread_infos), how,
                      (unsigned long long)*set, (unsigned long long)sig_blocked);
    }
    return 0;
}

// sigsuspend: the mask meanwhile, until a signal it lets through has run its handler
extern "C" void arm_sig_take(void);
extern "C" int arm_sig_suspend(std::uint64_t mask) {
    ThreadInfo *info = thread_info();
    const std::uint64_t old = sig_blocked;
    sig_blocked = mask;
    while (!(info->pending.load() & ~mask)) {
        std::unique_lock<std::mutex> lk(info->sig_m);
        info->sig_cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return (info->pending.load() & ~mask) != 0; });
    }
    arm_sig_take();
    sig_blocked = old;
    errno = EINTR;
    return -1;
}

// a wait (arm_imports.c, bionic_shim.c): where this thread waits now, or nothing (-1 / NULL)
extern "C" void arm_sig_wait_futex(int bucket) { thread_info()->fx_bucket.store(bucket); }
extern "C" void arm_sig_wait_cond(void *cond, void *mutex) {
    ThreadInfo *i = thread_info();
    i->wait_mutex.store(mutex);
    i->wait_cond.store(cond);
}
// A thread of ours (a Looper, the Choreographer, the audio thread) that runs the game's code
// now and then and waits in our C code in between: the game's GC knows it (il2cpp attached it
// in a callback) and pauses it with a signal. Outside ARM code there's no ARM stack frame to
// put the signal frame on: it goes in this thread's own buffer (all memory is the ARM side's
// too) and the handler runs on the thread's ARM stack. (Without it: Among Us froze, the GC
// waiting for a Looper thread that never answered its pause.)
static void deliver_signals_outside() {
    ThreadInfo *info = thread_info();
    alignas(16) static thread_local unsigned char space[sizeof(LinuxSigFrame) + 64];
    for (int rounds = 0; rounds < 64; rounds++) {
        const std::uint64_t ready = info->pending.load(std::memory_order_acquire) & ~sig_blocked;
        if (!ready) return;
        const int sig = __builtin_ctzll(ready) + 1;
        info->pending.fetch_and(~(std::uint64_t(1) << (sig - 1)));
        SigAct &a = sig_acts[sig];
        const std::uint64_t handler = a.handler.load();
        const int flags = a.flags.load();
        if (handler <= 1) continue;
        auto *f = reinterpret_cast<LinuxSigFrame *>(space);
        std::memset(f, 0, sizeof(*f));
        f->info.signo = sig;
        f->info.code = -6; // SI_TKILL
        f->info.pid = getpid();
        f->uc.sigmask = sig_blocked;
        const std::uint64_t saved = sig_blocked;
        sig_blocked |= a.mask.load() | ((flags & L_SA_NODEFER) ? 0 : (std::uint64_t(1) << (sig - 1)));
        if (std::uint32_t(flags) & L_SA_RESETHAND) a.handler.store(0);
        static std::atomic<std::uint64_t> outside{ 0 };
        if (outside.fetch_add(1) < 4)
            droid_log("arm: signal %d handled on a thread outside ARM code (ours, waiting)", sig);
        const std::uint64_t x[8] = { std::uint64_t(sig), reinterpret_cast<std::uint64_t>(&f->info),
                                     reinterpret_cast<std::uint64_t>(&f->uc) };
        std::uint64_t r = 0;
        if (!run(handler, x, nullptr, nullptr, &r))
            droid_log("arm: signal %d's handler %#llx did not return", sig, (unsigned long long)handler);
        sig_blocked = saved;
    }
}

// a blocking wait (a lock, a semaphore) between its tries: the signals for this thread run
// here, as they would interrupt the wait on Linux (the GC's pause has to reach every thread)
extern "C" void arm_sig_take(void) {
    if (ts.depth > 0 && ts.cpus[ts.depth - 1]) deliver_signals(ts.cpus[ts.depth - 1]);
    else if (ts.depth == 0) deliver_signals_outside();
}
// a thread of ours between its waits: its pending signals, if any (cheap when none)
extern "C" void arm_sig_point(void) {
    if (my_info && sig_used.load(std::memory_order_relaxed) && (my_info->pending.load() & ~sig_blocked)) arm_sig_take();
}
#ifdef DROID_PC
#include <execinfo.h>
#include <dlfcn.h>
// PC runner: each thread's own (host) call stack, logged - a freeze inside our C code
// (addresses as offsets in the runner: addr2line -e pc-out/android-pc <off>)
static void host_bt_handler(int) {
    void *f[24];
    const int n = backtrace(f, 24);
    Dl_info d{};
    char line[600];
    int at = std::snprintf(line, sizeof(line), "hostbt: thread %d:", my_info ? int(my_info - thread_infos) : -1);
    for (int i = 2; i < n && at < int(sizeof(line)) - 24; i++) {
        if (dladdr(f[i], &d) && d.dli_fbase && std::strstr(d.dli_fname ? d.dli_fname : "", "android-pc"))
            at += std::snprintf(line + at, sizeof(line) - at, " %#lx",
                                (unsigned long)(reinterpret_cast<std::uintptr_t>(f[i]) - reinterpret_cast<std::uintptr_t>(d.dli_fbase) - 1));
        else
            at += std::snprintf(line + at, sizeof(line) - at, " [%s]", d.dli_sname ? d.dli_sname : "?");
    }
    droid_log("%s", line);
}
extern "C" void arm_host_backtraces(void) {
    static bool installed;
    if (!installed) {
        installed = true;
        struct sigaction k;
        std::memset(&k, 0, sizeof(k));
        k.sa_handler = host_bt_handler;
        sigaction(SIGPROF, &k, nullptr);
    }
    const int n = std::min(thread_info_count.load(), MAX_THREAD_INFO);
    for (int i = 0; i < n; i++)
        if (thread_infos[i].alive.load() && !thread_infos[i].running.load()) {
            pthread_kill(thread_infos[i].self.load(), SIGPROF);
            usleep(20000);
        }
}
#else
extern "C" void arm_host_backtraces(void) {}
#endif

// the ARM code that called the PS5 function running now (its return address), as lib+off
extern "C" void arm_call_from(char *out, size_t n) {
    if (!my_info) { std::snprintf(out, n, "?"); return; }
    where(my_info->lr.load(), out, n);
}
// ---- CallDiff (the debug tool): the same game on the PC and on the console, call by call --------
// tools.txt "CallDiff": every host call the game makes goes to calldiff.txt in its folder (each
// thread's first 30 000, "CallDiff per=N" for more): thread, name, arguments, result, caller. Addresses are written as "p" (they
// differ between runs), small numbers as they are, C strings of the calls that take them as
// text. tools/calldiff.py lines a PC file up with a console one and prints where they part.
static std::atomic<int> calldiff_state{ -1 };
static FILE *calldiff_file;
static std::mutex calldiff_m;
static std::atomic<int> calldiff_lines{ 0 }, calldiff_threads{ 0 };
thread_local int calldiff_tid = -1, calldiff_mine = 0;
static int calldiff_per = 30000; // each thread's first calls ("CallDiff per=N")
extern "C" const char *droid_tool_args(const char *name); // game_settings.c
extern "C" char game_dir[256]; // game_settings.c: the game's folder
extern "C" int calldiff_on(void) {
    int s = calldiff_state.load(std::memory_order_relaxed);
    if (s >= 0) return s;
    std::lock_guard<std::mutex> g(calldiff_m);
    if (calldiff_state.load() < 0) {
        int on = droid_tool("CallDiff");
        if (on) {
            const char *a = droid_tool_args("CallDiff");
            const char *per = a ? std::strstr(a, "per=") : nullptr;
            if (per && std::atoi(per + 4) > 0) calldiff_per = std::atoi(per + 4);
            char path[512];
            std::snprintf(path, sizeof(path), "%s/calldiff.txt", game_dir);
            calldiff_file = std::fopen(path, "w");
            if (calldiff_file) std::setvbuf(calldiff_file, nullptr, _IOFBF, 1 << 20);
            droid_log("CallDiff: %s %s", calldiff_file ? "writing" : "can't write", path);
            on = calldiff_file != nullptr;
        }
        calldiff_state.store(on);
    }
    return calldiff_state.load();
}
static void calldiff_value(char *out, size_t n, std::uint64_t v) {
    if (v >= 0x1000000ull && v < 0xffff000000000000ull) std::snprintf(out, n, "p");
    else std::snprintf(out, n, "%#llx", (unsigned long long)v);
}
static bool calldiff_texty(const char *name, int arg) {
    static const char *const a0[] = { "strcmp", "strncmp", "strcasecmp", "strncasecmp", "strlen", "strstr", "strchr",
                                      "strrchr", "strdup", "getenv", "dlopen", "fopen", "open", "stat", "lstat",
                                      "access", "opendir", "__system_property_get", "atoi", "atof", "strtol",
                                      "strtoul", "strtod", "strtof", "setlocale", "mkdir", "realpath", "pthread_setname_np" };
    static const char *const a1[] = { "strcmp", "strncmp", "strcasecmp", "strncasecmp", "strstr", "dlsym", "openat",
                                      "fopen", "setlocale", "pthread_setname_np" };
    if (arg == 0) for (const char *k : a0) if (!std::strcmp(k, name)) return true;
    if (arg == 1) for (const char *k : a1) if (!std::strcmp(k, name)) return true;
    return false;
}
extern "C" void calldiff_log(const char *name, std::uint64_t lr, const std::uint64_t *x, std::uint64_t ret) {
    if (calldiff_mine >= calldiff_per || calldiff_lines.fetch_add(1, std::memory_order_relaxed) >= 3000000) return;
    calldiff_mine++;
    if (calldiff_tid < 0) calldiff_tid = calldiff_threads.fetch_add(1);
    if (!std::strcmp(name, "pthread_setname_np") && x[1] > 0x10000) { // the thread's name, to pair threads
        std::lock_guard<std::mutex> g(calldiff_m);
        std::fprintf(calldiff_file, "# T%d starts (%.31s)\n", calldiff_tid, reinterpret_cast<const char *>(x[1]));
    }
    char args[3][96], r[24], w[96];
    for (int i = 0; i < 3; i++) {
        bool text = calldiff_texty(name, i) && x[i] > 0x10000;
        if (text) {
            const char *str = reinterpret_cast<const char *>(x[i]);
            int k = 0;
            args[i][k++] = '"';
            for (int j = 0; j < 80 && str[j]; j++) args[i][k++] = (str[j] >= 32 && str[j] < 127 && str[j] != '"') ? str[j] : '.';
            args[i][k++] = '"';
            args[i][k] = 0;
        } else {
            calldiff_value(args[i], sizeof(args[i]), x[i]);
        }
    }
    calldiff_value(r, sizeof(r), ret);
    where(lr, w, sizeof(w));
    std::lock_guard<std::mutex> g(calldiff_m);
    std::fprintf(calldiff_file, "T%d %s(%s, %s, %s) -> %s  %s\n", calldiff_tid, name, args[0], args[1], args[2], r, w);
    if ((calldiff_lines.load(std::memory_order_relaxed) & 0x3fff) == 0) std::fflush(calldiff_file);
}
extern "C" void arm_calldiff_flush(void) {
    if (calldiff_state.load() != 1 || !calldiff_m.try_lock()) return; // (a crash may stop it mid-line)
    std::fflush(calldiff_file);
    calldiff_m.unlock();
}

// what a thread of ours is, for the thread report ("Looper main", "Choreographer"...)
extern "C" void arm_thread_role(const char *role) { thread_info()->role.store(role); }

// ---- ArmProf (the debug tool): where the game's ARM code spends its time --------------------
// tools.txt "ArmProf": every 2 ms each thread running ARM code (not in a call of ours) is
// stopped at its next block for its address (the report's UserDefined3 sample), and every
// 10 s the hottest 64-byte spots are logged as library+offset, all threads and the main one
// (thread 1). Its cost: one block exit per thread every 2 ms.
static void *armprof_thread(void *) {
    std::unordered_map<std::uint64_t, std::uint32_t> all, main_t, gc_t; // gc_t: while a GC has the game stopped
    std::uint64_t samples = 0, main_samples = 0, gc_samples = 0;
    std::int64_t window = mono_ns();
    for (;;) {
        usleep(2000);
        const int n = std::min(thread_info_count.load(), MAX_THREAD_INFO);
        for (int i = 0; i < n; i++) {
            ThreadInfo &t = thread_infos[i];
            if (!t.alive.load()) continue;
            if (t.sampled.exchange(0) == 1) { // last round's sample
                const std::uint64_t pc = t.sample_pc.load() & ~std::uint64_t(63);
                all[pc]++;
                samples++;
                if (i == 1) {
                    main_t[pc]++;
                    main_samples++;
                }
                if (gc_world_since.load(std::memory_order_relaxed)) {
                    gc_t[pc]++;
                    gc_samples++;
                }
            }
            void *c = t.running.load();
            if (c && !t.in_call.load()) static_cast<Cpu *>(c)->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
        }
        if (mono_ns() - window < 10000000000LL) continue;
        window = mono_ns();
        for (int pass = 0; pass < 3; pass++) {
            auto &m = pass == 2 ? gc_t : pass ? main_t : all;
            const std::uint64_t total = pass == 2 ? gc_samples : pass ? main_samples : samples;
            if (!total) continue;
            std::vector<std::pair<std::uint32_t, std::uint64_t>> v;
            v.reserve(m.size());
            for (auto &e : m) v.push_back({ e.second, e.first });
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
            droid_log("ArmProf: %s, %llu samples in 10 s:",
                      pass == 2 ? "while a GC had the game stopped" : pass ? "the main thread (1)" : "all threads",
                      (unsigned long long)total);
            for (size_t k = 0; k < v.size() && k < 20; k++) {
                char at[240];
                where(v[k].second, at, sizeof(at));
                droid_log("ArmProf:   %5.1f%%  %s", 100.0 * v[k].first / double(total), at);
            }
            m.clear();
        }
        samples = main_samples = gc_samples = 0;
    }
    return nullptr;
}
extern "C" void arm_prof_start(void) {
    static std::atomic<bool> on{ false };
    if (!droid_tool("ArmProf") || on.exchange(true)) return;
    pthread_t th;
    pthread_attr_t a; // its own stack size: the console's default thread stack is small
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);
    pthread_create(&th, &a, armprof_thread, nullptr);
    pthread_attr_destroy(&a);
    droid_log("ArmProf: on (every 2 ms, a report every 10 s)");
}

// The speed report (every 5 s with the fps line): which threads translated new code since the
// last one, the top few (Unity's job threads each translate the same code again)
// dynarmic's compile of one block, timed (third_party/patches/dynarmic-ps5.py)
// ---- Warm (tools.txt "Warm"): translate ahead -----------------------------------------------
// The blocks each game thread translated (as library+offset, under its start function) go to
// warm.txt in the game's folder, merged with the last run's; next launch, the first thread with
// that start function translates them before its first instruction (during the loading screen),
// so play doesn't stop for them. Threads that share a start function (job workers): the first only.
struct WarmList {
    std::vector<std::string> blocks;
    std::unordered_set<std::string> seen;
};
static std::mutex warm_m;
static std::unordered_map<std::string, WarmList> warm_now; // the last run's + this run's
static std::unordered_map<std::string, std::vector<std::string>> warm_saved; // the last run's
static std::unordered_set<std::string> warm_claimed;
static int warm_state = -1, warm_changed;
static int warm_on() {
    if (warm_state < 0) {
        std::lock_guard<std::mutex> g(warm_m);
        if (warm_state < 0) {
            warm_state = droid_tool("Warm");
            if (warm_state) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/warm.txt", game_dir);
                FILE *f = std::fopen(path, "r");
                size_t n = 0;
                if (f) {
                    char line[256];
                    while (std::fgets(line, sizeof(line), f)) {
                        char *tab = std::strchr(line, '\t');
                        if (!tab) continue;
                        *tab = 0;
                        char *end = tab + 1 + std::strcspn(tab + 1, "\r\n");
                        *end = 0;
                        std::string key = line, block = tab + 1;
                        WarmList &l = warm_now[key];
                        if (l.seen.insert(block).second) {
                            l.blocks.push_back(block);
                            warm_saved[key].push_back(block);
                            n++;
                        }
                    }
                    std::fclose(f);
                }
                droid_log("Warm: on, %zu blocks of %zu threads from the last run", n, warm_saved.size());
            }
        }
    }
    return warm_state;
}
static void warm_save() {
    if (warm_on() != 1) return;
    std::lock_guard<std::mutex> g(warm_m);
    if (!warm_changed) return;
    warm_changed = 0;
    char path[512], tmp[520];
    std::snprintf(path, sizeof(path), "%s/warm.txt", game_dir);
    std::snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = std::fopen(tmp, "w");
    if (!f) return;
    size_t n = 0;
    for (auto &kv : warm_now)
        for (auto &b : kv.second.blocks) {
            std::fprintf(f, "%s\t%s\n", kv.first.c_str(), b.c_str());
            n++;
        }
    std::fclose(f);
    std::rename(tmp, path);
    static int told;
    if (told++ < 6) droid_log("Warm: %zu blocks saved", n);
}
// library+offset -> its address now (0: not loaded, or past its end)
static std::uint64_t warm_address(const std::string &block) {
    size_t plus = block.rfind('+');
    if (plus == std::string::npos) return 0;
    std::string name = block.substr(0, plus);
    std::uint64_t off = std::strtoull(block.c_str() + plus + 1, nullptr, 16);
    for (int i = 0; i < arm_module_count(); i++) {
        so_module *m = arm_module(i);
        const char *mname = nullptr;
        if (!m || !m->base || !arm_module_at(reinterpret_cast<std::uint64_t>(m->base), &mname, nullptr) || !mname)
            continue;
        if (name == mname) return off < m->size ? reinterpret_cast<std::uint64_t>(m->base) + off : 0;
    }
    return 0;
}
extern "C" void arm_warm_ahead(void *cpu_p) {
    Cpu *cpu = static_cast<Cpu *>(cpu_p);
    if (!warm_pending || !warm_key[0]) return;
    warm_pending = 0;
    if (warm_on() != 1) return;
    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> g(warm_m);
        auto it = warm_saved.find(warm_key);
        if (it == warm_saved.end() || !warm_claimed.insert(warm_key).second) return;
        list = it->second;
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t done_n = 0;
    for (auto &b : list) {
        std::uint64_t a = warm_address(b);
        if (!a) continue;
        cpu->jit->Precompile(a);
        done_n++;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    droid_log("Warm: %zu blocks translated ahead for the thread starting at %s, %.0f ms", done_n, warm_key, ms);
}
// a game thread names itself (thread_place.c): its key gets the name (Unity's threads all start
// in one function: by start function only, UnityMain's list went to the asset collector), and its
// list is translated at the translator's next stop (between runs: not inside this call)
extern "C" void arm_warm_named(const char *name) {
    if (!warm_base[0] || !name || warm_on() != 1) return;
    std::snprintf(warm_key, sizeof(warm_key), "%s|%.31s", warm_base, name);
    warm_pending = 1;
    Cpu *cpu = static_cast<Cpu *>(thread_info()->running.load());
    if (cpu) cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined5);
}
// ---- Warm start (tools.txt "WarmStart", with "SharedJit"): the blocks this launch translated, kept
// by library + offset + FPCR in the game's warm_blocks.txt; next launch, two warm threads translate
// them ahead into the shared code cache on idle cores while the game loads, so its own threads find
// them done. Only addresses are kept: the code translated is the game's as it is now.
extern "C" const char *game_path(char *out, size_t n, const char *rel); // game_settings.c
static bool warm_start_on() {
    static int on = -1;
    if (on < 0) on = shared_jit_on() && !droid_tool("NoWarmStart") && (fast_jit_game() || droid_tool("WarmStart")) ? 1 : 0;
    return on == 1;
}
static std::mutex ws_m;
static std::unordered_set<std::string> ws_seen;
static std::vector<std::string> ws_new; // not in the file yet
static thread_local bool ws_warm_thread; // (this is a warm thread: its translations don't count as the game's)
static std::mutex ws_live_m;
static std::unordered_set<std::string> ws_live; // libraries the game itself has run code in
static bool warm_lib_live(const std::string &lib) {
    std::lock_guard<std::mutex> g(ws_live_m);
    return ws_live.count(lib) != 0;
}
static void warm_start_note(std::uint64_t pc, std::uint64_t fpcr) {
    // a warm thread's: from the file already (a line made and a global lock taken for each of ~500k
    // blocks, by six workshops at once, made them 3.6x slower each)
    if (ws_warm_thread) return;
    const char *name = nullptr;
    void *m = arm_module_at(pc, &name, nullptr);
    if (!m || !name) return; // only a library's code (its place is the same every launch)
    if (!ws_warm_thread) { // the game ran this library: it's loaded, set up and patched (unity_patch.c)
        std::lock_guard<std::mutex> g(ws_live_m);
        ws_live.insert(name);
    }
    char line[160];
    std::snprintf(line, sizeof(line), "%s %llx %llx", name, (unsigned long long)(pc - *static_cast<std::uint64_t *>(m)),
                  (unsigned long long)fpcr);
    std::lock_guard<std::mutex> g(ws_m);
    if (ws_seen.insert(line).second) ws_new.emplace_back(line);
}
static void warm_start_save() {
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> g(ws_m);
        out.swap(ws_new);
    }
    if (out.empty()) return;
    char p[512];
    game_path(p, sizeof(p), "warm_blocks.txt");
    if (std::FILE *f = std::fopen(p, "a")) {
        for (const auto &l : out) std::fprintf(f, "%s\n", l.c_str());
        std::fclose(f);
    }
}
extern "C" void arm_warm_start(void) {
    if (!warm_start_on()) return;
    char p[512];
    game_path(p, sizeof(p), "warm_blocks.txt");
    std::FILE *f = std::fopen(p, "r");
    if (!f) {
        droid_log("warm: no list yet (this launch makes it)");
        return;
    }
    // the list by library (Unity loads libunity / libil2cpp itself, later: each library's blocks
    // are translated once it is there)
    struct Item { std::uint64_t off, fpcr; };
    auto *by_lib = new std::unordered_map<std::string, std::vector<Item>>;
    char lib[128];
    unsigned long long off, fpcr;
    size_t total = 0;
    while (std::fscanf(f, "%127s %llx %llx", lib, &off, &fpcr) == 3) {
        {
            std::lock_guard<std::mutex> g(ws_m);
            char line[160];
            std::snprintf(line, sizeof(line), "%s %llx %llx", lib, off, fpcr);
            ws_seen.insert(line); // (in the file already: not written again)
        }
        (*by_lib)[lib].push_back({ off, fpcr });
        total++;
    }
    std::fclose(f);
    // parallel (PARALLEL_WARM_GAMES): WARM_WORKSHOPS threads, each in its own workshop, each a slice of
    // every library's addresses (its blocks then link to each other), in the order the game first ran them
    static const bool parallel = !droid_tool("NoParallelWarm") &&
                                 (droid_game_in(PARALLEL_WARM_GAMES) || droid_tool("ParallelWarm"));
    const int threads = parallel ? WARM_WORKSHOPS : 2;
    auto *owner = new std::unordered_map<std::string, std::vector<std::uint8_t>>; // each block's thread
    for (auto &kv : *by_lib) {
        const auto &items = kv.second;
        auto &o = (*owner)[kv.first];
        o.resize(items.size());
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        if (parallel)
            std::sort(order.begin(), order.end(), [&items](size_t a, size_t b) { return items[a].off < items[b].off; });
        for (size_t r = 0; r < order.size(); r++)
            o[order[r]] = static_cast<std::uint8_t>(parallel ? r * threads / order.size() : r % threads);
    }
    droid_log("warm: %zu blocks of %zu libraries to translate ahead, on %d threads%s (as their libraries load)", total,
              by_lib->size(), threads, parallel ? " in parallel workshops" : "");
    auto *libs = new std::vector<std::string>;
    for (auto &kv : *by_lib) libs->push_back(kv.first);
    for (int t = 0; t < threads; t++) {
        std::thread([by_lib, libs, owner, t] {
            // its own Jit, made as a game thread's (on the shared code cache, or its own workshop), kept for good
            ws_warm_thread = true;
            if (parallel) {
                tl_workshop = t + 1;
                thread_place_rest(); // off the main / render threads' cores
            }
            Cpu *c = make_cpu(false);
            if (!c) return;
            const auto t0 = std::chrono::steady_clock::now();
            size_t done = 0;
            std::vector<bool> finished(libs->size(), false);
            size_t left = libs->size();
            while (left && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(90)) {
                bool any = false;
                for (size_t li = 0; li < libs->size(); li++) {
                    if (finished[li]) continue;
                    std::uint64_t b = 0;
                    for (int i = 0, n = arm_module_count(); i < n && !b; i++) {
                        const so_module *mod = arm_module(i);
                        const char *name = nullptr;
                        if (arm_module_at(reinterpret_cast<std::uint64_t>(mod->base), &name, nullptr) && name &&
                            (*libs)[li] == name)
                            b = reinterpret_cast<std::uint64_t>(mod->base);
                    }
                    // not loaded yet, or the game hasn't run it yet (patches may still be coming)
                    if (!b || !warm_lib_live((*libs)[li])) continue;
                    const auto &items = (*by_lib)[(*libs)[li]];
                    const auto &mine = (*owner)[(*libs)[li]];
                    for (size_t k = 0; k < items.size(); k++) {
                        if (mine[k] != t) continue;
                        c->jit->SetFpcr(static_cast<std::uint32_t>(items[k].fpcr));
                        c->jit->Precompile(b + items[k].off);
                        done++;
                    }
                    finished[li] = true;
                    left--;
                    any = true;
                }
                if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            droid_log("warm: thread %d translated %zu blocks ahead in %.1f s%s", t, done,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                      left ? " (some libraries never loaded)" : "");
        }).detach();
    }
}

extern "C" void droid_translate_ns(std::uint64_t ns, std::uint64_t pc, std::uint64_t fpcr) {
    if (my_info) my_info->translate_ns.fetch_add(ns, std::memory_order_relaxed);
    if (warm_start_on()) warm_start_note(pc, fpcr);
    if (ws_warm_thread || !warm_key[0] || warm_on() != 1) return; // (a warm thread's: not the game's own)
    char at[96];
    where(pc, at, sizeof(at));
    if (!std::strchr(at, '+')) return; // not in a library (V8's own code, a trampoline): not kept
    std::lock_guard<std::mutex> g(warm_m);
    WarmList &l = warm_now[warm_key];
    if (l.blocks.size() < 300000 && l.seen.insert(at).second) {
        l.blocks.push_back(at);
        warm_changed = 1;
    }
}
// where a translation's time goes (the dynarmic patch: TSC cycles of each part), all threads
static std::atomic<std::uint64_t> phase_cycles[3];
extern "C" void droid_translate_phases(std::uint64_t decode, std::uint64_t passes, std::uint64_t emit) {
    phase_cycles[0].fetch_add(decode, std::memory_order_relaxed);
    phase_cycles[1].fetch_add(passes, std::memory_order_relaxed);
    phase_cycles[2].fetch_add(emit, std::memory_order_relaxed);
}
extern "C" void arm_translate_report(void) {
    {
        static int calls;
        if (++calls % 6 == 0) warm_save(); // (Warm: every 30 s)
        if (calls % 2 == 0) warm_start_save(); // (WarmStart: every 10 s, the new ones appended)
    }
    static std::uint64_t last[MAX_THREAD_INFO], last_ns[MAX_THREAD_INFO];
    struct { int i; std::uint64_t n, ns; } top[5] = {};
    std::uint64_t all = 0, all_ns = 0;
    int threads = 0;
    const int n = std::min(thread_info_count.load(), MAX_THREAD_INFO);
    for (int i = 0; i < n; i++) {
        const std::uint64_t now = thread_infos[i].translated.load(std::memory_order_relaxed);
        const std::uint64_t d = now >= last[i] ? now - last[i] : now;
        last[i] = now;
        const std::uint64_t now_ns = thread_infos[i].translate_ns.load(std::memory_order_relaxed);
        const std::uint64_t dns = now_ns >= last_ns[i] ? now_ns - last_ns[i] : now_ns;
        last_ns[i] = now_ns;
        if (!d) continue;
        all += d;
        all_ns += dns;
        threads++;
        for (int k = 0; k < 5; k++)
            if (d > top[k].n) {
                for (int m = 4; m > k; m--) top[m] = top[m - 1];
                top[k] = { i, d, dns };
                break;
            }
    }
    if (all < 2000) return;
    {
        static std::uint64_t was[3];
        std::uint64_t d[3], sum = 0;
        for (int k = 0; k < 3; k++) {
            const std::uint64_t v = phase_cycles[k].load(std::memory_order_relaxed);
            d[k] = v - was[k];
            was[k] = v;
            sum += d[k];
        }
        if (sum)
            droid_log("arm: translating: ARM decode %.0f%%, IR passes %.0f%%, x86 emit %.0f%% (of the timed part)",
                      100.0 * d[0] / sum, 100.0 * d[1] / sum, 100.0 * d[2] / sum);
    }
    char line[400];
    int at = snprintf(line, sizeof(line), "arm: new code %lluK on %d threads, %.0f ms translating (%.1f us each):",
                      (unsigned long long)(all / 1000), threads, all_ns / 1e6, all ? all_ns / 1e3 / all : 0.0);
    for (int k = 0; k < 5 && top[k].n && at < (int)sizeof(line) - 60; k++) {
        const char *role = thread_infos[top[k].i].role.load();
        at += snprintf(line + at, sizeof(line) - size_t(at), " #%d%s%s %lluK %.0fms", top[k].i, role ? " " : "",
                       role ? role : "", (unsigned long long)(top[k].n / 1000), top[k].ns / 1e6);
    }
    droid_log("%s", line);
}
extern "C" int arm_sig_waiting(void) { // a signal this thread should take now
    return sig_used.load(std::memory_order_relaxed) && my_info && (my_info->pending.load() & ~sig_blocked) != 0;
}

// the calling thread is the game's main one: its ARM code gets an 8 MB stack (make_cpu)
extern "C" void arm_want_big_stack(void) { want_big = true; }
