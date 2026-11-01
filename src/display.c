// display.c — the PS5 screen: two 1920x1080 tiled framebuffers in direct
// memory, flipped with VideoOut.

#include "display.h"

#include <string.h>

int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
void sceVideoOutSetBufferAttribute2(void *attr, uint64_t fmt, uint32_t tiling, uint32_t w,
                                    uint32_t h, uint64_t r1, uint32_t r2, uint64_t r3);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start, void *bufs,
                                int32_t count, void *attr, int32_t cat, void *opt);
int sceVideoOutSubmitFlip(int32_t handle, int32_t idx, uint32_t mode, int64_t arg);
int sceVideoOutWaitVblank(int32_t handle);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align,
                                      int type, int64_t *phys);
int32_t sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys,
                                 size_t align);
int sceKernelUsleep(uint32_t us);

#define FRAME_BYTES 0x1000000
#define POOL_BYTES (FRAME_BYTES * 2)
#define POOL_ALIGN 0x200000
#define MEM_TYPE_WC 3
#define MAP_PROT 0x33
#define PIX_FMT UINT64_C(0x8000000022000000)

struct ps5_vbuf { void *data; void *meta; void *r0; void *r1; };

static int video = -1;
static struct ps5_vbuf vbufs[2];

int display_open(int *busy_tries) {
    int tries = 0;
    for (; tries < 150; tries++) {
        video = sceVideoOutOpen(0xff, 0, 0, NULL);
        if (video != (int)0x80290009) break;
        sceKernelUsleep(100000);
    }
    if (busy_tries) *busy_tries = tries;
    return video;
}

int display_setup(void) {
    int64_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), POOL_BYTES,
                                           POOL_ALIGN, MEM_TYPE_WC, &phys);
    if (rc < 0) return rc;
    void *mapped = NULL;
    rc = sceKernelMapDirectMemory(&mapped, POOL_BYTES, MAP_PROT, 0, phys, POOL_ALIGN);
    if (rc < 0) return rc;
    vbufs[0].data = mapped;
    vbufs[1].data = (uint8_t *)mapped + FRAME_BYTES;
    uint8_t attr[80];
    memset(attr, 0, sizeof(attr));
    sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(attr, PIX_FMT, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0, 0);
    return sceVideoOutRegisterBuffers2(video, 0, 0, vbufs, 2, attr, 0, NULL);
}

// PS5 64KB tile swizzle (from prx-foundation demo_renderer)
uint64_t display_tile_offset(uint32_t x, uint32_t y) {
    uint32_t off = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^
                   ((y << 8) & 0x4000u) ^ ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^
                   ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
    uint32_t bpr = (DISPLAY_WIDTH + 127u) >> 7;
    uint32_t blk = (y >> 7) * bpr + (x >> 7);
    return ((uint64_t)blk << 16) + off;
}

uint32_t *display_buffer(int index) {
    return vbufs[index & 1].data;
}

void display_fill(int index, uint32_t color) {
    // Tiling doesn't matter for one colour: every byte of the frame is a pixel.
    uint32_t *p = vbufs[index & 1].data;
    for (size_t i = 0; i < FRAME_BYTES / 4; i++) p[i] = color;
}

void display_flip(int index, int64_t arg) {
    uint8_t *p = vbufs[index & 1].data;
    for (uint8_t *end = p + FRAME_BYTES; p < end; p += 64)
        __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
    __asm__ volatile("mfence" ::: "memory");
    sceVideoOutSubmitFlip(video, index & 1, 1, arg);
    sceVideoOutWaitVblank(video);
}
