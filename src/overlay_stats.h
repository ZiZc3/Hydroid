#ifndef OVERLAY_STATS_H
#define OVERLAY_STATS_H

#include <stdint.h>

// The in-game stats panel (a game's Options > Graphics > Stats overlay): drawn here on the CPU
// into a small RGBA image (Outfit, dark green glass), shown on the game's frame by gl_overlay.inc
// (ps5-opengl, ANGLE: blended) and vulkan_bridge.c (copied, its background solid).

enum { STATS_OFF = 0, STATS_FPS = 1, STATS_FULL = 2 }; // settings.fps_counter

// once a shown frame (each bridge's present / swap): the frame times and the fps
void overlay_stats_tick(void);

// The panel for this frame at a scale (screen pixels per 1080p pixel), or NULL (off, or nothing
// to show yet). premul: premultiplied alpha (GL blends it); else its colours on the panel's own
// background with alpha 0 / 255 for its rounded corners (Vulkan copies the solid part).
// *version changes when the picture did (a new upload only then).
const uint8_t *overlay_stats_image(int mode, float scale, int premul, int *w, int *h, unsigned *version);

#endif
