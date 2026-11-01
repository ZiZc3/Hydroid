#ifndef MAPPER_H
#define MAPPER_H

// The control mapper's markers (unity_host.c makes them, the overlays draw them): squares
// in the game's 1920x1080, PlayStation colours (Cross blue, Circle red, Triangle green,
// Square pink, shoulders white, the joystick orange). Shown while the mapper is open.
typedef struct {
    float x, y; // the square's centre
    int size;   // its side, px
    float rgb[3];
} droid_marker;

#define DROID_MARKERS 40
extern droid_marker droid_markers[DROID_MARKERS];
extern volatile int droid_marker_count;
extern volatile int droid_mapper_on; // the mapper is open: markers shown

#endif
