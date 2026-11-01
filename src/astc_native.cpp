// astc_native.cpp — Unity's ASTC textures decoded by the host, not through the ARM translator.
// The PS5's GPU (and the PC's AMD) has no ASTC: Unity decodes every ASTC texture on the CPU
// with ARM's astcenc built into libunity.so, on its job threads. Through the translator that
// was most of a game's startup (Among Us: 7 threads of NEON code for about 15 s). Here the
// same astcenc (ARM-software/astc-encoder 4.8.0, Apache-2.0) is built for the host with its
// AVX2 code, and libunity's astcenc_decompress_image jumps to it.
//
// How: when libunity.so loads (before its constructors), its astcenc_decompress_image is
// found by Unity's job function that calls it (below) and its first 16 bytes become
// "ldr x16, #8; br x16; <our thunk>". Unity's context stays Unity's (made by its own
// astcenc_context_alloc): astcenc keeps its config at the very start of the context
// (profile, flags, block x/y/z), the block size is read there. Unity hands every job thread
// the whole image with its thread index (astcenc's way to share an image between threads):
// index 0 decodes all of it here, the others are done at once.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include "astcenc.h"

extern "C" void droid_log(const char *fmt, ...);
extern "C" uint64_t arm_thunk(const char *name, void *fn);
extern "C" int mprotect(void *addr, size_t len, int prot);

namespace {

struct Ctx {
    unsigned profile, bx, by, bz;
    astcenc_context *ctx;
};
thread_local Ctx cache[8];
thread_local int cache_used;

astcenc_context *native_context(unsigned profile, unsigned bx, unsigned by, unsigned bz) {
    for (int i = 0; i < cache_used; i++)
        if (cache[i].profile == profile && cache[i].bx == bx && cache[i].by == by && cache[i].bz == bz)
            return cache[i].ctx;
    astcenc_config cfg;
    if (astcenc_config_init(static_cast<astcenc_profile>(profile), bx, by, bz, ASTCENC_PRE_FASTEST,
                            ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS)
        return nullptr;
    astcenc_context *ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) return nullptr;
    int slot = cache_used < 8 ? cache_used++ : 7;
    if (slot == 7 && cache[7].ctx) astcenc_context_free(cache[7].ctx);
    cache[slot] = { profile, bx, by, bz, ctx };
    return ctx;
}

std::uint64_t images, failures;

// astcenc_decompress_image(context, data, data_len, image_out, swizzle, thread_index)
std::uint64_t decompress(std::uint64_t arm_ctx, std::uint64_t data, std::uint64_t len, std::uint64_t image,
                         std::uint64_t swizzle, std::uint64_t thread_index) {
    if (static_cast<std::uint32_t>(thread_index) != 0) return ASTCENC_SUCCESS; // index 0 does it all
    const auto *cfg = reinterpret_cast<const std::uint32_t *>(arm_ctx);
    const unsigned profile = cfg[0], bx = cfg[2], by = cfg[3], bz = cfg[4];
    astcenc_context *ctx = profile <= 3 && bx >= 3 && bx <= 12 && by >= 3 && by <= 12 && bz >= 1 && bz <= 6
                               ? native_context(profile, bx, by, bz)
                               : nullptr;
    astcenc_error r = ASTCENC_ERR_BAD_PARAM;
    if (ctx) {
        r = astcenc_decompress_image(ctx, reinterpret_cast<const std::uint8_t *>(data), len,
                                     reinterpret_cast<astcenc_image *>(image),
                                     reinterpret_cast<const astcenc_swizzle *>(swizzle), 0);
        astcenc_decompress_reset(ctx);
    }
    const std::uint64_t n = __atomic_add_fetch(&images, 1, __ATOMIC_RELAXED);
    if (r != ASTCENC_SUCCESS && __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED) <= 8)
        droid_log("astc: native decode failed (%s): profile %u, block %ux%ux%u", astcenc_get_error_string(r),
                  profile, bx, by, bz);
    if (n == 1 || n == 10 || n == 100 || n == 1000) {
        const auto *img = reinterpret_cast<const std::uint32_t *>(image);
        droid_log("astc: %llu texture(s) decoded natively (last %ux%u, block %ux%u)", (unsigned long long)n,
                  img[0], img[1], bx, by);
    }
    return r;
}

// Unity's job function: astcenc_decompress_image(ctx, data, len, image, &swizzle, job index).
// Its code: mov w5, w1 / mov x4, sp / ldp x2, x3, [x0, #0x10] / ... / bl astcenc_decompress_image
std::uint64_t find_decompress(std::uint64_t base, std::size_t size) {
    const auto *w = reinterpret_cast<const std::uint32_t *>(base);
    const std::size_t n = size / 4;
    std::uint64_t found = 0;
    int hits = 0;
    for (std::size_t i = 0; i + 12 < n; i++) {
        if (w[i] != 0x2a0103e5u) continue; // mov w5, w1
        bool sp = false, ldp = false;
        for (std::size_t k = (i >= 6 ? i - 6 : 0); k <= i + 6; k++) {
            if (w[k] == 0x910003e4u) sp = true;  // mov x4, sp
            if (w[k] == 0xa9410c02u) ldp = true; // ldp x2, x3, [x0, #0x10]
        }
        if (!sp || !ldp) continue;
        for (std::size_t k = i + 1; k <= i + 8; k++) {
            if ((w[k] & 0xFC000000u) != 0x94000000u) continue; // bl
            const std::int64_t off = static_cast<std::int64_t>(static_cast<std::int32_t>(w[k] << 6) >> 6) * 4;
            const std::uint64_t target = base + (i + (k - i)) * 4 + off;
            if (target != found) hits++;
            found = target;
            break;
        }
    }
    return hits == 1 ? found : 0;
}

} // namespace

// a library loaded, its constructors not run yet: libunity.so's astcenc gets ours
extern "C" void astc_native_attach(const char *lib, std::uint64_t base, std::size_t size) {
    if (std::strcmp(lib, "libunity.so") != 0) return;
    const std::uint64_t at = find_decompress(base, size);
    if (!at) {
        droid_log("astc: libunity.so's astcenc_decompress_image not found: Unity decodes ASTC itself");
        return;
    }
    const std::uint64_t thunk = arm_thunk("astc_native_decompress", reinterpret_cast<void *>(&decompress));
    if (!thunk) return;
    const std::uint64_t page = at & ~std::uint64_t(0xFFF);
    mprotect(reinterpret_cast<void *>(page), 0x2000, 7); // read, write, exec
    std::uint32_t code[4] = { 0x58000050u, 0xd61f0200u, 0, 0 }; // ldr x16, #8; br x16
    std::memcpy(&code[2], &thunk, 8);
    std::memcpy(reinterpret_cast<void *>(at), code, 16);
    droid_log("astc: libunity.so+%#llx (astcenc_decompress_image) decoded natively (astcenc 4.8, AVX2)",
              (unsigned long long)(at - base));
}
