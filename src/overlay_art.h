#ifndef OVERLAY_ART_H
#define OVERLAY_ART_H

#include <stdint.h>

// What the FPS counter and the cursor are made of, for the GL bridge and the Vulkan
// bridge alike (overlay_art.c).

// 5x7 glyphs: 0-9, then F, P, S (bit 4 = the left column)
extern const uint8_t overlay_font5x7[13][7];
#define OVERLAY_GLYPH_F 10
#define OVERLAY_GLYPH_P 11
#define OVERLAY_GLYPH_S 12

// The counter's colour for an fps (XPSemu's: green fast, yellow ok, orange slow, red very slow)
void overlay_fps_color(int fps, float rgb[3]);

// Ruffle Flash PS5's pointer at a scale, into w x h straight-alpha RGBA floats (cleared
// first), its tip at (tip, tip).
void overlay_cursor_art(float *rgba, int w, int h, float tip, float scale);

#endif
