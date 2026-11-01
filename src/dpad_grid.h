#ifndef DPAD_GRID_H
#define DPAD_GRID_H

#include <stdint.h>

// The D-pad on a board of tiles (dpad_grid.c): a host asks, a swap reads the picture around
// the cursor, the tiles' pitch and a centre come back. Coordinates: the 1920x1080 screen.
typedef struct {
    int ok;                   // a board was found
    double at;                // when (seconds, CLOCK_MONOTONIC)
    float pitch_x, pitch_y;   // a tile's size
    float cx, cy;             // a tile's centre
    float x0, y0, x1, y1;     // the region it was found in
} dpad_grid;

void dpad_grid_request(float cx, float cy);  // the host: find the board around here, at the next frame
int dpad_grid_wanted(float *cx, float *cy);  // the swap: was it asked? (taken)
// the swap: the region read (RGBA, w x h), its top left on the screen (x0, y0) and the screen's
// pixels per region pixel (unit: 1 at 1080p, 0.5 for a 4K surface)
void dpad_grid_analyze(const uint8_t *px, int w, int h, int bottom_up, float x0, float y0, float unit);
int dpad_grid_get(dpad_grid *g);             // the newest answer (0: none yet)

#endif
