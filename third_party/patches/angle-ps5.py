#!/usr/bin/env python3
# angle-ps5.py — the few source changes ANGLE needs to build for the PS5 (run again any time:
# each change is made once). Usage: python3 angle-ps5.py <angle source dir>
import os, sys

src = sys.argv[1]


def patch(rel, old, new):
    p = os.path.join(src, rel)
    s = open(p, encoding='utf-8').read()
    if new in s:
        return
    if old not in s:
        sys.exit(f'angle-ps5: {rel}: the text to change is not there (ANGLE changed?)')
    open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
    print(f'angle-ps5: {rel} patched')


# the PS5 (its compiler says FreeBSD): ANGLE's Linux paths, the Vulkan display among them
patch('src/common/platform.h',
      '#elif defined(__linux__) || defined(EMSCRIPTEN)\n#    define ANGLE_PLATFORM_LINUX 1',
      '#elif defined(ANGLE_PLATFORM_PS5)\n#    define ANGLE_PLATFORM_LINUX 1\n#    define ANGLE_PLATFORM_POSIX 1\n'
      '#elif defined(__linux__) || defined(EMSCRIPTEN)\n#    define ANGLE_PLATFORM_LINUX 1')

# the console's libc has no pthread_setname_np (ANGLE only names its worker threads)
patch('src/common/system_utils_linux.cpp',
      '    pthread_setname_np(pthread_self(), name);',
      '#if !defined(ANGLE_PLATFORM_PS5)\n    pthread_setname_np(pthread_self(), name);\n#endif')

# no Linux futex on the console: ANGLE's other SimpleMutex (std::mutex)
patch('src/common/SimpleMutex.h',
      '#if !defined(ANGLE_WITH_TSAN)\n#    if defined(ANGLE_PLATFORM_LINUX) || defined(ANGLE_PLATFORM_ANDROID)',
      '#if !defined(ANGLE_WITH_TSAN) && !defined(ANGLE_PLATFORM_PS5)\n#    if defined(ANGLE_PLATFORM_LINUX) || defined(ANGLE_PLATFORM_ANDROID)')

# Vulkan: RADV is linked into the title (no libvulkan.so to open); its entry point straight
patch('src/libANGLE/renderer/vulkan/vk_renderer.cpp',
      '#include "libANGLE/renderer/vulkan/vk_renderer.h"\n',
      '#include "libANGLE/renderer/vulkan/vk_renderer.h"\n#if defined(ANGLE_PLATFORM_PS5)\n'
      'extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);\n#endif\n')
patch('src/libANGLE/renderer/vulkan/vk_renderer.cpp',
      '''        mLibVulkanLibrary = angle::vk::OpenLibVulkan();
        ANGLE_VK_CHECK(context, mLibVulkanLibrary, VK_ERROR_INITIALIZATION_FAILED);

        PFN_vkGetInstanceProcAddr vulkanLoaderGetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                angle::GetLibrarySymbol(mLibVulkanLibrary, "vkGetInstanceProcAddr"));''',
      '''#if defined(ANGLE_PLATFORM_PS5)
        PFN_vkGetInstanceProcAddr vulkanLoaderGetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr);
#else
        mLibVulkanLibrary = angle::vk::OpenLibVulkan();
        ANGLE_VK_CHECK(context, mLibVulkanLibrary, VK_ERROR_INITIALIZATION_FAILED);

        PFN_vkGetInstanceProcAddr vulkanLoaderGetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                angle::GetLibrarySymbol(mLibVulkanLibrary, "vkGetInstanceProcAddr"));
#endif''')

# the display mode: 60 Hz and the window's width (1920) first, as Hydroid's own Vulkan path
# picks it (vk_ps5.c); ANGLE took the first mode, which may be 4K or 120 Hz on a PS5
patch('src/libANGLE/renderer/vulkan/linux/display/WindowSurfaceVkSimple.cpp',
      '    info.displayMode                   = modeProperties[0].displayMode;',
      '''    size_t best = 0;
    int bestScore = -1;
    for (size_t i = 0; i < modeProperties.size(); i++)
    {
        const VkDisplayModeParametersKHR &p = modeProperties[i].parameters;
        int hz    = static_cast<int>((p.refreshRate + 500) / 1000);
        int score = (hz == 60 ? 4 : 0) + (p.visibleRegion.width == displayWindow->width ? 2 : 0) +
                    (hz <= 60 ? 1 : 0);
        if (score > bestScore)
        {
            bestScore = score;
            best      = i;
        }
    }
    info.displayMode                   = modeProperties[best].displayMode;''')

# why a window surface can't be made (ANGLE's own warnings don't reach Hydroid's log)
patch('src/libANGLE/renderer/vulkan/SurfaceVk.cpp',
      '#include "libANGLE/renderer/vulkan/SurfaceVk.h"\n',
      '#include "libANGLE/renderer/vulkan/SurfaceVk.h"\n#if defined(ANGLE_PLATFORM_PS5)\n'
      'extern "C" void droid_log(const char *fmt, ...);\n#endif\n')
patch('src/libANGLE/renderer/vulkan/SurfaceVk.cpp',
      '''    ANGLE_TRY(mRenderer->checkQueueForSurfacePresent(displayVk, mSurface, &presentSupported));
    if (!presentSupported)
    {
        return angle::Result::Continue;''',
      '''    ANGLE_TRY(mRenderer->checkQueueForSurfacePresent(displayVk, mSurface, &presentSupported));
    if (!presentSupported)
    {
#if defined(ANGLE_PLATFORM_PS5)
        droid_log("angle: the queue can't present to this surface");
#endif
        return angle::Result::Continue;''')
patch('src/libANGLE/renderer/vulkan/SurfaceVk.cpp',
      '''    mSurfaceColorSpace = MapEglColorSpaceToVkColorSpace(renderer, eglColorSpaceEnum);
    return displayVk->isSurfaceFormatColorspacePairSupported(mSurface, vkFormat,
                                                             mSurfaceColorSpace);''',
      '''    mSurfaceColorSpace = MapEglColorSpaceToVkColorSpace(renderer, eglColorSpaceEnum);
#if defined(ANGLE_PLATFORM_PS5)
    {
        bool ok = displayVk->isSurfaceFormatColorspacePairSupported(mSurface, vkFormat, mSurfaceColorSpace);
        if (!ok)
        {
            uint32_t n = 0;
            VkPhysicalDevice pd = renderer->getPhysicalDevice();
            vkGetPhysicalDeviceSurfaceFormatsKHR(pd, mSurface, &n, nullptr);
            std::vector<VkSurfaceFormatKHR> f(n);
            vkGetPhysicalDeviceSurfaceFormatsKHR(pd, mSurface, &n, f.data());
            char list[512] = "";
            size_t at      = 0;
            for (uint32_t i = 0; i < n && at < sizeof(list) - 24; i++)
                at += snprintf(list + at, sizeof(list) - at, " %d/%d", f[i].format, f[i].colorSpace);
            droid_log("angle: surface format %d with colour space %d not offered; the surface has:%s", vkFormat,
                      mSurfaceColorSpace, list);
        }
        return ok;
    }
#endif
    return displayVk->isSurfaceFormatColorspacePairSupported(mSurface, vkFormat,
                                                             mSurfaceColorSpace);''')

# the PS5's display takes BGRA only (VK_FORMAT_B8G8R8A8_UNORM / _SRGB): configs of that format
# (ANGLE's RGBA8 / RGB8 ones were R8G8B8A8, "Bad match" at the window surface)
patch('src/libANGLE/renderer/vulkan/linux/display/DisplayVkSimple.cpp',
      '    constexpr GLenum kColorFormats[] = {GL_RGBA8, GL_BGRA8_EXT, GL_RGB565, GL_RGB8};',
      '#if defined(ANGLE_PLATFORM_PS5)\n    constexpr GLenum kColorFormats[] = {GL_BGRA8_EXT};\n#else\n'
      '    constexpr GLenum kColorFormats[] = {GL_RGBA8, GL_BGRA8_EXT, GL_RGB565, GL_RGB8};\n#endif')

# the picture's size: the window's (1920x1080), not the display mode's (a 4K-only TV: the game
# drew 1920x1080 into a quarter of a 4K swapchain); the PS5 scales the plane to the mode
patch('src/libANGLE/renderer/vulkan/linux/display/WindowSurfaceVkSimple.cpp',
      '    *extentsOut = gl::Extents(surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height, 1);',
      '''#if defined(ANGLE_PLATFORM_PS5)
    const angle::vk::SimpleDisplayWindow *window =
        reinterpret_cast<const angle::vk::SimpleDisplayWindow *>(mNativeWindowType);
    if (window && window->width && window->height)
    {
        *extentsOut = gl::Extents(window->width, window->height, 1);
        return angle::Result::Continue;
    }
#endif
    *extentsOut = gl::Extents(surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height, 1);''')
# (the display surface's size, in the log: on the PS5 its images are the mode's size, 4K on a 4K TV;
# gles_bridge.c scales the game's 1920x1080 viewport to it)
patch('src/libANGLE/renderer/vulkan/SurfaceVk.cpp',
      '''    if (surfaceCaps.currentExtent.width == kSurfaceSizedBySwapchain)
    {
        ASSERT(surfaceCaps.currentExtent.height == kSurfaceSizedBySwapchain);''',
      '''#if defined(ANGLE_PLATFORM_PS5)
    droid_log("angle: the display surface says %ux%u (images %ux%u to %ux%u)", surfaceCaps.currentExtent.width,
              surfaceCaps.currentExtent.height, surfaceCaps.minImageExtent.width, surfaceCaps.minImageExtent.height,
              surfaceCaps.maxImageExtent.width, surfaceCaps.maxImageExtent.height);
#endif
    if (surfaceCaps.currentExtent.width == kSurfaceSizedBySwapchain)
    {
        ASSERT(surfaceCaps.currentExtent.height == kSurfaceSizedBySwapchain);''')
