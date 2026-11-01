#ifndef VULKAN_BRIDGE_H
#define VULKAN_BRIDGE_H

#include <vulkan/vulkan.h>

// Android's libvulkan.so for ARM code (vulkan_bridge.c). Off unless /app0/data/vulkan
// exists (or DROID_VULKAN=1): the game then falls back to GLES.
int vk_bridge_enabled(void);
void *vk_bridge_dlsym(const char *name); // an ARM-callable command, or NULL

// The platform's part (vk_ps5.c on the console, pc/vk_pc.c in the PC test runner):
PFN_vkGetInstanceProcAddr vk_platform_gipa(void); // NULL: no Vulkan here
int vk_platform_instance_extensions(const char **out, int max); // for the window
VkResult vk_platform_create_surface(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, VkSurfaceKHR *surface);
const char *vk_platform_name(void);
void vk_platform_presented(void); // the first frame is on screen

#endif
