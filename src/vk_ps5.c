// vk_ps5.c — the PS5's part of the Vulkan bridge: RADV (PS5_Vulkan, linked in) and the
// TV through VK_KHR_display, the way PS5_VulkanTemplate's direct-to-display surface does.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_settings.h"
#include "android_shim.h"
#include "vulkan_bridge.h"

// RADV's entry point; weak, so a build without RADV links (and says "no Vulkan").
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) __attribute__((weak));

PFN_vkGetInstanceProcAddr vk_platform_gipa(void) {
    if (!vk_icdGetInstanceProcAddr) droid_log("vulkan: this build has no RADV");
    return vk_icdGetInstanceProcAddr ? (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr : NULL;
}

int vk_platform_instance_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = "VK_KHR_surface";
    out[1] = "VK_KHR_display";
    return 2;
}

const char *vk_platform_name(void) { return "PS5 display"; }

void droid_screen_ready(void); // gles_bridge.c: the video-out port's vblank for the Choreographer
void vk_platform_presented(void) { droid_screen_ready(); }

#define GET(name) PFN_##name name = (PFN_##name)gipa(instance, #name)

VkResult vk_platform_create_surface(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, VkSurfaceKHR *surface) {
    GET(vkEnumeratePhysicalDevices);
    GET(vkGetPhysicalDeviceDisplayPropertiesKHR);
    GET(vkGetDisplayModePropertiesKHR);
    GET(vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    GET(vkGetDisplayPlaneSupportedDisplaysKHR);
    GET(vkCreateDisplayPlaneSurfaceKHR);
    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceDisplayPropertiesKHR || !vkCreateDisplayPlaneSurfaceKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &n, &pd);
    if (!pd) return VK_ERROR_INITIALIZATION_FAILED;
    // the display, and its 1920x1080 mode (else its first)
    uint32_t dn = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &dn, NULL);
    if (!dn) return VK_ERROR_INITIALIZATION_FAILED;
    VkDisplayPropertiesKHR *dp = calloc(dn, sizeof(*dp));
    vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &dn, dp);
    VkDisplayKHR display = dp[0].display;
    free(dp);
    uint32_t mn = 0;
    vkGetDisplayModePropertiesKHR(pd, display, &mn, NULL);
    if (!mn) return VK_ERROR_INITIALIZATION_FAILED;
    VkDisplayModePropertiesKHR *mp = calloc(mn, sizeof(*mp));
    vkGetDisplayModePropertiesKHR(pd, display, &mn, mp);
    // The mode: 60 Hz (a 30/60 fps game fits it evenly; hz120 file: 120), 1920x1080 if the
    // display has it. The game draws 1920x1080 (its window), so the surface is that size
    // and the PS5 scales it to the mode (a 4K-only display showed it in a quarter).
    // auto: 60 (even frames); 120 only when the game's options ask and the app's FPS cap is off
    int want_hz = app_fps_cap_hz() == 120 ? 120 : app_settings.fps_cap ? 60 : settings.refresh_hz ? settings.refresh_hz : 60;
    int best = 0, best_score = -1;
    for (uint32_t i = 0; i < mn; i++) {
        const VkDisplayModeParametersKHR *p = &mp[i].parameters;
        droid_log("vulkan: display mode %u: %ux%u at %.2f Hz", i, p->visibleRegion.width, p->visibleRegion.height,
                  p->refreshRate / 1000.0);
        int hz = (int)((p->refreshRate + 500) / 1000);
        int score = (hz == want_hz ? 4 : 0) + (p->visibleRegion.width == 1920 ? 2 : 0) + (hz <= 60 ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    VkDisplayModeKHR mode = mp[best].displayMode;
    VkExtent2D mode_size = mp[best].parameters.visibleRegion;
    {
        extern int droid_vblank_hz; // the Choreographer ticks at it; Unity's frame rate follows it
        droid_vblank_hz = (int)((mp[best].parameters.refreshRate + 500) / 1000);
    }
    droid_log("vulkan: using mode %d (%ux%u at %.2f Hz)", best, mode_size.width, mode_size.height,
              mp[best].parameters.refreshRate / 1000.0);
    free(mp);
    VkExtent2D extent = { 1920, 1080 }; // the game's window size
    // a plane that shows that display
    uint32_t pn = 0, plane = 0;
    if (vkGetPhysicalDeviceDisplayPlanePropertiesKHR) vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &pn, NULL);
    for (uint32_t p = 0; p < pn && vkGetDisplayPlaneSupportedDisplaysKHR; p++) {
        uint32_t c = 0;
        vkGetDisplayPlaneSupportedDisplaysKHR(pd, p, &c, NULL);
        VkDisplayKHR *ds = calloc(c ? c : 1, sizeof(*ds));
        vkGetDisplayPlaneSupportedDisplaysKHR(pd, p, &c, ds);
        int found = 0;
        for (uint32_t i = 0; i < c; i++) found |= ds[i] == display;
        free(ds);
        if (found) {
            plane = p;
            break;
        }
    }
    VkDisplaySurfaceCreateInfoKHR si = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode,
        .planeIndex = plane,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = extent,
    };
    VkResult r = vkCreateDisplayPlaneSurfaceKHR(instance, &si, NULL, surface);
    droid_log("vulkan: display surface %ux%u on plane %u (mode %ux%u) -> %d", extent.width, extent.height, plane,
              mode_size.width, mode_size.height, r);
    if (r != VK_SUCCESS && (extent.width != mode_size.width || extent.height != mode_size.height)) {
        si.imageExtent = mode_size; // the mode's own size, then
        r = vkCreateDisplayPlaneSurfaceKHR(instance, &si, NULL, surface);
        droid_log("vulkan: display surface at the mode's %ux%u -> %d", mode_size.width, mode_size.height, r);
    }
    return r;
}
