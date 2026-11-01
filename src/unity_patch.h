#ifndef UNITY_PATCH_H
#define UNITY_PATCH_H

// Vulkan first for a Unity game (unity_patch.c): copy is the game's APK with Vulkan at the
// head of its graphics API list. 0: the copy is ready (use it); 1: nothing to change
// (Vulkan first already, no Vulkan, not Unity); -1: failed (use the APK as it is).
int unity_vulkan_first(const char *apk, const char *copy);

// Without a copy (the usual case): 0 = Vulkan first as Unity unpacks the list (its
// globalgamemanagers is deflated; unity_inflate does it); 2 = it's stored: use
// unity_vulkan_first; 1 = nothing to change; -1 = can't read the APK.
int unity_vulkan_plan(const char *apk);
int unity_inflate(void *strm, int flush); // zlib's inflate (a z_stream *) for the game's code

// The phone view's Auto: 1 = the Unity game runs portrait only (PlayerSettings), 0 = landscape
// allowed, -1 = not known / not Unity
int unity_portrait_only(const char *apk);

#endif
