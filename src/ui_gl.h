#ifndef UI_GL_H
#define UI_GL_H

#include <stdint.h>

// The menus' drawing: GPU quads on its own EGL window (ps5-opengl on the PS5, Mesa
// GLES in the PC test runner), 1920x1080. Shapes are signed-distance rounded rectangles,
// so corners and glows stay smooth at any size.

#define UI_W 1920
#define UI_H 1080

typedef struct { float r, g, b, a; } ui_color;
#define UI_RGBA(r, g, b, a) ((ui_color){ (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, (a) })

int ui_open(void);   // the window and the GL context; 0 = ok
void ui_close(void); // all of it gone, and the screen let go (eglTerminate) for the game
void ui_frame_begin(void);
void ui_frame_end(void); // shows the frame (waits for the screen's refresh)
void ui_shot(const char *name); // PC tests: the frame so far, as a .ppm file (the console: nothing)

// A rounded rectangle: radius of the corners, feather = how soft its edge is (a glow:
// a big feather). col2 / vertical: a gradient from col to col2.
void ui_rect(float x, float y, float w, float h, float radius, float feather, ui_color col);
void ui_rect_gradient(float x, float y, float w, float h, float radius, ui_color top, ui_color bottom);
// A ring (a circle's outline) around (cx, cy), radius r to its outside, soft by feather.
void ui_ring(float cx, float cy, float r, float thickness, float feather, ui_color col);
// A quad with any four corners (the background's slanted shapes), flat colour.
void ui_quad(const float xy[8], ui_color col);

// A 3D turn for what's drawn next: about the screen point (cx, cy), rot_x (tips back, radians),
// rot_y (swings sideways), then moved z pixels away and scaled; text and images turn with it.
// ui_transform_reset: flat again. (The menus' 3D opening.)
void ui_transform(float cx, float cy, float rot_x, float rot_y, float z, float scale);
void ui_transform_reset(void);

// Drawing after ui_clip shows only between rows y0 and y1 of the screen (a scissor); ui_clip_off: all of it
void ui_clip(float y0, float y1);
void ui_clip_off(void);

// Textures: RGBA8 images (icons), R8 masks (the font's atlas)
unsigned ui_texture_rgba(const uint8_t *rgba, int w, int h);
unsigned ui_texture_mask(const uint8_t *mask, int w, int h);
void ui_texture_free(unsigned tex);
// An image in a rounded rectangle, faded by alpha
void ui_image(unsigned tex, float x, float y, float w, float h, float radius, float alpha);
// the same with a soft band of light across it, corner to corner: at -0.3 .. 1.3 along the
// diagonal (top left to bottom right), strength 0-1 (a focused icon's shine)
void ui_image_shine(unsigned tex, float x, float y, float w, float h, float radius, float alpha, float at,
                    float strength);
// A part of a mask texture (u0,v0)-(u1,v1), tinted: one glyph
void ui_mask(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, ui_color col);

#endif
