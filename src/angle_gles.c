// angle_gles.c — ANGLE (Google's OpenGL ES on Vulkan, BSD-3-Clause) for the games that draw with
// GLES: the PS5 build links third_party/build-angle-ps5.py's libangle_ps5.a, RADV under it. Its
// exported gl* / egl* names are left out of that archive (ps5-opengl has the same names); every
// function comes from ANGLE's own EGL_GetProcAddress, by the usual name.

#include <stddef.h>
#include <string.h>

#include "android_shim.h"

#ifdef DROID_ANGLE
void *EGL_GetProcAddress(const char *name); // ANGLE's entry point (entry_points_egl_autogen.cpp)

void *angle_resolve(const char *name) {
    if (!name || (strncmp(name, "gl", 2) != 0 && strncmp(name, "egl", 3) != 0)) return NULL;
    void *p = EGL_GetProcAddress(name);
    static int told;
    if (!p && told++ < 40) droid_log("angle: %s: not in ANGLE", name);
    return p;
}
int angle_available(void) { return 1; }
#else
void *angle_resolve(const char *name) { (void)name; return NULL; }
int angle_available(void) { return 0; }
#endif
