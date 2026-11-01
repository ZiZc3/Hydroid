// etc_native.cpp — Unity's ETC2 / EAC textures decoded by the host, not through the ARM translator.
// The PS5's GPU (and the PC's AMD) has no ETC2: Unity turns every ETC texture into RGBA8 on the
// CPU when a scene loads. Through the translator that was Cut the Rope's 5-7 fps for seconds on
// entering a level (ArmProf: ~40% of all samples in libunity's ETC block code, the main thread
// waiting for the render thread's texture uploads; GC only 13 ms of it).
//
// Unity's texture decoder: five small functions (ETC2 RGB, ETC2 RGB A1, ETC2 RGBA8, EAC R11,
// EAC RG11), each "sub sp; str x30; mov x9, x1; mov w8, w4; ...; adr x0..x3; bl <block loop>":
// the loop gets two (fetch block, decode block) pairs. A decode function ("leaf") turns one
// 64-bit block into a 4x4 RGBA temp and copies clipW x clipH of it to dst:
//   leaf(block, dst, pitch, 4, 4, clipW, clipH, 4 bytes a pixel)
// The leaves are ours here, but only after a check: at load, each of Unity's leaves (through a
// trampoline: its first 4 instructions, then a jump back) and each of our decoders run the same
// few thousand random blocks (every mode: individual, differential, T, H, planar; clipped
// copies) into the same buffers; a leaf is replaced only by a decoder whose bytes all match.
// None matches: Unity's code is put back as it was (nothing else runs it yet: before the
// library's constructors). tools.txt "NoEtcNative": Unity's own decoders.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void droid_log(const char *fmt, ...);
extern "C" uint64_t arm_thunk(const char *name, void *fn);
extern "C" uint64_t arm_call(uint64_t fn, const uint64_t *args, int count, int *ok);
extern "C" int droid_tool(const char *name);
extern "C" int mprotect(void *addr, size_t len, int prot);

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline unsigned bits(u64 b, int hi, int lo) { return unsigned(b >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline u8 clamp255(int v) { return u8(v < 0 ? 0 : v > 255 ? 255 : v); }
inline int ext4(int v) { return (v << 4) | v; }
inline int ext5(int v) { return (v << 3) | (v >> 2); }
inline int ext6(int v) { return (v << 2) | (v >> 4); }
inline int ext7(int v) { return (v << 1) | (v >> 6); }
inline int sign3(unsigned v) { return int(v ^ 4) - 4; }

const int etc1_mod[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };
const int etc_dist[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };
const int eac_mod[16][8] = {
    { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 },
    { -2, -4, -6, -13, 1, 3, 5, 12 }, { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 },
    { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 }, { -2, -6, -8, -10, 1, 5, 7, 9 },
    { -2, -5, -8, -10, 1, 4, 7, 9 },  { -2, -4, -8, -10, 1, 3, 7, 9 },  { -2, -5, -7, -10, 1, 4, 6, 9 },
    { -3, -4, -7, -10, 2, 3, 6, 9 },  { -1, -2, -3, -10, 0, 1, 2, 9 },  { -4, -6, -8, -9, 3, 5, 7, 8 },
    { -3, -5, -7, -9, 2, 4, 6, 8 },
};

// the 2-bit index of pixel (x, y): bit 4x+y (low), bit 4x+y+16 (high)
inline int pix_index(u64 b, int x, int y) {
    const int j = x * 4 + y;
    return int(((b >> j) & 1) | (((b >> (j + 16)) & 1) << 1));
}

inline void set(u8 *p, int r, int g, int bl, int a) {
    p[0] = clamp255(r);
    p[1] = clamp255(g);
    p[2] = clamp255(bl);
    p[3] = u8(a);
}

// one ETC2 RGB block (punch: the RGB A1 kind, bit 33 = opaque) -> o[y*4+x] RGBA
void etc2_block(u64 b, bool punch, u8 o[16][4]) {
    const bool opaque = punch ? bits(b, 33, 33) != 0 : true;
    const bool diff = punch ? true : bits(b, 33, 33) != 0;
    int c[2][3];
    if (diff) {
        const int r = int(bits(b, 63, 59)), g = int(bits(b, 55, 51)), bl = int(bits(b, 47, 43));
        const int r2 = r + sign3(bits(b, 58, 56)), g2 = g + sign3(bits(b, 50, 48)), b2 = bl + sign3(bits(b, 42, 40));
        if (r2 < 0 || r2 > 31 || g2 < 0 || g2 > 31) { // T or H
            int p[4][3];
            if (r2 < 0 || r2 > 31) { // T
                const int c1[3] = { ext4(int((bits(b, 60, 59) << 2) | bits(b, 57, 56))), ext4(int(bits(b, 55, 52))),
                                    ext4(int(bits(b, 51, 48))) };
                const int c2[3] = { ext4(int(bits(b, 47, 44))), ext4(int(bits(b, 43, 40))), ext4(int(bits(b, 39, 36))) };
                const int d = etc_dist[(bits(b, 35, 34) << 1) | bits(b, 32, 32)];
                for (int k = 0; k < 3; k++) {
                    p[0][k] = c1[k];
                    p[1][k] = c2[k] + d;
                    p[2][k] = c2[k];
                    p[3][k] = c2[k] - d;
                }
            } else { // H
                const int r1 = int(bits(b, 62, 59)), g1 = int((bits(b, 58, 56) << 1) | bits(b, 52, 52)),
                          b1 = int((bits(b, 51, 51) << 3) | bits(b, 49, 47));
                const int rr = int(bits(b, 46, 43)), gg = int(bits(b, 42, 39)), bb = int(bits(b, 38, 35));
                const int k1 = (r1 << 8) | (g1 << 4) | b1, k2 = (rr << 8) | (gg << 4) | bb;
                const int d = etc_dist[(bits(b, 34, 34) << 2) | (bits(b, 32, 32) << 1) | (k1 >= k2 ? 1 : 0)];
                const int c1[3] = { ext4(r1), ext4(g1), ext4(b1) }, c2[3] = { ext4(rr), ext4(gg), ext4(bb) };
                for (int k = 0; k < 3; k++) {
                    p[0][k] = c1[k] + d;
                    p[1][k] = c1[k] - d;
                    p[2][k] = c2[k] + d;
                    p[3][k] = c2[k] - d;
                }
            }
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    const int i = pix_index(b, x, y);
                    if (!opaque && i == 2) set(o[y * 4 + x], 0, 0, 0, 0);
                    else set(o[y * 4 + x], p[i][0], p[i][1], p[i][2], 255);
                }
            return;
        }
        if (b2 < 0 || b2 > 31) { // planar
            const int ro = ext6(int(bits(b, 62, 57))), go = ext7(int((bits(b, 56, 56) << 6) | bits(b, 54, 49))),
                      bo = ext6(int((bits(b, 48, 48) << 5) | (bits(b, 44, 43) << 3) | bits(b, 41, 39)));
            const int rh = ext6(int((bits(b, 38, 34) << 1) | bits(b, 32, 32))), gh = ext7(int(bits(b, 31, 25))),
                      bh = ext6(int(bits(b, 24, 19)));
            const int rv = ext6(int(bits(b, 18, 13))), gv = ext7(int(bits(b, 12, 6))), bv = ext6(int(bits(b, 5, 0)));
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    set(o[y * 4 + x], (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
                        (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
                        (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2, 255);
            return;
        }
        c[0][0] = ext5(r), c[0][1] = ext5(g), c[0][2] = ext5(bl);
        c[1][0] = ext5(r2), c[1][1] = ext5(g2), c[1][2] = ext5(b2);
    } else {
        c[0][0] = ext4(int(bits(b, 63, 60))), c[0][1] = ext4(int(bits(b, 55, 52))), c[0][2] = ext4(int(bits(b, 47, 44)));
        c[1][0] = ext4(int(bits(b, 59, 56))), c[1][1] = ext4(int(bits(b, 51, 48))), c[1][2] = ext4(int(bits(b, 43, 40)));
    }
    const int cw[2] = { int(bits(b, 39, 37)), int(bits(b, 36, 34)) };
    const bool flip = bits(b, 32, 32) != 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            const int s = flip ? (y >= 2) : (x >= 2);
            const int i = pix_index(b, x, y);
            int m;
            if (!opaque) {
                if (i == 2) {
                    set(o[y * 4 + x], 0, 0, 0, 0);
                    continue;
                }
                m = i == 0 ? 0 : i == 1 ? etc1_mod[cw[s]][1] : -etc1_mod[cw[s]][1];
            } else {
                m = etc1_mod[cw[s]][i & 1];
                if (i & 2) m = -m;
            }
            set(o[y * 4 + x], c[s][0] + m, c[s][1] + m, c[s][2] + m, 255);
        }
}

// one EAC block (Unity's 8-bit way for alpha, R11 and RG11 alike) -> v[y*4+x]
void eac_block(u64 b, u8 v[16]) {
    const int base = int(b >> 56), mult = int(bits(b, 55, 52));
    const int *t = eac_mod[bits(b, 51, 48)];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) v[y * 4 + x] = clamp255(base + mult * t[(b >> (45 - 3 * (4 * x + y))) & 7]);
}

enum Kind { K_RGBA, K_RGB, K_A1, K_EAC_A, K_EAC_R, K_EAC_G, K_COUNT };
const char *const kind_name[K_COUNT] = { "ETC2 RGB", "ETC2 RGB (of RGBA8)", "ETC2 RGB A1", "EAC alpha", "EAC R11",
                                         "EAC G (RG11)" };

// The leaf's copy out of its 4x4 temp: row r of the clip reads the temp from r * clipW (Unity's
// way: the same as a plain copy for whole blocks, its own for the clipped ones at an edge).
void leaf(Kind k, u64 b, u8 *dst, int pitch, int cw, int ch, int bpp) {
    u8 o[16][4];
    if (k <= K_A1) {
        etc2_block(b, k == K_A1, o);
    } else {
        u8 v[16];
        eac_block(b, v);
        for (int i = 0; i < 16; i++) o[i][0] = o[i][1] = o[i][2] = o[i][3] = v[i];
    }
    // Unity 2022.3 (Temple Run) writes smaller pixels when the texture allows: ETC RGB as RGB565,
    // ETC2 RGB A1 as RGBA4444, an EAC channel as one byte
    if (bpp == 2 && (k == K_RGB || k == K_A1)) {
        for (int r = 0; r < ch; r++)
            for (int c = 0; c < cw; c++) {
                const u8 *s = o[r * cw + c];
                const unsigned px = k == K_RGB ? ((unsigned(s[0]) << 8) & 0xf800u) | (unsigned(s[1] >> 2) << 5) | (s[2] >> 3)
                                               : ((unsigned(s[0]) << 8) & 0xf000u) | ((unsigned(s[1]) << 4) & 0x0f00u) |
                                                     (s[2] & 0xf0u) | (s[3] >> 4);
                u8 *d = dst + r * pitch + c * 2;
                d[0] = u8(px), d[1] = u8(px >> 8);
            }
        return;
    }
    if (bpp == 1 && (k == K_EAC_A || k == K_EAC_R)) {
        for (int r = 0; r < ch; r++)
            for (int c = 0; c < cw; c++) dst[r * pitch + c] = o[r * cw + c][0];
        return;
    }
    if (bpp != 4) return; // (no such form: the check sees it differ)
    for (int r = 0; r < ch; r++)
        for (int c = 0; c < cw; c++) {
            const u8 *s = o[r * cw + c];
            u8 *d = dst + r * pitch + c * bpp;
            switch (k) {
            case K_RGBA: d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255; break;
            case K_RGB: d[0] = s[0], d[1] = s[1], d[2] = s[2]; break;
            case K_A1: d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = s[3]; break;
            case K_EAC_A: d[3] = s[3]; break;
            case K_EAC_R: d[0] = s[0], d[1] = 0, d[2] = 0, d[3] = 255; break;
            case K_EAC_G: d[1] = s[1]; break;
            default: break;
            }
        }
}

constexpr int MAX_LEAVES = 8;
struct Leaf {
    u64 at;       // libunity's leaf
    u32 orig[4];  // its first 4 instructions
    Kind kind;
    int bpp_ok;   // the pixel sizes (its last argument, bytes) it gave the same bytes for: bit 2, bit 4
};
Leaf leaves[MAX_LEAVES];
int leaf_count;
alignas(16) u32 tramp[MAX_LEAVES][8]; // Unity's leaf, unpatched: its first 4 instructions, then leaf+16
u64 blocks_done;

template <int I>
u64 native_leaf(u64 b, u64 dst, u64 pitch, u64 bw, u64 bh, u64 cw, u64 ch, u64 bpp) {
    const u64 bytes = bpp;
    if (bw != 4 || bh != 4 || cw > 4 || ch > 4 || bytes > 4 || !(leaves[I].bpp_ok & (1 << bytes))) {
        // (a call the check didn't cover: Unity's own code)
        const u64 a[8] = { b, dst, pitch, bw, bh, cw, ch, bpp };
        int ok = 0;
        arm_call(reinterpret_cast<u64>(tramp[I]), a, 8, &ok);
        return 0;
    }
    leaf(leaves[I].kind, b, reinterpret_cast<u8 *>(dst), int(pitch), int(cw), int(ch), int(bytes));
    const u64 n = __atomic_add_fetch(&blocks_done, 1, __ATOMIC_RELAXED);
    if (n == 1 || n == 100000 || n == 1000000 || n == 10000000)
        droid_log("etc: %llu block(s) decoded natively (last: %s)", (unsigned long long)n, kind_name[leaves[I].kind]);
    return 0;
}
using LeafFn = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
const LeafFn leaf_fns[MAX_LEAVES] = { native_leaf<0>, native_leaf<1>, native_leaf<2>, native_leaf<3>,
                                      native_leaf<4>, native_leaf<5>, native_leaf<6>, native_leaf<7> };

// an instruction that means something else somewhere else (can't go in the trampoline)
bool pc_relative(u32 w) {
    return (w & 0x1f000000u) == 0x10000000u    // adr, adrp
           || (w & 0x7c000000u) == 0x14000000u // b, bl
           || (w & 0xff000010u) == 0x54000000u // b.cond
           || (w & 0x7e000000u) == 0x34000000u // cbz, cbnz
           || (w & 0x7e000000u) == 0x36000000u // tbz, tbnz
           || (w & 0x3b000000u) == 0x18000000u // ldr (literal)
           || (w & 0xfffffc1fu) == 0xd65f0000u; // ret (a leaf shorter than 4 instructions)
}

// adr xN, target (with or without the nop the linker left of an adrp)
bool adr_target(u64 at, u32 w, u64 *out) {
    if ((w & 0x9f000000u) != 0x10000000u) return false;
    std::int64_t imm = std::int64_t(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3));
    if (imm & (1 << 20)) imm -= 1 << 21;
    *out = at + u64(imm);
    return true;
}

// adrp xN, page
static bool adrp_page(u64 at, u32 w, u64 *out) {
    if ((w & 0x9f000000u) != 0x90000000u) return false;
    std::int64_t imm = std::int64_t(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3));
    if (imm & (1 << 20)) imm -= 1 << 21;
    *out = (at & ~u64(0xfff)) + u64(imm << 12);
    return true;
}

// One of Unity's five texture functions at word i: its four function pointers (x0..x3: fetch,
// decode, fetch, decode) and the "bl <block loop>". Two builds of it are known:
//  A (Crossy Road, Shadow Fight 4): sub sp; str x30; mov x9, x1; mov w8, w4; mov w7, w3; mov w6, w2;
//    mov x4, x0; adr x0..x3 (nops between); str w5, [sp, #8]; mov x5, x9; str w8, [sp]; bl
//  B (Unity 2022.3: Temple Run 1 and 2, Harbor Master): sub sp; str x30; mov w7, w3; mov w6, w2;
//    mov x8, x1; mov x9, x0; adrp x0..x3; str w5, [sp, #8]; add x0..x3; str w4, [sp]; mov x4, x9;
//    mov x5, x8; bl - the same arguments to the loop, in another order
static bool match_dispatcher(const u32 *w, std::size_t i, u64 base, u64 fn[4], std::size_t *blk) {
    static const u32 headA[7] = { 0xd10083ffu, 0xf9000bfeu, 0xaa0103e9u, 0x2a0403e8u, 0x2a0303e7u, 0x2a0203e6u, 0xaa0003e4u };
    static const u32 headB[6] = { 0xd10083ffu, 0xf9000bfeu, 0x2a0303e7u, 0x2a0203e6u, 0xaa0103e8u, 0xaa0003e9u };
    if (w[i] != 0xd10083ffu) return false;
    if (!std::memcmp(&w[i], headA, sizeof headA)) {
        int got = 0;
        std::size_t k = i + 7;
        for (; k < i + 16 && got < 4; k++) {
            if (w[k] == 0xd503201fu) continue; // nop
            u64 t;
            if (!adr_target(base + k * 4, w[k], &t) || (w[k] & 31) != u32(got)) break;
            fn[got++] = t;
        }
        if (got != 4 || w[k] != 0xb9000be5u || w[k + 1] != 0xaa0903e5u || w[k + 2] != 0xb90003e8u ||
            (w[k + 3] & 0xfc000000u) != 0x94000000u)
            return false;
        *blk = k + 3;
        return true;
    }
    if (!std::memcmp(&w[i], headB, sizeof headB)) {
        const std::size_t k = i + 6;
        u64 page[4];
        for (int r = 0; r < 4; r++)
            if (!adrp_page(base + (k + r) * 4, w[k + r], &page[r]) || (w[k + r] & 31) != u32(r)) return false;
        if (w[k + 4] != 0xb9000be5u) return false; // str w5, [sp, #8]
        for (int r = 0; r < 4; r++) {
            const u32 a = w[k + 5 + r]; // add xr, xr, #lo
            if ((a & 0xffc00000u) != 0x91000000u || (a & 31) != u32(r) || ((a >> 5) & 31) != u32(r)) return false;
            fn[r] = page[r] + ((a >> 10) & 0xfff);
        }
        if (w[k + 9] != 0xb90003e4u || w[k + 10] != 0xaa0903e4u || w[k + 11] != 0xaa0803e5u ||
            (w[k + 12] & 0xfc000000u) != 0x94000000u)
            return false;
        *blk = k + 12;
        return true;
    }
    return false;
}

u64 rnd_state = 0x9e3779b97f4a7c15ull;
u64 rnd() {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

// a block of every kind of mode: random, then T, H and planar for sure (bit 33 set, a channel overflowing)
u64 test_block(int i) {
    u64 b = rnd();
    const int mode = i & 3;
    if (mode == 0) return b;
    b |= u64(1) << 33;
    if (mode == 1) b = (b & ~(u64(0xff) << 56)) | (u64(31) << 59) | (u64(3) << 56); // R overflows: T
    if (mode == 2) {
        b = (b & ~(u64(0xff) << 56)) | (u64(rnd() & 15) << 59);                    // R + 0 fits
        b = (b & ~(u64(0xff) << 48)) | (u64(31) << 51) | (u64(3) << 48);            // G overflows: H
    }
    if (mode == 3) {
        b = (b & ~(u64(0xff) << 56)) | (u64(rnd() & 15) << 59);
        b = (b & ~(u64(0xff) << 48)) | (u64(rnd() & 15) << 51);
        b = (b & ~(u64(0xff) << 40)) | (u64(31) << 43) | (u64(3) << 40);            // B overflows: planar
    }
    return b;
}

// Unity's leaf (through its trampoline) against our kind k: the same bytes everywhere?
bool same_as_unity(int li, Kind k, int *checked, int bpp) {
    constexpr int PITCH = 32, ROWS = 6;
    static const int clips[][2] = { { 4, 4 }, { 4, 4 }, { 4, 4 }, { 1, 1 }, { 2, 2 }, { 2, 4 }, { 4, 2 }, { 3, 1 }, { 1, 3 } };
    alignas(16) u8 a[PITCH * ROWS], n[PITCH * ROWS];
    rnd_state = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 1800; i++) {
        const u64 b = test_block(i);
        const int *cl = clips[i % 9];
        for (int j = 0; j < PITCH * ROWS; j++) a[j] = n[j] = u8(rnd());
        const u64 args[8] = { b, reinterpret_cast<u64>(a + PITCH + 4), PITCH, 4, 4, u64(cl[0]), u64(cl[1]), u64(bpp) };
        int ok = 0;
        arm_call(reinterpret_cast<u64>(tramp[li]), args, 8, &ok);
        if (!ok) return false;
        leaf(k, b, n + PITCH + 4, PITCH, cl[0], cl[1], bpp);
        if (std::memcmp(a, n, sizeof a) != 0) {
            *checked = i;
            static const bool debug = std::getenv("DROID_ETC_DEBUG") != nullptr;
            if (debug && k <= K_A1) {
                char line[400];
                int len = std::snprintf(line, sizeof line, "etc: leaf %d as %s, block %d %016llx (clip %dx%d): unity/ours", li,
                                        kind_name[k], i, (unsigned long long)b, cl[0], cl[1]);
                for (int j = 0; j < PITCH * ROWS && len < 360; j++)
                    if (a[j] != n[j]) len += std::snprintf(line + len, sizeof line - len, " @%d %02x/%02x", j, a[j], n[j]);
                droid_log("%s", line);
            }
            return false;
        }
    }
    *checked = 1800;
    return true;
}

void patch(u64 at, const u32 code[4]) {
    std::memcpy(reinterpret_cast<void *>(at), code, 16);
    __builtin___clear_cache(reinterpret_cast<char *>(at), reinterpret_cast<char *>(at + 16));
}

// ---- the whole texture: Unity's five functions ------------------------------------------------
// With only the leaves ours, each 4x4 block still went through Unity's block loop and two byte-by-
// byte fetches in the translator, and two calls out of ARM code (Cut the Rope: 17% of its samples
// in those calls on entering a level). Here the function itself:
//   f(dst, src, width, height, dstW, dstH): blocks ceil(w/4) x ceil(h/4) of 8 bytes (one leaf) or
//   16 (two: the first 8 bytes to the first leaf, then the second); a pixel 4 bytes, dst rows
//   dstW * 4 apart. dst smaller than the image (dstW < w or dstH < h): 2x2 of each block (the top
//   left) at 8 bytes a block. Clipped to dstW x dstH unless w and h are both multiples of 4
//   (Unity's loop: its aligned paths copy whole 4x4 / 2x2 pieces).
struct Disp {
    u64 at;
    u32 orig[4];
    u64 leaf1, leaf2; // Unity's decode functions (x1, x3)
    int k1, k2;       // ours for them (k2 -1: one 8-byte block)
};
constexpr int MAX_DISPS = 8;
Disp disps[MAX_DISPS];
int disp_count;
alignas(16) u32 dtramp[MAX_DISPS][8];
u64 images_done;

inline u64 be64(const u8 *p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

void image(const Disp &d, u8 *dst, const u8 *src, int w, int h, int dw, int dh) {
    if (w <= 0 || h <= 0) return;
    const int step = dw < w || dh < h ? 2 : 4;
    const bool aligned = ((w | h) & 3) == 0;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, pitch = dw * 4, bpb = d.k2 < 0 ? 8 : 16;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            int cw = step, ch = step;
            if (!aligned) {
                cw = dw - bx * step < step ? dw - bx * step : step;
                ch = dh - by * step < step ? dh - by * step : step;
            }
            u8 *o = dst + std::ptrdiff_t(by) * pitch * step + std::ptrdiff_t(bx) * step * 4;
            const u8 *s = src + (std::size_t(by) * bw + bx) * bpb;
            leaf(Kind(d.k1), be64(s), o, pitch, cw, ch, 4);
            if (d.k2 >= 0) leaf(Kind(d.k2), be64(s + 8), o, pitch, cw, ch, 4);
        }
}

template <int I>
u64 native_image(u64 dst, u64 src, u64 w, u64 h, u64 dw, u64 dh, u64, u64) {
    image(disps[I], reinterpret_cast<u8 *>(dst), reinterpret_cast<const u8 *>(src), int(std::int32_t(w)),
          int(std::int32_t(h)), int(std::int32_t(dw)), int(std::int32_t(dh)));
    const u64 n = __atomic_add_fetch(&images_done, 1, __ATOMIC_RELAXED);
    if (n == 1 || n == 100 || n == 1000 || n == 10000)
        droid_log("etc: %llu texture(s) decoded natively (last %dx%d)", (unsigned long long)n, int(std::int32_t(w)),
                  int(std::int32_t(h)));
    return 0;
}
const LeafFn image_fns[MAX_DISPS] = { native_image<0>, native_image<1>, native_image<2>, native_image<3>,
                                      native_image<4>, native_image<5>, native_image<6>, native_image<7> };

// Unity's function (through its trampoline) against ours on small textures of every shape
bool image_same_as_unity(int di, int *failed_case) {
    static const int cases[][4] = { { 4, 4, 4, 4 },   { 8, 8, 8, 8 },   { 16, 8, 16, 8 }, { 32, 32, 32, 32 },
                                    { 5, 3, 5, 3 },   { 13, 7, 13, 7 }, { 1, 1, 1, 1 },   { 2, 2, 2, 2 },
                                    { 3, 6, 3, 6 },   { 8, 8, 4, 4 },   { 16, 16, 8, 8 }, { 6, 6, 3, 3 },
                                    { 12, 4, 6, 2 },  { 7, 5, 4, 3 },   { 8, 4, 8, 2 },   { 9, 9, 9, 9 },
                                    { 4, 8, 2, 4 } };
    constexpr std::size_t BUF = 16384, AT = 1024;
    static u8 src[4096], a[BUF], n[BUF];
    rnd_state = 0x2545f4914f6cdd1dull;
    for (std::size_t c = 0; c < sizeof cases / sizeof cases[0]; c++)
        for (int rep = 0; rep < 3; rep++) {
            const int *t = cases[c];
            for (std::size_t j = 0; j < sizeof src; j++) src[j] = u8(rnd());
            for (int j = 0; j < 64; j++) { // a few T / H / planar blocks among them
                u64 b = test_block(j);
                std::memcpy(src + (rnd() % (sizeof src / 8)) * 8, &b, 8);
            }
            for (std::size_t j = 0; j < BUF; j++) a[j] = n[j] = u8(rnd());
            const u64 args[6] = { reinterpret_cast<u64>(a + AT), reinterpret_cast<u64>(src), u64(t[0]), u64(t[1]),
                                  u64(t[2]), u64(t[3]) };
            int ok = 0;
            arm_call(reinterpret_cast<u64>(dtramp[di]), args, 6, &ok);
            image(disps[di], n + AT, src, t[0], t[1], t[2], t[3]);
            if (!ok || std::memcmp(a, n, BUF) != 0) {
                *failed_case = int(c);
                return false;
            }
        }
    return true;
}

} // namespace

extern "C" void arm_code_changed(uint64_t addr, uint64_t len);
extern "C" int droid_game_in(const char *list); // game_settings.c

// a library loaded, its constructors not run yet: libunity.so's ETC decoders get ours
extern "C" void etc_native_attach(const char *lib, std::uint64_t base, std::size_t size) {
    if (std::strcmp(lib, "libunity.so") != 0) return;
    // for the games it was tried with (tools.txt "EtcNative": any game); every other one decodes as
    // it did. "NoEtcNative": off. (Cut the Rope, com.zeptolab.: the game this was built for, 7 -> 39-56
    // fps at level entry; left out when the list was made, its textures slow again)
    if (droid_tool("NoEtcNative") ||
        !(droid_game_in("com.imangi.,com.yodo1.crossyroad,com.kiloo.subwaysurf,com.zeptolab.") || droid_tool("EtcNative"))) {
        droid_log("etc: Unity decodes ETC itself");
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto *w = reinterpret_cast<const u32 *>(base);
    const std::size_t nw = size / 4;
    int dispatchers = 0;
    u64 loop = 0;
    for (std::size_t i = 0; i + 24 < nw && leaf_count < MAX_LEAVES; i++) {
        u64 fn[4];
        std::size_t blk = 0; // the "bl <block loop>"
        if (!match_dispatcher(w, i, base, fn, &blk)) continue;
        const std::size_t k = blk - 3; // (as the first layout counts it)
        const u64 target = base + (k + 3) * 4 + u64(std::int64_t(std::int32_t(w[k + 3] << 6) >> 6) * 4);
        if (loop && target != loop) continue; // not the ETC loop
        loop = target;
        dispatchers++;
        if (disp_count < MAX_DISPS && !pc_relative(w[i]) && !pc_relative(w[i + 1]) && !pc_relative(w[i + 2]) &&
            !pc_relative(w[i + 3])) {
            Disp &D = disps[disp_count++];
            D.at = base + i * 4;
            std::memcpy(D.orig, &w[i], 16);
            D.leaf1 = fn[1];
            D.leaf2 = fn[3];
            D.k1 = D.k2 = -2;
        }
        for (int d = 1; d <= 3; d += 2) { // the decode functions (x1, x3)
            const u64 at = fn[d];
            if (at < base || at + 16 > base + size) continue;
            const u32 *lw = reinterpret_cast<const u32 *>(at);
            if ((lw[0] & 0xffc003ffu) != 0xd10003ffu) continue; // not "sub sp, sp, #n": the no-op one
            bool seen = false;
            for (int l = 0; l < leaf_count; l++) seen |= leaves[l].at == at;
            if (seen || leaf_count == MAX_LEAVES) continue;
            if (pc_relative(lw[0]) || pc_relative(lw[1]) || pc_relative(lw[2]) || pc_relative(lw[3])) continue;
            Leaf &L = leaves[leaf_count];
            L.at = at;
            std::memcpy(L.orig, lw, 16);
            L.kind = K_COUNT;
            L.bpp_ok = 0;
            leaf_count++;
        }
    }
    if (dispatchers < 3 || !leaf_count) {
        droid_log("etc: libunity.so's ETC decoder not found (%d of its functions): Unity decodes ETC itself", dispatchers);
        return;
    }
    // each leaf: its trampoline, our thunk in its place, then the check; no match: put back
    int ours = 0;
    for (int l = 0; l < leaf_count; l++) {
        Leaf &L = leaves[l];
        std::memcpy(tramp[l], L.orig, 16);
        tramp[l][4] = 0x58000050u; // ldr x16, #8
        tramp[l][5] = 0xd61f0200u; // br x16
        const u64 back = L.at + 16;
        std::memcpy(&tramp[l][6], &back, 8);
        char name[32];
        std::snprintf(name, sizeof name, "etc_native_leaf%d", l);
        const u64 thunk = arm_thunk(name, reinterpret_cast<void *>(leaf_fns[l]));
        if (!thunk) continue;
        mprotect(reinterpret_cast<void *>(L.at & ~u64(0xfff)), 0x2000, 7);
        u32 code[4] = { 0x58000050u, 0xd61f0200u, 0, 0 }; // ldr x16, #8; br x16; <thunk>
        std::memcpy(&code[2], &thunk, 8);
        int best_at = -1;
        for (int k = 0; k < K_COUNT && L.kind == K_COUNT; k++) { // 4 bytes a pixel, and RGB565's 2
            int mask = 0;
            for (int bpp = 1; bpp <= 4; bpp *= 2) {
                int at = 0;
                if (same_as_unity(l, Kind(k), &at, bpp)) mask |= 1 << bpp;
                if (at > best_at) best_at = at;
            }
            if (mask) {
                L.kind = Kind(k);
                L.bpp_ok = mask;
                break;
            }
        }
        if (L.kind == K_COUNT) {
            droid_log("etc: libunity.so+%#llx: none of ours gives its bytes (best: %d of 1800 blocks): Unity's kept",
                      (unsigned long long)(L.at - base), best_at);
            continue;
        }
        patch(L.at, code);
        arm_code_changed(L.at, 16);
        ours++;
        droid_log("etc: libunity.so+%#llx (%s%s) decoded natively (1800 test blocks: same bytes)",
                  (unsigned long long)(L.at - base), kind_name[L.kind],
                  L.bpp_ok & (1 << 2) ? ", 2-byte pixels" : L.bpp_ok & (1 << 1) ? ", 1-byte pixels" : "");
    }
    // then each of Unity's five texture functions whose leaves are ours: the whole texture
    int whole = 0;
    for (int di = 0; di < disp_count; di++) {
        Disp &D = disps[di];
        for (int l = 0; l < leaf_count; l++) {
            if (leaves[l].kind == K_COUNT) continue;
            if (leaves[l].at == D.leaf1) D.k1 = leaves[l].kind;
            if (leaves[l].at == D.leaf2) D.k2 = leaves[l].kind;
        }
        const auto *l2 = reinterpret_cast<const u32 *>(D.leaf2);
        if (D.k2 == -2 && D.leaf2 >= base && D.leaf2 + 4 <= base + size && (l2[0] & 0xffc003ffu) != 0xd10003ffu)
            D.k2 = -1; // the no-op second decoder: one 8-byte block
        if (D.k1 < 0 || D.k2 == -2) continue; // a leaf that stayed Unity's
        std::memcpy(dtramp[di], D.orig, 16);
        dtramp[di][4] = 0x58000050u; // ldr x16, #8
        dtramp[di][5] = 0xd61f0200u; // br x16
        const u64 back = D.at + 16;
        std::memcpy(&dtramp[di][6], &back, 8);
        int failed = -1;
        if (!image_same_as_unity(di, &failed)) {
            droid_log("etc: libunity.so+%#llx: our whole-texture loop differs (test %d): its loop kept",
                      (unsigned long long)(D.at - base), failed);
            continue;
        }
        char name[32];
        std::snprintf(name, sizeof name, "etc_native_image%d", di);
        const u64 thunk = arm_thunk(name, reinterpret_cast<void *>(image_fns[di]));
        if (!thunk) continue;
        mprotect(reinterpret_cast<void *>(D.at & ~u64(0xfff)), 0x2000, 7);
        u32 code[4] = { 0x58000050u, 0xd61f0200u, 0, 0 };
        std::memcpy(&code[2], &thunk, 8);
        patch(D.at, code);
        arm_code_changed(D.at, 16);
        whole++;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    droid_log("etc: %d of %d ETC/EAC block decoders native, %d of %d texture functions whole (check %.0f ms)", ours,
              leaf_count, whole, dispatchers, ms);
}
