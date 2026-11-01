#ifndef COCOS_HOST_H
#define COCOS_HOST_H

#include "android_shim.h"
#include "soloader.h"

typedef void (*cocos_input_sink)(const droid_input_event *);
typedef void (*cocos_poll_fn)(cocos_input_sink sink);

// A cocos2d-x 2.x game library (exports Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender:
// Geometry Dash).
int cocos_detect(so_module *mod);

// Plays Cocos2dxActivity's Java side: the APK path, then a GL thread (as Cocos2dxGLSurfaceView's)
// calling nativeInit, then nativeRender every frame. poll feeds the DualSense in. Never returns
// unless the natives are missing.
void cocos_run(so_module *mod, const char *apk_path, cocos_poll_fn poll);

#endif
