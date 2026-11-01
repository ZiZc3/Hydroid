#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#define DISPLAY_WIDTH 1920
#define DISPLAY_HEIGHT 1080

// The screen first, before other setup: 0x80290009 (busy) means another
// process still owns it. Returns the VideoOut handle or an error.
int display_open(int *busy_tries);
int display_setup(void);
uint64_t display_tile_offset(uint32_t x, uint32_t y);
uint32_t *display_buffer(int index);
void display_fill(int index, uint32_t color);
void display_flip(int index, int64_t arg);

#endif
