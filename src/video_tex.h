// video_tex.h — a video's picture for the game's GL: Android's SurfaceTexture, ours
// (java_layer.c: the Java class; mediandk.c: the decoder's output; video_tex.c: the GL side).
#ifndef VIDEO_TEX_H
#define VIDEO_TEX_H

#include <stdint.h>

#include "native_activity.h"

// The games that play their videos for real (mediandk.c); every other one: they end at once
#define VIDEO_GAMES "com.nekki.shadowfightarena"
int video_game(void);

typedef struct video_surface video_surface;
video_surface *vsurf_new(unsigned tex);
// the newest decoded picture (NV12), from the decoder's thread; then onFrameAvailable
void vsurf_frame_nv12(video_surface *s, const uint8_t *nv12, int w, int h, int64_t pts_us);
void vsurf_set_listener(video_surface *s, void *listener, void *self);
// SurfaceTexture.updateTexImage, on the game's GL thread: the picture into its texture
void vsurf_update(video_surface *s);
int64_t vsurf_timestamp_ns(video_surface *s);

// the Surface made from a SurfaceTexture, as an ANativeWindow (droid_window first: the
// ANativeWindow_* functions read it as one)
typedef struct {
    droid_window base;
    uint32_t magic;
    video_surface *vs;
} video_window;
#define VIDEO_WINDOW_MAGIC 0x56494457u
video_window *video_window_of(void *window); // NULL: not one of these

// the game's GL names, as they're resolved: external (video) textures become 2D textures
// and the shaders' samplerExternalOES a sampler2D (desktop GL and Mesa's ES take no upload
// into an external texture). Video games only.
void *video_gl_wrap(const char *name, void *real);

#endif
