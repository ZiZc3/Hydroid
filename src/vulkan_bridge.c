// vulkan_bridge.c — Android's libvulkan.so for the game's ARM code, on the host's Vulkan
// (PS5: RADV from PS5_Vulkan, linked in; PC test runner: the system's loader).
//
// Vulkan's structs have one layout on arm64 and x86-64 Linux (LP64), and every
// command takes integers, pointers or floats, so each command reaches the host
// through the ordinary import thunk. What is Android's is done here:
//   - the window: VK_KHR_android_surface becomes the platform's surface
//     (vk_platform_*: VK_KHR_display on the PS5, an X11 window on the PC);
//   - pAllocator: ARM callbacks the host can't call, so it reaches the host as NULL;
//   - debug messengers / layers: hidden (their callbacks are ARM code too);
//   - vkGet*ProcAddr: ARM thunks for the host's commands.
// On unless the game's settings say GLES: the game then falls back to GLES.

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "game_settings.h"
#include "mapper.h"
#include "android_shim.h"
#include "arm_cpu.h"
#include "overlay_art.h"
#include "overlay_stats.h"
#include "quick_menu.h"
#include "game_file.h"
#include "vulkan_bridge.h"

static PFN_vkGetInstanceProcAddr host_gipa;
static PFN_vkGetDeviceProcAddr host_gdpa;
static VkInstance the_instance;
static VkPhysicalDevice the_pd; // the device made (the scaler's memory types)
static void fill_pending(void);

int vk_bridge_enabled(void) {
    static int on = -1;
    if (on < 0) {
        // the game's settings: auto and Vulkan on (GLES when asked; DROID_GLES=1 on the PC)
        on = !renderer_gles() && !getenv("DROID_GLES");
        if (on) {
            // (no RADV_DEBUG=info: its GPU report calls readlink, which a title doesn't have)
            host_gipa = vk_platform_gipa();
            if (!host_gipa) on = 0;
        }
        droid_log("vulkan: %s", on ? "on (the game's Vulkan renderer runs on the host's Vulkan)"
                                   : "off (the game's settings: GLES): the game falls back to GLES");
    }
    return on;
}

// ---- pAllocator: which argument it is, by the command's name -------------------------

static int allocator_arg(const char *n) {
    if (!strcmp(n, "vkCreateInstance") || !strcmp(n, "vkDestroyInstance") || !strcmp(n, "vkDestroyDevice"))
        return 1;
    if (!strcmp(n, "vkCreateGraphicsPipelines") || !strcmp(n, "vkCreateComputePipelines")) return 4;
    if (!strcmp(n, "vkCreateSharedSwapchainsKHR")) return 3;
    if (!strcmp(n, "vkCreateRayTracingPipelinesKHR")) return 5;
    if (!strcmp(n, "vkCreateRayTracingPipelinesNV")) return 4;
    if (!strncmp(n, "vkCreate", 8) || !strncmp(n, "vkDestroy", 9) || !strcmp(n, "vkAllocateMemory") ||
        !strcmp(n, "vkFreeMemory") || !strcmp(n, "vkRegisterDeviceEventEXT") ||
        !strcmp(n, "vkRegisterDisplayEventEXT"))
        return 2;
    return -1;
}

// ---- the commands done here -----------------------------------------------------------

static int is_platform_surface_ext(const char *e) {
    return !strcmp(e, "VK_KHR_xlib_surface") || !strcmp(e, "VK_KHR_xcb_surface") ||
           !strcmp(e, "VK_KHR_wayland_surface") || !strcmp(e, "VK_KHR_display") ||
           !strcmp(e, "VK_EXT_direct_mode_display") || !strcmp(e, "VK_EXT_acquire_xlib_display") ||
           !strcmp(e, "VK_EXT_acquire_drm_display") || !strcmp(e, "VK_KHR_get_display_properties2") ||
           !strcmp(e, "VK_EXT_display_surface_counter") || !strcmp(e, "VK_EXT_headless_surface");
}
static int is_debug_ext(const char *e) {
    return !strcmp(e, "VK_EXT_debug_report") || !strcmp(e, "VK_EXT_debug_utils") ||
           !strcmp(e, "VK_EXT_debug_marker") || !strcmp(e, "VK_EXT_validation_features") ||
           !strcmp(e, "VK_EXT_layer_settings");
}

// The host's instance extensions as Android's: its window ones out, android_surface in.
static VkResult VKAPI_CALL b_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                    VkExtensionProperties *out) {
    if (layer) {
        *count = 0;
        return VK_SUCCESS;
    }
    PFN_vkEnumerateInstanceExtensionProperties f =
        (PFN_vkEnumerateInstanceExtensionProperties)host_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    f(NULL, &n, NULL);
    VkExtensionProperties *all = calloc(n + 1, sizeof(*all));
    f(NULL, &n, all);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!is_platform_surface_ext(all[i].extensionName) && !is_debug_ext(all[i].extensionName)) all[k++] = all[i];
    snprintf(all[k].extensionName, sizeof(all[k].extensionName), "VK_KHR_android_surface");
    all[k++].specVersion = 6;
    VkResult r = VK_SUCCESS;
    if (!out) {
        *count = k;
    } else {
        uint32_t give = *count < k ? *count : k;
        memcpy(out, all, give * sizeof(*out));
        *count = give;
        if (give < k) r = VK_INCOMPLETE;
    }
    free(all);
    return r;
}

static VkResult VKAPI_CALL b_vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *out) {
    (void)out;
    *count = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL b_vkEnumerateDeviceLayerProperties(VkPhysicalDevice pd, uint32_t *count,
                                                              VkLayerProperties *out) {
    (void)pd; (void)out;
    *count = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL b_vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc,
                                              VkInstance *instance) {
    (void)alloc;
    const char *exts[64];
    uint32_t n = 0;
    int surface = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount && n < 56; i++) {
        const char *e = info->ppEnabledExtensionNames[i];
        if (!strcmp(e, "VK_KHR_android_surface")) {
            surface = 1;
            continue;
        }
        if (is_debug_ext(e)) continue;
        exts[n++] = e;
    }
    if (surface) n += (uint32_t)vk_platform_instance_extensions(exts + n, 8);
    VkInstanceCreateInfo mine = *info;
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;
    mine.enabledExtensionCount = n;
    mine.ppEnabledExtensionNames = exts;
    mine.pNext = NULL; // debug messengers and validation settings only, from a game
    uint32_t api = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    PFN_vkCreateInstance f = (PFN_vkCreateInstance)host_gipa(NULL, "vkCreateInstance");
    VkResult r = f(&mine, NULL, instance);
    char list[512] = "";
    int at = 0;
    for (uint32_t i = 0; i < n && at < 480; i++) at += snprintf(list + at, sizeof(list) - at, " %s", exts[i]);
    droid_log("vulkan: vkCreateInstance(api %u.%u) -> %d, extensions:%s", VK_API_VERSION_MAJOR(api),
              VK_API_VERSION_MINOR(api), r, list);
    if (r == VK_SUCCESS) {
        the_instance = *instance;
        if (!host_gdpa) host_gdpa = (PFN_vkGetDeviceProcAddr)host_gipa(*instance, "vkGetDeviceProcAddr");
        fill_pending();
    }
    return r;
}

static VkResult VKAPI_CALL b_vkCreateAndroidSurfaceKHR(VkInstance instance, const void *info,
                                                       const VkAllocationCallbacks *alloc, VkSurfaceKHR *surface) {
    (void)info; (void)alloc;
    VkResult r = vk_platform_create_surface(instance, host_gipa, surface);
    droid_log("vulkan: the game's window -> the %s surface: %d", vk_platform_name(), r);
    return r;
}

static VkResult VKAPI_CALL b_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *info,
                                            const VkAllocationCallbacks *alloc, VkDevice *device) {
    (void)alloc;
    VkDeviceCreateInfo mine = *info;
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;
    PFN_vkCreateDevice f = (PFN_vkCreateDevice)host_gipa(the_instance, "vkCreateDevice");
    VkResult r = f(pd, &mine, NULL, device);
    if (r == VK_SUCCESS) the_pd = pd; // the scaler's memory types
    char list[768] = "";
    int at = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount && at < 740; i++)
        at += snprintf(list + at, sizeof(list) - at, " %s", info->ppEnabledExtensionNames[i]);
    droid_log("vulkan: vkCreateDevice -> %d, extensions:%s", r, list);
    return r;
}

// Debug messengers: made of nothing (the extension is hidden, but a game may still ask)
static VkResult VKAPI_CALL b_vkCreateDebugUtilsMessengerEXT(VkInstance i, const void *info, const void *alloc,
                                                            uint64_t *out) {
    (void)i; (void)info; (void)alloc;
    *out = 0x5eed;
    return VK_SUCCESS;
}
static void VKAPI_CALL b_vkDestroyDebugUtilsMessengerEXT(VkInstance i, uint64_t m, const void *alloc) {
    (void)i; (void)m; (void)alloc;
}

// The device as the game sees it: a software one (the PC's lavapipe) shown as an
// integrated GPU - Unity drops a CPU device and falls back to GLES.
static void show_device(VkPhysicalDeviceProperties *p) {
    static int logged;
    if (logged++ < 2)
        droid_log("vulkan: device \"%s\" (type %d, vendor %#x, api %u.%u.%u)", p->deviceName, p->deviceType,
                  p->vendorID, VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion),
                  VK_API_VERSION_PATCH(p->apiVersion));
    if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}
static void VKAPI_CALL b_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    ((PFN_vkGetPhysicalDeviceProperties)host_gipa(the_instance, "vkGetPhysicalDeviceProperties"))(pd, p);
    show_device(p);
}
static void VKAPI_CALL b_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    PFN_vkGetPhysicalDeviceProperties2 f =
        (PFN_vkGetPhysicalDeviceProperties2)host_gipa(the_instance, "vkGetPhysicalDeviceProperties2");
    if (!f) f = (PFN_vkGetPhysicalDeviceProperties2)host_gipa(the_instance, "vkGetPhysicalDeviceProperties2KHR");
    f(pd, p);
    show_device(&p->properties);
}

static VkResult VKAPI_CALL b_vkEnumeratePhysicalDevices(VkInstance i, uint32_t *count, VkPhysicalDevice *out) {
    PFN_vkEnumeratePhysicalDevices f = (PFN_vkEnumeratePhysicalDevices)host_gipa(i, "vkEnumeratePhysicalDevices");
    VkResult r = f(i, count, out);
    static int logged;
    if (logged++ < 6) droid_log("vulkan: vkEnumeratePhysicalDevices -> %d, %u device(s)%s", r, *count, out ? "" : " (count)");
    return r;
}

// ---- the scaler: the game draws at its window's size (1920x1080), the screen may be
// bigger (the PS5's display has only 4K modes). The game's swapchain images are then
// ours at its size; at each present they're stretched onto the real swapchain's
// (vkCmdBlitImage, linear), as a phone's compositor scales a surface.
// (the game's screen: 1920x1080, or a phone's 1080x1920 in the phone view, shown in the
// middle at its full height, black at the sides)
#define GAME_W ((uint32_t)droid_game_w)
#define GAME_H ((uint32_t)droid_game_h)
#define MAX_SC_IMAGES 8

// The overlay drawn on the screen's image after the game's frame (the GL bridge's twins):
// the FPS counter and the controller's cursor, written into a host buffer and copied
// over the image (copies don't blend: the cursor's soft shadow is left out).
#define OVL_W 1600 // the panel's box, at most (overlay_stats.c: the stats 300 x 132 at 2x, a puzzle's address)
#define OVL_H 440
#define CURA_W 96 // the cursor's, at 4K
#define CURA_H 112
#define MK_W 112 // a mapper marker's block (its square and border, at 4K)
#define OVL_BYTES ((OVL_W * OVL_H + CURA_W * CURA_H + DROID_MARKERS * MK_W * MK_W) * 4)
extern float droid_cursor_x, droid_cursor_y; // unity_host.c
extern volatile int droid_cursor_shown;

typedef struct {
    VkSwapchainKHR real;
    VkDevice dev;
    uint32_t count;
    VkImage game[MAX_SC_IMAGES], screen[MAX_SC_IMAGES];
    VkDeviceMemory mem[MAX_SC_IMAGES];
    VkCommandBuffer cmd[MAX_SC_IMAGES];
    VkSemaphore done[MAX_SC_IMAGES];
    VkFence fence[MAX_SC_IMAGES];
    VkCommandPool pool;
    VkExtent2D game_size, screen_size;
    VkFormat format;
    VkBuffer ovl[MAX_SC_IMAGES];
    VkDeviceMemory ovl_mem[MAX_SC_IMAGES];
    uint8_t *ovl_map[MAX_SC_IMAGES];
} scaled_chain;
static scaled_chain chains[4];

// Where the game's picture is on the screen (all of it, or the phone view's middle), and a
// point of the game's screen there
static void pic_rect(const scaled_chain *c, int *x, int *y, int *w, int *h) {
    view_rect((int)c->screen_size.width, (int)c->screen_size.height, x, y, w, h);
    if (!phone_view()) *x = 0, *y = 0, *w = (int)c->screen_size.width, *h = (int)c->screen_size.height;
}
static void to_screen(const scaled_chain *c, float gx, float gy, int *sx, int *sy) {
    int x, y, w, h;
    pic_rect(c, &x, &y, &w, &h);
    *sx = x + (int)(gx * w / c->game_size.width);
    *sy = y + (int)(gy * h / c->game_size.height);
}

static scaled_chain *chain_of(VkSwapchainKHR s) {
    for (int i = 0; i < 4; i++)
        if (chains[i].real == s && s) return &chains[i];
    return NULL;
}

#define DEV(name) PFN_##name name = (PFN_##name)host_gdpa(dev, #name)

// The game sees the surface at its window's size.
static VkResult VKAPI_CALL b_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                                                      VkSurfaceCapabilitiesKHR *caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR f =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)host_gipa(the_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkResult r = f(pd, s, caps);
    if (r == VK_SUCCESS && (caps->currentExtent.width != GAME_W || caps->currentExtent.height != GAME_H)) {
        static int logged;
        if (!logged++)
            droid_log("vulkan: the screen is %ux%u: the game gets %ux%u, scaled up at each present",
                      caps->currentExtent.width, caps->currentExtent.height, GAME_W, GAME_H);
        caps->currentExtent = (VkExtent2D){ GAME_W, GAME_H };
        if (caps->minImageExtent.width > GAME_W) caps->minImageExtent.width = GAME_W;
        if (caps->minImageExtent.height > GAME_H) caps->minImageExtent.height = GAME_H;
        if (caps->maxImageExtent.width < GAME_W) caps->maxImageExtent.width = GAME_W;
        if (caps->maxImageExtent.height < GAME_H) caps->maxImageExtent.height = GAME_H;
        caps->supportedUsageFlags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    return r;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)host_gipa(the_instance, "vkGetPhysicalDeviceMemoryProperties"))(the_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

static void splash_start(scaled_chain *c); // a one-game app's splash, until the game's first frame
static void splash_stop(int how);
static scaled_chain *splash_chain; // the swapchain it's on

static VkResult VKAPI_CALL b_vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *info,
                                                  const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    (void)alloc;
    DEV(vkCreateSwapchainKHR);
    VkSurfaceCapabilitiesKHR caps;
    ((PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)host_gipa(the_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))(
        the_pd, info->surface, &caps);
    VkExtent2D screen = caps.currentExtent; // the same size too: the overlay goes on in the copy
    scaled_chain *c = NULL;
    for (int i = 0; i < 4 && !c; i++)
        if (!chains[i].real) c = &chains[i];
    if (!c) return vkCreateSwapchainKHR(dev, info, NULL, out);
    VkSwapchainCreateInfoKHR real = *info;
    real.imageExtent = screen;
    real.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    real.oldSwapchain = info->oldSwapchain;
    // every frame waits for a refresh (FIFO), whatever the game asks: a game's mailbox /
    // immediate mode ran at 90 fps on the 60 Hz screen, frames shown unevenly (not smooth)
    // (not on the PC runner: WSL's FIFO ran at 40 fps; its frames follow the Choreographer)
#ifndef DROID_PC
    real.presentMode = VK_PRESENT_MODE_FIFO_KHR;
#endif
    VkResult r = vkCreateSwapchainKHR(dev, &real, NULL, out);
    droid_log("vulkan: swapchain %ux%u for the game, %ux%u on the screen (format %d, the game's present mode %d -> FIFO) -> %d",
              info->imageExtent.width, info->imageExtent.height, screen.width, screen.height, info->imageFormat,
              info->presentMode, r);
    if (r != VK_SUCCESS) return r;
    DEV(vkGetSwapchainImagesKHR);
    DEV(vkCreateImage);
    DEV(vkGetImageMemoryRequirements);
    DEV(vkAllocateMemory);
    DEV(vkBindImageMemory);
    DEV(vkCreateCommandPool);
    DEV(vkAllocateCommandBuffers);
    DEV(vkCreateSemaphore);
    DEV(vkCreateFence);
    DEV(vkCreateBuffer);
    DEV(vkGetBufferMemoryRequirements);
    DEV(vkBindBufferMemory);
    DEV(vkMapMemory);
    memset(c, 0, sizeof(*c));
    c->format = info->imageFormat;
    c->real = *out;
    c->dev = dev;
    c->game_size = info->imageExtent;
    c->screen_size = screen;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, *out, &n, NULL);
    if (n > MAX_SC_IMAGES) n = MAX_SC_IMAGES;
    vkGetSwapchainImagesKHR(dev, *out, &n, c->screen);
    c->count = n;
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
    vkCreateCommandPool(dev, &pi, NULL, &c->pool);
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = n };
    vkAllocateCommandBuffers(dev, &ai, c->cmd);
    for (uint32_t i = 0; i < n; i++) {
        VkImageCreateInfo ii = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = info->imageFormat,
            .extent = { info->imageExtent.width, info->imageExtent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = info->imageArrayLayers ? info->imageArrayLayers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        vkCreateImage(dev, &ii, NULL, &c->game[i]);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, c->game[i], &req);
        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                    .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        vkAllocateMemory(dev, &ma, NULL, &c->mem[i]);
        vkBindImageMemory(dev, c->game[i], c->mem[i], 0);
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(dev, &si, NULL, &c->done[i]);
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        vkCreateFence(dev, &fi, NULL, &c->fence[i]);
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = OVL_BYTES,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        vkCreateBuffer(dev, &bi, NULL, &c->ovl[i]);
        VkMemoryRequirements breq;
        vkGetBufferMemoryRequirements(dev, c->ovl[i], &breq);
        VkMemoryAllocateInfo bm = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = breq.size,
            .memoryTypeIndex = memory_type(breq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        vkAllocateMemory(dev, &bm, NULL, &c->ovl_mem[i]);
        vkBindBufferMemory(dev, c->ovl[i], c->ovl_mem[i], 0);
        void *m = NULL;
        vkMapMemory(dev, c->ovl_mem[i], 0, OVL_BYTES, 0, &m);
        c->ovl_map[i] = m;
    }
    splash_start(c);
    return VK_SUCCESS;
}

// One pixel of the overlay in the screen's byte order (BGRA or RGBA)
static void ovl_px(const scaled_chain *c, uint8_t *d, float r, float g, float b) {
    int bgra = c->format == VK_FORMAT_B8G8R8A8_UNORM || c->format == VK_FORMAT_B8G8R8A8_SRGB;
    uint8_t R = (uint8_t)(r * 255 + 0.5f), G = (uint8_t)(g * 255 + 0.5f), B = (uint8_t)(b * 255 + 0.5f);
    d[0] = bgra ? B : R;
    d[1] = G;
    d[2] = bgra ? R : B;
    d[3] = 255;
}

// The overlay's copies, recorded after the game's frame went onto the screen image.
static void draw_overlay(scaled_chain *c, uint32_t i, VkCommandBuffer cb) {
    VkDevice dev = c->dev;
    DEV(vkCmdCopyBufferToImage);
    if (!c->ovl_map[i]) return;
    float sc = c->screen_size.height / 1080.0f; // the GL counter's size, at this screen's
    VkBufferImageCopy regions[256];
    uint32_t n = 0;
    // the stats panel (overlay_stats.c): its solid part, a copy for each run of rows with the
    // same ends (its rounded corners cut row by row: copies can't blend)
    overlay_stats_tick();
    int sw = 0, sh = 0;
    unsigned sv;
    const uint8_t *img = app_settings.stats ? overlay_stats_image(app_settings.stats, sc, 0, &sw, &sh, &sv) : NULL;
    if (img) {
        if (sw > OVL_W) sw = OVL_W;
        if (sh > OVL_H) sh = OVL_H;
        uint8_t *buf = c->ovl_map[i];
        const int at = (int)(24 * sc);
        // the stats: top left
        const int ox = at;
        const int oy = at;
        // into this image's buffer only when the picture changed (each frame cost a 4K panel's pixels)
        static unsigned buf_ver[16];
        static const void *buf_chain;
        static short row_x0[OVL_H], row_x1[OVL_H]; // each row's solid ends, for the newest picture
        static unsigned rows_ver;
        if (buf_chain != c) {
            memset(buf_ver, 0, sizeof(buf_ver));
            buf_chain = c;
        }
        if (i >= 16 || buf_ver[i] != sv) {
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    const uint8_t *q = img + (y * sw + x) * 4;
                    ovl_px(c, buf + (y * OVL_W + x) * 4, q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f);
                }
            if (i < 16) buf_ver[i] = sv;
        }
        if (rows_ver != sv) {
            for (int y = 0; y < sh; y++) {
                row_x0[y] = row_x1[y] = -1;
                for (int x = 0; x < sw; x++)
                    if (img[(y * sw + x) * 4 + 3]) { row_x0[y] = (short)x; break; }
                for (int x = sw - 1; x >= 0; x--)
                    if (img[(y * sw + x) * 4 + 3]) { row_x1[y] = (short)x; break; }
            }
            rows_ver = sv;
        }
        int run_y = 0, run_x0 = -1, run_x1 = -1;
        for (int y = 0; y <= sh && n < 200; y++) {
            int x0 = y < sh ? row_x0[y] : -1, x1 = y < sh ? row_x1[y] : -1;
            if (y < sh && x0 == run_x0 && x1 == run_x1) continue; // the run goes on
            if (run_x0 >= 0 && y > run_y)
                regions[n++] = (VkBufferImageCopy){
                    .bufferOffset = (VkDeviceSize)(run_y * OVL_W + run_x0) * 4, .bufferRowLength = OVL_W,
                    .bufferImageHeight = OVL_H, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageOffset = { ox + run_x0, oy + run_y, 0 },
                    .imageExtent = { (uint32_t)(run_x1 - run_x0 + 1), (uint32_t)(y - run_y), 1 },
                };
            run_y = y, run_x0 = x0, run_x1 = x1;
        }
    }
    // the mapper's markers: squares with a black border, copied before the rest (the
    // cursor goes over them: a second copy, copies in one call mustn't overlap)
    if (droid_mapper_on) {
        VkBufferImageCopy mk[DROID_MARKERS];
        uint32_t m = 0;
        const VkDeviceSize mk_at = (VkDeviceSize)(OVL_W * OVL_H + CURA_W * CURA_H) * 4;
        int count = droid_marker_count;
        for (int k = 0; k < count && k < DROID_MARKERS; k++) {
            const droid_marker *d = &droid_markers[k];
            int border = (int)(3 * sc), side = (int)(d->size * sc) + 2 * border;
            if (side > MK_W) side = MK_W;
            uint8_t *blk = c->ovl_map[i] + mk_at + (size_t)k * MK_W * MK_W * 4;
            for (int y = 0; y < side; y++)
                for (int x = 0; x < side; x++) {
                    int edge = x < border || y < border || x >= side - border || y >= side - border;
                    ovl_px(c, blk + (y * MK_W + x) * 4, edge ? 0 : d->rgb[0], edge ? 0 : d->rgb[1], edge ? 0 : d->rgb[2]);
                }
            int sx, sy;
            to_screen(c, d->x, d->y, &sx, &sy);
            sx -= side / 2, sy -= side / 2;
            int ox = sx < 0 ? -sx : 0, oy = sy < 0 ? -sy : 0;
            int w = side - ox, h = side - oy;
            if (sx + side > (int)c->screen_size.width) w -= sx + side - (int)c->screen_size.width;
            if (sy + side > (int)c->screen_size.height) h -= sy + side - (int)c->screen_size.height;
            if (w <= 0 || h <= 0) continue;
            mk[m++] = (VkBufferImageCopy){
                .bufferOffset = mk_at + ((VkDeviceSize)k * MK_W * MK_W + (VkDeviceSize)oy * MK_W + ox) * 4,
                .bufferRowLength = MK_W, .bufferImageHeight = MK_W,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { sx + ox, sy + oy, 0 }, .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
            };
        }
        if (m) vkCmdCopyBufferToImage(cb, c->ovl[i], c->screen[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, m, mk);
    }
    // the cursor: Ruffle's arrow at this scale, its solid pixels (a span a row)
    if (droid_cursor_shown) {
        static float art[CURA_W * CURA_H * 4];
        static float art_scale;
        if (art_scale != sc) {
            overlay_cursor_art(art, CURA_W, CURA_H, 4 * sc, sc);
            art_scale = sc;
        }
        uint8_t *buf = c->ovl_map[i] + OVL_W * OVL_H * 4;
        int cx, cy;
        to_screen(c, droid_cursor_x, droid_cursor_y, &cx, &cy);
        cx -= (int)(4 * sc), cy -= (int)(4 * sc);
        for (int y = 0; y < CURA_H && n < 256; y++) {
            for (int x = 0; x < CURA_W && n < 256;) {
                const float *p = art + (y * CURA_W + x) * 4;
                if (p[3] < 0.5f) { x++; continue; }
                int start = x;
                while (x < CURA_W && art[(y * CURA_W + x) * 4 + 3] >= 0.5f) {
                    const float *q = art + (y * CURA_W + x) * 4;
                    ovl_px(c, buf + (y * CURA_W + x) * 4, q[0], q[1], q[2]);
                    x++;
                }
                int sx = cx + start, sy = cy + y, len = x - start;
                if (sy < 0 || sy >= (int)c->screen_size.height || sx >= (int)c->screen_size.width) continue;
                int skip = sx < 0 ? -sx : 0;
                if (sx + len > (int)c->screen_size.width) len = (int)c->screen_size.width - sx;
                if (len - skip <= 0) continue;
                regions[n++] = (VkBufferImageCopy){
                    .bufferOffset = (VkDeviceSize)(OVL_W * OVL_H * 4 + (y * CURA_W + start + skip) * 4),
                    .bufferRowLength = CURA_W, .bufferImageHeight = CURA_H,
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageOffset = { sx + skip, sy, 0 }, .imageExtent = { (uint32_t)(len - skip), 1, 1 },
                };
            }
        }
    }
    if (n) vkCmdCopyBufferToImage(cb, c->ovl[i], c->screen[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regions);
}

static VkResult VKAPI_CALL b_vkGetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR s, uint32_t *count, VkImage *out) {
    scaled_chain *c = chain_of(s);
    if (!c) {
        DEV(vkGetSwapchainImagesKHR);
        return vkGetSwapchainImagesKHR(dev, s, count, out);
    }
    if (!out) {
        *count = c->count;
        return VK_SUCCESS;
    }
    uint32_t n = *count < c->count ? *count : c->count;
    memcpy(out, c->game, n * sizeof(VkImage));
    *count = n;
    return n < c->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static void VKAPI_CALL b_vkDestroySwapchainKHR(VkDevice dev, VkSwapchainKHR s, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    DEV(vkDestroySwapchainKHR);
    scaled_chain *c = chain_of(s);
    if (c && c == splash_chain) splash_stop(2); // (the next swapchain starts it again)
    if (c) {
        DEV(vkDeviceWaitIdle);
        DEV(vkDestroyImage);
        DEV(vkFreeMemory);
        DEV(vkDestroySemaphore);
        DEV(vkDestroyFence);
        DEV(vkDestroyCommandPool);
        DEV(vkDestroyBuffer);
        vkDeviceWaitIdle(dev);
        for (uint32_t i = 0; i < c->count; i++) {
            vkDestroyImage(dev, c->game[i], NULL);
            vkFreeMemory(dev, c->mem[i], NULL);
            vkDestroySemaphore(dev, c->done[i], NULL);
            vkDestroyFence(dev, c->fence[i], NULL);
            vkDestroyBuffer(dev, c->ovl[i], NULL);
            vkFreeMemory(dev, c->ovl_mem[i], NULL);
        }
        vkDestroyCommandPool(dev, c->pool, NULL);
        memset(c, 0, sizeof(*c));
    }
    vkDestroySwapchainKHR(dev, s, NULL);
}

static void barrier(PFN_vkCmdPipelineBarrier f, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    f(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// ---- a one-game app's splash (/app0/splash.png, the game's own: title/make_game_app.py): from
// the game's swapchain until its first frame (Subway Surfers: 12 s of black before), the picture in
// the middle on its corner's colour; at the first frame it fades out (0.4 s), then the game shows.
// The splash uses the game's queue and swapchain: while it's up, the game's queue calls and
// acquires take q_lock (after it, they don't).
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int splash_on;  // the thread is drawing
static volatile int splash_end; // 1: fade out and stop (the game's first frame); 2: stop now (its swapchain goes)
static pthread_t splash_thread;
static uint8_t *splash_px; // RGBA, fitted into the screen
static int splash_w, splash_h, splash_tried;
static int game_presented;

static void *splash_run(void *arg) {
    scaled_chain *c = arg;
    VkDevice dev = c->dev;
    DEV(vkGetDeviceQueue);
    DEV(vkCreateBuffer);
    DEV(vkGetBufferMemoryRequirements);
    DEV(vkAllocateMemory);
    DEV(vkBindBufferMemory);
    DEV(vkMapMemory);
    DEV(vkCreateCommandPool);
    DEV(vkAllocateCommandBuffers);
    DEV(vkCreateFence);
    DEV(vkCreateSemaphore);
    DEV(vkAcquireNextImageKHR);
    DEV(vkWaitForFences);
    DEV(vkResetFences);
    DEV(vkResetCommandBuffer);
    DEV(vkBeginCommandBuffer);
    DEV(vkEndCommandBuffer);
    DEV(vkCmdPipelineBarrier);
    DEV(vkCmdClearColorImage);
    DEV(vkCmdCopyBufferToImage);
    DEV(vkQueueSubmit);
    DEV(vkQueuePresentKHR);
    DEV(vkQueueWaitIdle);
    DEV(vkDestroyBuffer);
    DEV(vkFreeMemory);
    DEV(vkDestroyCommandPool);
    DEV(vkDestroyFence);
    DEV(vkDestroySemaphore);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    const VkDeviceSize bytes = (VkDeviceSize)splash_w * splash_h * 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t *map = NULL;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkMemoryRequirements req;
    vkCreateBuffer(dev, &bi, NULL, &buf);
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence acq = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    VkSemaphore ready[MAX_SC_IMAGES] = { 0 };
    int ok = vkAllocateMemory(dev, &ma, NULL, &mem) == VK_SUCCESS && vkBindBufferMemory(dev, buf, mem, 0) == VK_SUCCESS &&
             vkMapMemory(dev, mem, 0, bytes, 0, (void **)&map) == VK_SUCCESS;
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
    ok = ok && vkCreateCommandPool(dev, &pi, NULL, &pool) == VK_SUCCESS;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    ok = ok && vkAllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFenceCreateInfo fs = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    ok = ok && vkCreateFence(dev, &fi, NULL, &acq) == VK_SUCCESS && vkCreateFence(dev, &fs, NULL, &done) == VK_SUCCESS;
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < c->count && ok; i++) ok = vkCreateSemaphore(dev, &si, NULL, &ready[i]) == VK_SUCCESS;
    const int bgra = c->format == VK_FORMAT_B8G8R8A8_UNORM || c->format == VK_FORMAT_B8G8R8A8_SRGB;
    const int x0 = ((int)c->screen_size.width - splash_w) / 2, y0 = ((int)c->screen_size.height - splash_h) / 2;
    float level = 1, written = -1;
    int fade = 0, shown = 0;
    const int FADE_FRAMES = 12; // 0.4 s at 30 frames a second
    while (ok && splash_end != 2) {
        if (splash_end == 1) level = 1 - (float)++fade / FADE_FRAMES;
        if (level < 0) level = 0;
        if (level != written) { // the picture at this brightness, in the screen's byte order
            for (int i = 0; i < splash_w * splash_h; i++) {
                const uint8_t *s = splash_px + i * 4;
                uint8_t *d = map + i * 4;
                d[0] = (uint8_t)((bgra ? s[2] : s[0]) * level);
                d[1] = (uint8_t)(s[1] * level);
                d[2] = (uint8_t)((bgra ? s[0] : s[2]) * level);
                d[3] = 255;
            }
            written = level;
        }
        pthread_mutex_lock(&q_lock);
        uint32_t idx = 0;
        vkResetFences(dev, 1, &acq);
        VkResult r = vkAcquireNextImageKHR(dev, c->real, 100000000ULL, VK_NULL_HANDLE, acq, &idx);
        if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && idx < c->count) {
            vkWaitForFences(dev, 1, &acq, VK_TRUE, UINT64_MAX);
            vkWaitForFences(dev, 1, &done, VK_TRUE, UINT64_MAX);
            vkResetFences(dev, 1, &done);
            vkResetCommandBuffer(cb, 0);
            VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cb, &cbi);
            barrier(vkCmdPipelineBarrier, cb, c->screen[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT);
            const uint8_t *bg = splash_px; // the picture's corner: the colour around it
            const VkClearColorValue col = { .float32 = { bg[0] / 255.0f * level, bg[1] / 255.0f * level,
                                                         bg[2] / 255.0f * level, 1 } };
            const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, c->screen[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &all);
            barrier(vkCmdPipelineBarrier, cb, c->screen[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                      .imageOffset = { x0, y0, 0 },
                                      .imageExtent = { (uint32_t)splash_w, (uint32_t)splash_h, 1 } };
            vkCmdCopyBufferToImage(cb, buf, c->screen[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
            barrier(vkCmdPipelineBarrier, cb, c->screen[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            vkEndCommandBuffer(cb);
            VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb,
                                 .signalSemaphoreCount = 1, .pSignalSemaphores = &ready[idx] };
            vkQueueSubmit(q, 1, &sub, done);
            VkPresentInfoKHR pres = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                      .pWaitSemaphores = &ready[idx], .swapchainCount = 1, .pSwapchains = &c->real,
                                      .pImageIndices = &idx };
            vkQueuePresentKHR(q, &pres);
            if (!shown++) droid_log("splash: on the screen (%dx%d)", splash_w, splash_h);
        }
        pthread_mutex_unlock(&q_lock);
        if (splash_end == 1 && level <= 0) break;
        usleep(33000);
    }
    pthread_mutex_lock(&q_lock);
    if (ok) vkQueueWaitIdle(q);
    pthread_mutex_unlock(&q_lock);
    for (uint32_t i = 0; i < c->count; i++)
        if (ready[i]) vkDestroySemaphore(dev, ready[i], NULL);
    if (acq) vkDestroyFence(dev, acq, NULL);
    if (done) vkDestroyFence(dev, done, NULL);
    if (pool) vkDestroyCommandPool(dev, pool, NULL);
    if (buf) vkDestroyBuffer(dev, buf, NULL);
    if (mem) vkFreeMemory(dev, mem, NULL);
    droid_log("splash: %s after %d frames%s", splash_end == 1 ? "faded out" : "stopped", shown, ok ? "" : " (no Vulkan objects)");
    splash_on = 0;
    return NULL;
}

// at a new swapchain, before the game's first frame: the splash on it (a one-game app with a splash.png)
static void splash_start(scaled_chain *c) {
    extern int droid_single_game; // app_main.c
    if (!droid_single_game || game_presented || splash_on) return;
    if (!splash_tried) {
        splash_tried = 1;
        FILE *f = fopen(APP0 "/splash.png", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *data = n > 0 ? malloc((size_t)n) : NULL;
            if (data && fread(data, 1, (size_t)n, f) == (size_t)n)
                splash_px = library_image_decode(data, (size_t)n, (int)c->screen_size.width, (int)c->screen_size.height,
                                                 &splash_w, &splash_h);
            free(data);
            fclose(f);
        }
        droid_log("splash: %s", splash_px ? "the app's splash.png" : "none (no /app0/splash.png)");
    }
    if (!splash_px) return;
    splash_chain = c;
    splash_end = 0;
    splash_on = 1;
    if (pthread_create(&splash_thread, NULL, splash_run, c) != 0) splash_on = 0;
}

// stop it: 1 fades out first (the game's first frame waits for that), 2 at once
static void splash_stop(int how) {
    if (!splash_on) return;
    splash_end = how;
    pthread_join(splash_thread, NULL);
    splash_chain = NULL;
}

// the game's queue calls and acquires, locked while the splash shares them
static VkResult VKAPI_CALL b_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    static PFN_vkQueueSubmit real;
    if (!real) real = (PFN_vkQueueSubmit)host_gipa(the_instance, "vkQueueSubmit");
    if (!splash_on) return real(q, n, s, f);
    pthread_mutex_lock(&q_lock);
    VkResult r = real(q, n, s, f);
    pthread_mutex_unlock(&q_lock);
    return r;
}
static VkResult VKAPI_CALL b_vkQueueWaitIdle(VkQueue q) {
    static PFN_vkQueueWaitIdle real;
    if (!real) real = (PFN_vkQueueWaitIdle)host_gipa(the_instance, "vkQueueWaitIdle");
    if (!splash_on) return real(q);
    pthread_mutex_lock(&q_lock);
    VkResult r = real(q);
    pthread_mutex_unlock(&q_lock);
    return r;
}
static VkResult VKAPI_CALL b_vkAcquireNextImageKHR(VkDevice dev, VkSwapchainKHR s, uint64_t timeout, VkSemaphore sem,
                                                   VkFence f, uint32_t *idx) {
    static PFN_vkAcquireNextImageKHR real;
    if (!real) real = (PFN_vkAcquireNextImageKHR)host_gipa(the_instance, "vkAcquireNextImageKHR");
    if (!splash_on) return real(dev, s, timeout, sem, f, idx);
    pthread_mutex_lock(&q_lock);
    VkResult r = real(dev, s, timeout, sem, f, idx);
    pthread_mutex_unlock(&q_lock);
    return r;
}

// The game's image stretched onto the screen's; the present then waits for that.
int droid_shot_due(void); // overlay_art.c: ShotBox
void droid_shot_write(const uint8_t *px, int w, int h, size_t stride, int bgra, int bottom_up, int sec);
static struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } shot;

static VkResult scale_for_present(VkQueue q, scaled_chain *c, uint32_t i, const VkPresentInfoKHR *info,
                                  VkSemaphore *wait) {
    VkDevice dev = c->dev;
    DEV(vkWaitForFences);
    DEV(vkResetFences);
    DEV(vkResetCommandBuffer);
    DEV(vkBeginCommandBuffer);
    DEV(vkEndCommandBuffer);
    DEV(vkCmdPipelineBarrier);
    DEV(vkCmdBlitImage);
    DEV(vkQueueSubmit);
    vkWaitForFences(dev, 1, &c->fence[i], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &c->fence[i]);
    VkCommandBuffer cb = c->cmd[i];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    barrier(vkCmdPipelineBarrier, cb, c->game[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(vkCmdPipelineBarrier, cb, c->screen[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    int px, py, pw, ph;
    pic_rect(c, &px, &py, &pw, &ph);
    if (phone_view()) { // the sides black, then the picture in the middle
        DEV(vkCmdClearColorImage);
        const VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
        const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, c->screen[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        barrier(vkCmdPipelineBarrier, cb, c->screen[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)c->game_size.width, (int32_t)c->game_size.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { px, py, 0 }, { px + pw, py + ph, 1 } },
    };
    vkCmdBlitImage(cb, c->game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->screen[i],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(vkCmdPipelineBarrier, cb, c->screen[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    draw_overlay(c, i, cb); // the FPS counter and the cursor on top
    // ShotBox: the game's picture (before our overlay), into a buffer we can read
    int shot_sec = droid_shot_due();
    VkDeviceSize shot_bytes = (VkDeviceSize)c->game_size.width * c->game_size.height * 4;
    if (shot_sec && shot.size < shot_bytes) {
        DEV(vkCreateBuffer);
        DEV(vkGetBufferMemoryRequirements);
        DEV(vkAllocateMemory);
        DEV(vkBindBufferMemory);
        DEV(vkMapMemory);
        VkBufferCreateInfo sb = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = shot_bytes,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        VkMemoryRequirements r;
        if (vkCreateBuffer(dev, &sb, NULL, &shot.buf) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(dev, shot.buf, &r);
            VkMemoryAllocateInfo am = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                                        .memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
            if (vkAllocateMemory(dev, &am, NULL, &shot.mem) == VK_SUCCESS &&
                vkBindBufferMemory(dev, shot.buf, shot.mem, 0) == VK_SUCCESS &&
                vkMapMemory(dev, shot.mem, 0, shot_bytes, 0, &shot.map) == VK_SUCCESS)
                shot.size = shot_bytes;
        }
    }
    if (shot_sec && shot.size >= shot_bytes) {
        DEV(vkCmdCopyImageToBuffer);
        VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { c->game_size.width, c->game_size.height, 1 } };
        vkCmdCopyImageToBuffer(cb, c->game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &rg);
    } else {
        shot_sec = 0;
    }
    barrier(vkCmdPipelineBarrier, cb, c->screen[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    barrier(vkCmdPipelineBarrier, cb, c->game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags stages[8];
    uint32_t nw = info->waitSemaphoreCount < 8 ? info->waitSemaphoreCount : 8;
    for (uint32_t k = 0; k < nw; k++) stages[k] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nw,
        .pWaitSemaphores = info->pWaitSemaphores,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &c->done[i],
    };
    *wait = c->done[i];
    VkResult r = vkQueueSubmit(q, 1, &si, c->fence[i]);
    if (shot_sec && r == VK_SUCCESS) { // ShotBox: its copy done, written out
        vkWaitForFences(dev, 1, &c->fence[i], VK_TRUE, UINT64_MAX);
        int bgra = c->format == VK_FORMAT_B8G8R8A8_UNORM || c->format == VK_FORMAT_B8G8R8A8_SRGB;
        droid_shot_write(shot.map, (int)c->game_size.width, (int)c->game_size.height, (size_t)c->game_size.width * 4,
                         bgra, 0, shot_sec);
    }
    return r;
}

// ---- the Quick menu (quick_menu.c) on the Vulkan path: after the game's present, the frame
// it showed read back, then frames of ours on the real swapchain until it's shut: the frosted
// frame (an image of ours) stretched over the screen, the card copied over it (made opaque
// over the backdrop on the CPU: copies don't blend). Two sets of buffers, one frame's each in
// turn: a frame waits only for the GPU to be done with its set (a wait for the whole queue
// after each present cost a refresh: Unity games' menus ran at 38-48 fps).
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; } host_buf;
static double qm_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int host_buf_make(VkDevice dev, host_buf *b, VkDeviceSize size) {
    DEV(vkCreateBuffer);
    DEV(vkGetBufferMemoryRequirements);
    DEV(vkAllocateMemory);
    DEV(vkBindBufferMemory);
    DEV(vkMapMemory);
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (vkCreateBuffer(dev, &bi, NULL, &b->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b->buf, &r);
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                                .memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    void *m = NULL;
    if (vkAllocateMemory(dev, &ma, NULL, &b->mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(dev, b->buf, b->mem, 0) != VK_SUCCESS || vkMapMemory(dev, b->mem, 0, size, 0, &m) != VK_SUCCESS)
        return 0;
    b->map = m;
    return 1;
}

static void host_buf_free(VkDevice dev, host_buf *b) {
    DEV(vkDestroyBuffer);
    DEV(vkFreeMemory);
    if (b->buf) vkDestroyBuffer(dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static void quick_menu_vk(VkQueue q, scaled_chain *c, uint32_t i, PFN_vkQueuePresentKHR present) {
    VkDevice dev = c->dev;
    DEV(vkQueueWaitIdle);
    DEV(vkWaitForFences);
    DEV(vkResetFences);
    DEV(vkResetCommandBuffer);
    DEV(vkBeginCommandBuffer);
    DEV(vkEndCommandBuffer);
    DEV(vkCmdPipelineBarrier);
    DEV(vkCmdCopyImageToBuffer);
    DEV(vkCmdCopyBufferToImage);
    DEV(vkCmdBlitImage);
    DEV(vkQueueSubmit);
    DEV(vkCreateImage);
    DEV(vkGetImageMemoryRequirements);
    DEV(vkAllocateMemory);
    DEV(vkBindImageMemory);
    DEV(vkDestroyImage);
    DEV(vkFreeMemory);
    DEV(vkCreateFence);
    DEV(vkDestroyFence);
    DEV(vkAcquireNextImageKHR);
    const int gw = (int)c->game_size.width, gh = (int)c->game_size.height;
    const int sw = (int)c->screen_size.width, sh = (int)c->screen_size.height;
    const int bgra = c->format == VK_FORMAT_B8G8R8A8_UNORM || c->format == VK_FORMAT_B8G8R8A8_SRGB;
    host_buf frame = { 0 }, card = { 0 }, frame2 = { 0 }, card2 = { 0 };
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkFence acq = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    int ok = 0, began = 0, frames = 0;

    // the game's frame (the one just shown), read back
    vkQueueWaitIdle(q);
    if (host_buf_make(dev, &frame, (VkDeviceSize)gw * gh * 4)) {
        VkCommandBuffer cb = c->cmd[i];
        vkWaitForFences(dev, 1, &c->fence[i], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &c->fence[i]);
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        barrier(vkCmdPipelineBarrier, cb, c->game[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { (uint32_t)gw, (uint32_t)gh, 1 } };
        vkCmdCopyImageToBuffer(cb, c->game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.buf, 1, &rg);
        barrier(vkCmdPipelineBarrier, cb, c->game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT, 0);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        if (vkQueueSubmit(q, 1, &si, c->fence[i]) == VK_SUCCESS) {
            vkWaitForFences(dev, 1, &c->fence[i], VK_TRUE, UINT64_MAX);
            ok = began = quick_menu_begin(frame.map, gw, gh, (size_t)gw * 4, bgra, 0, sw, sh);
        }
    }
    if (!ok) quick_menu_begin(NULL, 0, 0, 0, 0, 0, sw, sh); // (the request let go)

    // the backdrop's image, the card's buffer, the acquire's fence
    int bw = 0, bh = 0, cx, cy, cw = 0, ch = 0;
    unsigned bv, cv;
    float ca;
    if (ok) {
        ok = 0;
        quick_menu_backdrop(&bw, &bh, &bv);
        quick_menu_card_max(&cw, &ch); // (its buffers: room for the biggest card, the editor's tray)
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = c->format, .extent = { (uint32_t)bw, (uint32_t)bh, 1 }, .mipLevels = 1,
                                 .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateImage(dev, &ii, NULL, &img) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(dev, img, &req);
            VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                        .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
            ok = vkAllocateMemory(dev, &ma, NULL, &img_mem) == VK_SUCCESS && vkBindImageMemory(dev, img, img_mem, 0) == VK_SUCCESS &&
                 (size_t)bw * bh <= (size_t)gw * gh && host_buf_make(dev, &card, (VkDeviceSize)cw * ch * 4) &&
                 host_buf_make(dev, &card2, (VkDeviceSize)cw * ch * 4) && host_buf_make(dev, &frame2, (VkDeviceSize)gw * gh * 4) &&
                 vkCreateFence(dev, &fi, NULL, &acq) == VK_SUCCESS;
        }
        if (!ok) droid_log("vulkan: quick menu: no image / buffer for it");
    }

    // each set: its buffers, what's in them, the fence of the frame that last read them
    host_buf *fset[2] = { &frame, &frame2 }, *cset[2] = { &card, &card2 };
    VkFence set_fence[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    unsigned set_bv[2] = { 0, 0 }, set_cv[2] = { 0, 0 }, set_cbv[2] = { 0, 0 }, img_bv = 0;
    float set_ca[2] = { -1, -1 };
    int set_cx[2] = { -1, -1 }, set_cy[2] = { -1, -1 }, img_ready = 0;
    double t_menu = 0, t_wait = 0, t_cpu = 0, t_gpu = 0;
    int n_back = 0, n_card = 0;
    while (ok) {
        double q0 = qm_now();
        int more = quick_menu_frame();
        double q1 = qm_now();
        const int k = frames & 1;
        if (set_fence[k]) vkWaitForFences(dev, 1, &set_fence[k], VK_TRUE, UINT64_MAX); // (the GPU done with this set)
        double q1b = qm_now();
        const uint8_t *b = quick_menu_backdrop(&bw, &bh, &bv);
        if (b && set_bv[k] != bv) { // into this set's buffer in the screen's byte order (a swap of R and B: 4 bytes at once)
            const size_t n = (size_t)bw * bh;
            const uint32_t *src = (const uint32_t *)b;
            uint32_t *dst = (uint32_t *)fset[k]->map;
            if (bgra)
                for (size_t q = 0; q < n; q++) {
                    uint32_t v = src[q];
                    dst[q] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
                }
            else
                memcpy(dst, src, n * 4);
            set_bv[k] = bv;
            n_back++;
        }
        const int back_new = b && (img_bv != bv || !img_ready);
        quick_menu_card(&cx, &cy, &cw, &ch, &ca, &cv);
        if (cv != set_cv[k] || ca != set_ca[k] || cx != set_cx[k] || cy != set_cy[k] || set_cbv[k] != bv) {
            quick_menu_card_opaque(cset[k]->map, (size_t)cw * 4, bgra);
            n_card++;
            set_cv[k] = cv, set_ca[k] = ca, set_cx[k] = cx, set_cy[k] = cy, set_cbv[k] = bv;
        }
        double q2 = qm_now();
        uint32_t j = 0;
        VkResult r = vkAcquireNextImageKHR(dev, c->real, UINT64_MAX, VK_NULL_HANDLE, acq, &j);
        if ((r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) || j >= c->count) {
            droid_log("vulkan: quick menu: acquire -> %d: shut", r);
            break;
        }
        vkWaitForFences(dev, 1, &acq, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &acq);
        vkWaitForFences(dev, 1, &c->fence[j], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &c->fence[j]);
        VkCommandBuffer cb = c->cmd[j];
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        if (back_new) {
            barrier(vkCmdPipelineBarrier, cb, img, img_ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                     .imageExtent = { (uint32_t)bw, (uint32_t)bh, 1 } };
            vkCmdCopyBufferToImage(cb, fset[k]->buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            barrier(vkCmdPipelineBarrier, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            img_ready = 1;
            img_bv = bv;
        }
        barrier(vkCmdPipelineBarrier, cb, c->screen[j], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { bw, bh, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { sw, sh, 1 } },
        };
        vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->screen[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
        barrier(vkCmdPipelineBarrier, cb, c->screen[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (ca > 0.002f) { // the card, the part of it on the screen
            int x0 = cx < 0 ? -cx : 0, y0 = cy < 0 ? -cy : 0;
            int w = cw - x0, h = ch - y0;
            if (cx + cw > sw) w -= cx + cw - sw;
            if (cy + ch > sh) h -= cy + ch - sh;
            if (w > 0 && h > 0) {
                VkBufferImageCopy rg = { .bufferOffset = ((VkDeviceSize)y0 * cw + x0) * 4, .bufferRowLength = (uint32_t)cw,
                                         .bufferImageHeight = (uint32_t)ch,
                                         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                         .imageOffset = { cx + x0, cy + y0, 0 },
                                         .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
                vkCmdCopyBufferToImage(cb, cset[k]->buf, c->screen[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            }
        }
        barrier(vkCmdPipelineBarrier, cb, c->screen[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb,
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &c->done[j] };
        if (vkQueueSubmit(q, 1, &si, c->fence[j]) != VK_SUCCESS) break;
        set_fence[k] = c->fence[j];
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &c->done[j], .swapchainCount = 1, .pSwapchains = &c->real,
                                .pImageIndices = &j };
        present(q, &pi);
        frames++;
        double q3 = qm_now();
        t_menu += q1 - q0, t_wait += q1b - q1, t_cpu += q2 - q1b, t_gpu += q3 - q2;
        if (!more) break;
    }
    if (frames > 0)
        droid_log("vulkan: quick menu: a frame's time: %.1f ms the menu (with its wait), %.1f ms waiting for the GPU, %.1f ms the pictures, %.1f ms acquire + submit + present; %d backdrops, %d cards made",
                  t_menu * 1000 / frames, t_wait * 1000 / frames, t_cpu * 1000 / frames, t_gpu * 1000 / frames, n_back, n_card);
    vkQueueWaitIdle(q);
    if (acq) vkDestroyFence(dev, acq, NULL);
    if (img) vkDestroyImage(dev, img, NULL);
    if (img_mem) vkFreeMemory(dev, img_mem, NULL);
    host_buf_free(dev, &card);
    host_buf_free(dev, &card2);
    host_buf_free(dev, &frame);
    host_buf_free(dev, &frame2);
    if (began) droid_log("vulkan: quick menu: %d frames", frames);
    if (began) quick_menu_end(); // (Close Game: the app closes from here)
}

// The first present: the screen is up (the PS5's Choreographer then ticks on its vblank).
// ---- frame pacing: a steady 60 on a 60 Hz screen; on 120 Hz: 120 when the game keeps up,
// else an even 60 ------------------------------------------------------------------------
// A game at 85 fps on a 120 Hz screen shows its frames for 1 or 2 refreshes unevenly: it
// looks rough though the number is higher. The game's own time per frame (from one
// present's return to the next present) says what it can do: 120 Hz when it keeps up
// (with a margin), else every frame shown for 2 refreshes, an even 60. Looked at every 2 s.
extern int droid_vblank_hz; // the screen's (vk_ps5.c)

static double pace_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double back_at; // when the last present returned (the game's next frame starts)
static void pace_returned(void) { back_at = pace_now(); }

// how long each frame stayed on screen, in refreshes (judder: 1s and 3s mixed in a 2s game)
static void pace_histogram(double t) {
    static double last, since;
    static int bins[5]; // ~1, ~2, ~3, ~4+ refreshes, and under a refresh
    if (last > 0) {
        double r = (t - last) * 59.94;
        bins[r < 0.6 ? 4 : r < 1.5 ? 0 : r < 2.5 ? 1 : r < 3.5 ? 2 : 3]++;
    } else {
        since = t;
    }
    last = t;
    if (t - since >= 5.0) {
        droid_log("vulkan: frames shown for 1 refresh %d, 2 refreshes %d, 3 refreshes %d, 4+ %d (under one: %d)",
                  bins[0], bins[1], bins[2], bins[3], bins[4]);
        memset(bins, 0, sizeof(bins));
        since = t;
    }
}

static void pace_frame(void) {
    static int even60, initialized, half;
    static double next_at, win_start, work, gaps;
    static int win_frames;
    double t = pace_now();
    if (!initialized) {
        initialized = 1;
        win_start = back_at = next_at = t;
        return;
    }
    work += t - back_at; // the game's part of this frame
    win_frames++;
    // the game's own pace: a game at 30 (Among Us, many Unity games) gets every frame shown for
    // exactly 2 refreshes. Snapped to the next refresh, a frame a little late stayed 3 and the
    // next one only 1: 50 ms then 16 ms, judder at "30 fps"
    {
        // (held at 2 refreshes the gaps are 33 ms whatever the game wants: then the game's
        // own time per frame tells - a game that paces itself at 30 spends most of it waiting)
        static double prev, own;
        if (prev > 0) {
            double g = t - prev, o = t - back_at;
            if (g < 0.2) gaps = gaps > 0 ? gaps * 0.9 + g * 0.1 : g;
            if (g < 0.2) own = own > 0 ? own * 0.9 + o * 0.1 : o;
        }
        prev = t;
        int want = half ? own > 0.018 : gaps > 0.030 && gaps < 0.042;
        static int other; // frames in a row wanting the other way: a second of them to switch
        other = want != half ? other + 1 : 0;
        if (other >= 30) {
            other = 0;
            half = want;
            droid_log("vulkan: pacing: %s", half ? "the game runs at 30: every frame 2 refreshes" : "every refresh");
        }
    }
    if (t - win_start >= 2.0 && win_frames > 0) {
        double can = win_frames / (work > 1e-6 ? work : 1e-6); // fps the game could make
        int want60 = even60 ? can < 118 : can < 108; // a margin each way
        if (want60 != even60) {
            even60 = want60;
            if (droid_vblank_hz >= 100) droid_log("vulkan: pacing: %s (the game makes up to %.0f fps)", even60 ? "an even 60" : "120 Hz", can);
        }
        win_start = t;
        work = 0;
        win_frames = 0;
    }
    // the frame's slot: the app's FPS cap (on by default) or the even 60 on a 120 Hz screen:
    // one 59.94 Hz refresh per frame. The PS5's display doesn't hold a FIFO present back for
    // the refresh (Subway made 76 fps on the 60 Hz mode), so the cap is this wait. Not on the
    // PC runner: its present waits, and a second clock there lost frames.
#ifdef DROID_PC
    double slot = half ? 2.0 / 59.94 : 0;
#else
    const int cap = app_fps_cap_hz(); // Settings > Performance > FPS cap: 30 / 60 / 120 / off
    double slot = cap == 30 ? 2.0 / 59.94 : cap == 120 ? 1.0 / 119.88 : cap || (even60 && droid_vblank_hz >= 100) ? 1.0 / 59.94 : 0;
    if (half) slot = 2.0 / 59.94;
    extern int droid_flip_rate; // gles_bridge.c: FlipRate on, the console holds frames itself
    if (droid_flip_rate > 0) slot = 0;
#endif
    if (slot > 0) {
        next_at += slot;
        // on the screen's real vblank grid (the Choreographer's), a frame 1 ms after a
        // refresh: the GPU then has a whole refresh for the game's work and our upscale before
        // the flip (2 ms before one, the flip slipped: frames of 18-26 ms, judder at "60")
        extern volatile int64_t droid_vblank_ns; // java_layer.c
        double v = droid_vblank_ns / 1e9, period = 1.0 / 59.94;
        if (v > 0 && t - v < 1.0) {
            double k = floor((next_at - 0.001 - v) / period + 0.5);
            double snapped = v + k * period + 0.001;
            if (snapped > t - 0.05) next_at = snapped;
        }
        if (next_at < t - 0.05) next_at = t; // far behind (a load): start over
        while (t < next_at - 0.0003) {
            double left = next_at - t;
            struct timespec d = { 0, (long)((left > 0.002 ? left - 0.001 : left) * 1e9) };
            nanosleep(&d, NULL);
            t = pace_now();
        }
    } else {
        next_at = t;
    }
    pace_histogram(pace_now());
}

// the same pacing for ANGLE's swaps (gles_bridge.c): it shows its frames on this display too
void droid_pace_before_present(void) { pace_frame(); }
void droid_pace_after_present(void) { pace_returned(); }

static VkResult VKAPI_CALL b_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info) {
    static PFN_vkQueuePresentKHR f;
    static unsigned long presents;
    if (!f) f = (PFN_vkQueuePresentKHR)host_gipa(the_instance, "vkQueuePresentKHR");
    if (!game_presented) { // the game's first frame: the splash fades out before it
        game_presented = 1;
        splash_stop(1);
    }
    pace_frame();
    VkResult r;
    scaled_chain *c = info->swapchainCount == 1 ? chain_of(info->pSwapchains[0]) : NULL;
    if (c) { // the game's frame onto the screen, then the screen's image presented
        VkSemaphore wait;
        r = scale_for_present(q, c, info->pImageIndices[0], info, &wait);
        VkPresentInfoKHR mine = *info;
        mine.waitSemaphoreCount = 1;
        mine.pWaitSemaphores = &wait;
        if (r == VK_SUCCESS) r = f(q, &mine);
        if (r == VK_SUCCESS && quick_menu_wanted()) quick_menu_vk(q, c, info->pImageIndices[0], f); // Options held
    } else {
        r = f(q, info);
    }
    if (presents++ == 0) {
        droid_log("vulkan: first frame presented (%d)", r);
        droid_startup_frame("Vulkan");
        vk_platform_presented();
    }
    pace_returned();
    return r;
}

static PFN_vkVoidFunction VKAPI_CALL b_vkGetInstanceProcAddr(VkInstance instance, const char *name);
static PFN_vkVoidFunction VKAPI_CALL b_vkGetDeviceProcAddr(VkDevice device, const char *name);

static const struct { const char *name; void *fn; } ours[] = {
#define O(n) { #n, (void *)b_##n }
    O(vkGetInstanceProcAddr), O(vkGetDeviceProcAddr), O(vkEnumerateInstanceExtensionProperties),
    O(vkEnumerateInstanceLayerProperties), O(vkEnumerateDeviceLayerProperties), O(vkCreateInstance),
    O(vkCreateAndroidSurfaceKHR), O(vkCreateDevice),
    O(vkCreateDebugUtilsMessengerEXT), O(vkDestroyDebugUtilsMessengerEXT),
    O(vkGetPhysicalDeviceProperties), O(vkGetPhysicalDeviceProperties2), O(vkQueuePresentKHR),
    O(vkEnumeratePhysicalDevices), O(vkGetPhysicalDeviceSurfaceCapabilitiesKHR), O(vkCreateSwapchainKHR),
    O(vkGetSwapchainImagesKHR), O(vkDestroySwapchainKHR),
    O(vkQueueSubmit), O(vkQueueWaitIdle), O(vkAcquireNextImageKHR), // (locked only while a splash shows)
    { "vkGetPhysicalDeviceProperties2KHR", (void *)b_vkGetPhysicalDeviceProperties2 },
    { "vkCreateDebugReportCallbackEXT", (void *)b_vkCreateDebugUtilsMessengerEXT },
    { "vkDestroyDebugReportCallbackEXT", (void *)b_vkDestroyDebugUtilsMessengerEXT },
#undef O
};

// The ARM thunk for a command: ours, or the host's (looked up now, or once an
// instance / device is there - thunks are by name, filled in as they become known).
static void *thunk_for(const char *name, void *host) {
    for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
        if (!strcmp(ours[i].name, name)) host = ours[i].fn;
    if (!host) return NULL;
    static int traced;
    if (traced++ < 400) droid_log("vulkan: the game takes %s", name);
    uint64_t t = arm_thunk_set(name, host);
    int a = allocator_arg(name);
    if (a >= 0) arm_thunk_null_arg(name, a);
    return (void *)t;
}

static PFN_vkVoidFunction VKAPI_CALL b_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    void *host = (void *)host_gipa(instance ? instance : the_instance, name);
    if (!host && !instance) host = (void *)host_gipa(NULL, name);
    void *t = thunk_for(name, host);
    static int logged;
    if (!t && logged++ < 40) droid_log("vulkan: %s: not had", name);
    return (PFN_vkVoidFunction)t;
}

static PFN_vkVoidFunction VKAPI_CALL b_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    void *host = host_gdpa ? (void *)host_gdpa(device, name) : NULL;
    if (!host) host = (void *)host_gipa(the_instance, name);
    void *t = thunk_for(name, host);
    static int logged;
    if (!t && logged++ < 40) droid_log("vulkan: %s (device): not had", name);
    return (PFN_vkVoidFunction)t;
}

// dlsym(libvulkan.so, name): Android's libvulkan exports the core commands too. One the
// host only gives with an instance gets its thunk now, filled at vkCreateInstance.
static char *pending[512];
static int pending_count;

static void fill_pending(void) {
    for (int i = 0; i < pending_count; i++) {
        void *host = (void *)host_gipa(the_instance, pending[i]);
        if (host) thunk_for(pending[i], host);
    }
}

void *vk_bridge_dlsym(const char *name) {
    if (!vk_bridge_enabled() || strncmp(name, "vk", 2) != 0) return NULL;
    void *t = (void *)b_vkGetInstanceProcAddr(NULL, name);
    if (t || the_instance) return t;
    if (pending_count < 512) pending[pending_count++] = strdup(name);
    return (void *)arm_thunk_set(name, NULL); // "missing" until filled
}
