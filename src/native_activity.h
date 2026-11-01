#ifndef NATIVE_ACTIVITY_H
#define NATIVE_ACTIVITY_H

#include <stddef.h>
#include <stdint.h>

#include "android_shim.h"

// Our ANativeWindow: the game only passes it to EGL and ANativeWindow_*.
typedef struct {
    int32_t width, height;
} droid_window;

typedef struct droid_activity droid_activity;

// ANativeActivityCallbacks, in the NDK's order.
typedef struct {
    void (*on_start)(droid_activity *);
    void (*on_resume)(droid_activity *);
    void *(*on_save_instance_state)(droid_activity *, size_t *);
    void (*on_pause)(droid_activity *);
    void (*on_stop)(droid_activity *);
    void (*on_destroy)(droid_activity *);
    void (*on_window_focus_changed)(droid_activity *, int);
    void (*on_native_window_created)(droid_activity *, void *);
    void (*on_native_window_resized)(droid_activity *, void *);
    void (*on_native_window_redraw_needed)(droid_activity *, void *);
    void (*on_native_window_destroyed)(droid_activity *, void *);
    void (*on_input_queue_created)(droid_activity *, void *);
    void (*on_input_queue_destroyed)(droid_activity *, void *);
    void (*on_content_rect_changed)(droid_activity *, const void *);
    void (*on_configuration_changed)(droid_activity *);
    void (*on_low_memory)(droid_activity *);
} droid_activity_callbacks;

// ANativeActivity, in the NDK's layout.
struct droid_activity {
    droid_activity_callbacks *callbacks;
    void *vm;
    void *env;
    void *clazz;
    const char *internal_data_path;
    const char *external_data_path;
    int32_t sdk_version;
    void *instance;
    void *asset_manager;
    const char *obb_path;
};

// Calls the game's ANativeActivity_onCreate, then starts, resumes and gives
// it the window and the input queue, as Android does.
int activity_start(void *on_create, void *vm, void *env, void *clazz);
void activity_push_input(const droid_input_event *e);
droid_window *activity_window(void);
void *activity_resolve(const char *name);
// The folder holding the game's OBB (main.<version>.<package>.obb), for activity->obbPath.
void activity_set_obb(const char *dir);

#endif
