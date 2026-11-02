// ib_native.cpp — the Infinity Blade 2/3 ports' texture conversion as native PS5 code.
// The ports (libib3.so / libib2.so) turn every iPhone texture (PVRTC) into ETC2 at load: their
// pvrtc_decode and etcpak's CompressEtc2Rgb / Rgba, as translated ARM (NEON) code on the JIT,
// took IB3's first start 48 s on a black screen (1661 textures). All three are called through the
// library's PLT (JUMP_SLOT), so soloader binds them here instead (arm_modules.c: so_arm_interpose):
// the same decoder (third_party/ib-pvrtc, MIT) and the same encoder (third_party/etcpak, BSD),
// compiled for the PS5's Zen 2 (AVX2). The same blocks come out: the ports' texture cache
// stays valid either way.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <algorithm>

#include "../third_party/etcpak/ProcessRGB.hpp"

namespace gles {
void pvrtc_decode(const uint8_t *src, int width, int height, bool two_bpp, uint8_t *rgba_out);
}

extern "C" {
int droid_game_in(const char *list);
void droid_log(const char *fmt, ...);
uint64_t arm_thunk(const char *name, void *fn);
}

namespace {
std::atomic<uint64_t> n_decoded{0}, n_encoded{0}, us_decode{0}, us_encode{0};

uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// per texture, from its cache key (the port's first step for every PVRTC upload) to the next one: the
// whole conversion's wall time; "other" = what isn't native decode / encode (the port's ARM code around them)
std::atomic<uint64_t> n_keys{0}, us_between{0};
thread_local uint64_t key_at = 0;
void report() { // every 200 textures: how much the native code did, and in how long
    const uint64_t d = n_decoded.load();
    if (d && d % 200 == 0) {
        const uint64_t all = us_between.load(), dec = us_decode.load(), enc = us_encode.load();
        droid_log("ib: native textures: %llu decoded (%llu ms), %llu encode calls (%llu ms); %llu keys, %llu ms from a key to "
                  "the next on its thread (other work: %lld ms)", (unsigned long long)d, (unsigned long long)(dec / 1000),
                  (unsigned long long)n_encoded.load(), (unsigned long long)(enc / 1000), (unsigned long long)n_keys.load(),
                  (unsigned long long)(all / 1000), (long long)((int64_t)all - (int64_t)dec - (int64_t)enc) / 1000);
    }
}

void n_pvrtc_decode(const uint8_t *src, int w, int h, bool two_bpp, uint8_t *rgba) {
    const uint64_t t = now_us();
    gles::pvrtc_decode(src, w, h, two_bpp, rgba);
    us_decode += now_us() - t;
    n_decoded++;
    report();
}
// A big texture's block rows over a few host threads (the port's own split, below, is off: it started three
// game threads per texture, each a new translated thread with its own 2 MB stack, ~30-80 ms a texture)
template <typename F>
void split_rows(F encode, const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width, bool heuristics, int words) {
    const uint32_t per_row = (uint32_t)(width / 4);
    const uint32_t rows = per_row ? blocks / per_row : 0;
    int threads = blocks >= 16384 && per_row && blocks % per_row == 0 ? (int)std::min<uint32_t>(6, rows) : 1;
    if (threads <= 1) {
        encode(src, dst, blocks, width, heuristics);
        return;
    }
    std::thread pool[6];
    const uint32_t step = (rows + threads - 1) / threads;
    for (int i = 0; i < threads; i++) {
        const uint32_t r0 = i * step, r1 = std::min(rows, r0 + step);
        if (r0 >= r1) break;
        pool[i] = std::thread(encode, src + (size_t)r0 * 4 * width, dst + (size_t)r0 * per_row * words,
                              (r1 - r0) * per_row, width, heuristics);
    }
    for (auto &t : pool)
        if (t.joinable()) t.join();
}
void n_etc2_rgb(const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width, bool heuristics) {
    const uint64_t t = now_us();
    split_rows(CompressEtc2Rgb, src, dst, blocks, width, heuristics, 1);
    us_encode += now_us() - t;
    n_encoded++;
}
void n_etc2_rgba(const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width, bool heuristics) {
    const uint64_t t = now_us();
    split_rows(CompressEtc2Rgba, src, dst, blocks, width, heuristics, 2);
    us_encode += now_us() - t;
    n_encoded++;
}
// gles::texture_cache_key, natively: the port's own function, the same arithmetic (its cache stays valid)
uint64_t n_texture_cache_key(const void *data, unsigned long size, unsigned int format, int w, int h) {
    const uint64_t t = now_us();
    if (key_at) us_between += t - key_at;
    key_at = t;
    n_keys++;
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)format << 40) ^ ((uint64_t)(uint32_t)w << 20) ^ (uint64_t)(uint32_t)h ^
                 ((uint64_t)size << 48);
    auto mix = [&](uint64_t v) {
        x ^= v;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 31;
    };
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        mix(v);
    }
    uint64_t tail = 0;
    std::memcpy(&tail, p + i, size - i);
    mix(tail ^ size);
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 29);
}
// std::thread::hardware_concurrency, for the port: 1 (it only sizes its ETC2 encoder's thread split, gl.cpp)
unsigned n_hardware_concurrency() { return 1; }
} // namespace

// soloader's interpose while an Infinity Blade port's library loads: its own definitions of these,
// bound to the native ones (an ARM-callable thunk each); everything else as it was
extern "C" void *ib_interpose(const char *name) {
    static const struct { const char *sym; void *fn; } swaps[] = {
        { "_ZN4gles12pvrtc_decodeEPKhiibPh", (void *)n_pvrtc_decode },
        { "_Z15CompressEtc2RgbPKjPmjmb", (void *)n_etc2_rgb },
        { "_Z16CompressEtc2RgbaPKjPmjmb", (void *)n_etc2_rgba },
        { "_ZN4gles17texture_cache_keyEPKvmjii", (void *)n_texture_cache_key },
        { "_ZNSt6__ndk16thread20hardware_concurrencyEv", (void *)n_hardware_concurrency },
    };
    for (const auto &s : swaps)
        if (!std::strcmp(name, s.sym)) {
            static int told;
            if (told++ < 5) droid_log("ib: %s runs natively (x86) instead of translated ARM", s.sym);
            return (void *)(uintptr_t)arm_thunk(s.sym, s.fn);
        }
    return nullptr;
}
extern "C" int ib_native_game(void) { return droid_game_in("com.ib3port.,com.ib2port.,com.ib1port.") != 0; }

// ---- the movie keeper ---------------------------------------------------------------------------
// The ports show a movie two ways: their boot screen (a thread of their own drawing the movie until
// the game's first frame), then inside the game's own frames. IB2's startup movie (18 s) froze from
// 9 s to its end: the game presented its first frame at 9 s (the boot screen ended) and then loaded
// for 12 s without presenting, so the picture stood still while its sound went on.
// When the game hasn't presented for 150 ms while a movie is showing, the port's own boot screen is
// started again (uikit::create_boot_window; its first-frame flag set back to false): it draws the
// movie, taking turns with the game's presents on its own lock, and ends at the game's next frame.
// The flag isn't exported: gles::window_presented() reads it (adrp; add; ldarb; ret), decoded here.
#include <thread>
namespace {
typedef uint64_t (*u64_fn)(void);
typedef void *(*ptr_fn)(void);
typedef int (*frame_fn)(const uint8_t **rgba, int *w, int *h, unsigned long *serial);
typedef void (*void_fn)(void);
typedef void (*boot_fn)(void *window, const void *name);

uint8_t *flag_from(const uint32_t *code) { // adrp xN, page; add xN, xN, #off; ldarb w0, [xN]
    const uint32_t adrp = code[0], add = code[1], ld = code[2];
    if ((adrp & 0x9f000000) != 0x90000000 || (add & 0xff800000) != 0x91000000 || (ld & 0xfffffc00) != 0x08dffc00)
        return nullptr;
    int64_t imm = (int64_t)(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3));
    if (imm & (1 << 20)) imm -= 1 << 21;
    const uint64_t page = ((uint64_t)code & ~0xfffull) + (uint64_t)(imm << 12);
    const uint64_t off = (uint64_t)((add >> 10) & 0xfff) << (((add >> 22) & 1) ? 12 : 0);
    return (uint8_t *)(page + off);
}
} // namespace

extern "C" void *arm_callback(uint64_t guest);
// the port's functions (their addresses in the loaded library; 0: not there, no keeper)
extern "C" void ib_movie_keeper_start(uint64_t window_presented, uint64_t frames_presented, uint64_t main_window,
                                      uint64_t current_frame, uint64_t release_frame, uint64_t create_boot_window) {
    if (!window_presented || !frames_presented || !main_window || !current_frame || !release_frame || !create_boot_window)
        return;
    uint8_t *flag = flag_from((const uint32_t *)window_presented);
    if (!flag) {
        droid_log("ib: movie keeper off (the first-frame flag not found)");
        return;
    }
    droid_log("ib: movie keeper on (the boot screen draws a movie while the game doesn't)");
    const u64_fn frames = (u64_fn)arm_callback(frames_presented);
    const ptr_fn window = (ptr_fn)arm_callback(main_window);
    const frame_fn frame = (frame_fn)arm_callback(current_frame);
    const void_fn release = (void_fn)arm_callback(release_frame);
    const boot_fn boot = (boot_fn)arm_callback(create_boot_window);
    std::thread([=] {
        static uint64_t no_name[4]; // an empty std::wstring (libc++: all zero)
        uint64_t last = 0, since = now_us();
        bool keeping = false;
        int times = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!keeping && !__atomic_load_n(flag, __ATOMIC_ACQUIRE)) continue; // (the port's own boot screen is up)
            const uint64_t n = frames(), t = now_us();
            if (keeping) {
                if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) { // the game drew: the boot screen ended
                    keeping = false;
                    if (times <= 20) droid_log("ib: movie keeper: the game draws again after %.1f s", (t - since) / 1e6);
                    last = n, since = t;
                }
                continue;
            }
            if (n != last || !n) { last = n, since = t; continue; }
            if (t - since < 150000) continue;
            const uint8_t *rgba = nullptr;
            int w = 0, h = 0;
            unsigned long serial = 0;
            if (!frame(&rgba, &w, &h, &serial)) { since = t; continue; } // (no movie: a slow frame only)
            release();
            void *win = window();
            if (!win) continue;
            __atomic_store_n(flag, 0, __ATOMIC_RELEASE);
            boot(win, no_name);
            keeping = true;
            if (++times <= 20) droid_log("ib: movie keeper: the game stopped drawing during a movie: the boot screen shows it");
        }
    }).detach();
}
