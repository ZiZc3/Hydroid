// Android Phase 0b: load an Android x86_64 .so with our own ELF loader.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "soloader.h"

int sceKernelSendNotificationRequest(int device, const void *buf,
                                     int size, int blocking);

struct notification_request {
    int type;
    int req_id;
    int priority;
    int msg_id;
    int target_id;
    int user_id;
    int unk1;
    int unk2;
    int app_id;
    int error_num;
    int unk3;
    char use_icon_image_uri;
    char message[1024];
    char uri[1024];
    char unk[1024];
};

static void notify(const char *msg) {
    struct notification_request noti;
    memset(&noti, 0, sizeof(noti));
    noti.type = 0;
    noti.target_id = -1;
    strncpy(noti.message, msg, sizeof(noti.message) - 1);
    sceKernelSendNotificationRequest(0, &noti, sizeof(noti), 0);
}

static int try_so(const char *path, const char *label) {
    char buf[256];

    snprintf(buf, sizeof(buf), "Android: loading %s ...", label);
    notify(buf);

    so_module mod;
    int rc = so_load(path, &mod);
    if (rc != 0) {
        snprintf(buf, sizeof(buf), "Android: %s load FAIL rc=%d", label, rc);
        notify(buf);
        return 1;
    }

    snprintf(buf, sizeof(buf), "Android: %s mapped at %p (%llu KB)",
             label, mod.base, (unsigned long long)(mod.size / 1024));
    notify(buf);

    typedef int32_t (*hello_fn)(int32_t, int32_t);
    hello_fn hello = (hello_fn)so_sym(&mod, "droid_hello");
    if (!hello) {
        snprintf(buf, sizeof(buf), "Android: %s sym not found", label);
        notify(buf);
        so_unload(&mod);
        return 2;
    }

    snprintf(buf, sizeof(buf), "Android: %s droid_hello at %p", label, (void *)hello);
    notify(buf);

    int32_t result = hello(40, 2);
    snprintf(buf, sizeof(buf), "Android: %s droid_hello(40,2) = %d", label, result);
    notify(buf);

    typedef const char *(*version_fn)(void);
    version_fn ver = (version_fn)so_sym(&mod, "droid_version");
    if (ver) {
        snprintf(buf, sizeof(buf), "Android: %s ver: %s", label, ver());
        notify(buf);
    }

    so_unload(&mod);
    return 0;
}

int main(void) {
    notify("Android: Phase 0b — own ELF loader");

    int ok = 0;
    ok += (try_so("/data/android/libtest_ndk_nostdlib.so", "NDK") == 0);

    char buf[128];
    if (ok > 0) {
        snprintf(buf, sizeof(buf), "Android: SUCCESS — Android .so ran on PS5!");
        notify(buf);
    } else {
        snprintf(buf, sizeof(buf), "Android: FAIL — check klog");
        notify(buf);
    }
    return ok > 0 ? 0 : 1;
}
