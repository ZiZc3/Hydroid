// FalsoJNI's logger for the PS5 (replaces its Vita FalsoJNI_Logger.c):
// lines go to android.log through droid_log.

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "FalsoJNI.h"
#include "FalsoJNI_Logger.h"
#include "android_shim.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Set to N: the next N debug lines (every JNI call) are logged too.
int fjni_trace;

static void emit(const char *level, const char *fn, const char *fmt, va_list ap) {
    char text[1024];
    pthread_mutex_lock(&lock);
    vsnprintf(text, sizeof(text), fmt, ap);
    droid_log("[JNI %s] %s: %s", level, fn, text);
    pthread_mutex_unlock(&lock);
}

#define LOGGER(name, level, min)                                               \
    void name(const char *fi, int li, const char *fn, const char *fmt, ...) { \
        (void)fi; (void)li;                                                    \
        if (FALSOJNI_DEBUGLEVEL > (min)) {                                     \
            if ((min) != FALSOJNI_DEBUG_ALL || fjni_trace <= 0) return;        \
            fjni_trace--;                                                      \
        }                                                                      \
        va_list ap;                                                            \
        va_start(ap, fmt);                                                     \
        emit(level, fn, fmt, ap);                                              \
        va_end(ap);                                                            \
    }

LOGGER(_fjni_log_info, "info", FALSOJNI_DEBUG_INFO)
LOGGER(_fjni_log_warn, "warn", FALSOJNI_DEBUG_WARN)
LOGGER(_fjni_log_debug, "dbg", FALSOJNI_DEBUG_ALL)
LOGGER(_fjni_log_error, "error", FALSOJNI_DEBUG_ERROR)
