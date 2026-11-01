// droid_game.c — Phase 2 Android test game, built with the NDK.
// Uses the real NDK APIs a native game uses: <android/input.h> for the
// gamepad and AAudio for sound. The PS5 host answers those calls.

#include <stdint.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <aaudio/AAudio.h>

typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t frame;
    uint64_t (*tile_offset)(uint32_t x, uint32_t y);
} droid_surface;

enum { B_A, B_B, B_X, B_Y, B_L1, B_R1, B_START, B_SELECT, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_COUNT };

static volatile uint32_t held;
static volatile float stick_x, stick_y, right_y, l2, r2;
static volatile int last_button = -1;
static AAudioStream *stream;
static int audio_state = -100;
static float phase;

static int button_of(int32_t keycode) {
    switch (keycode) {
    case AKEYCODE_BUTTON_A: return B_A;
    case AKEYCODE_BUTTON_B: return B_B;
    case AKEYCODE_BUTTON_X: return B_X;
    case AKEYCODE_BUTTON_Y: return B_Y;
    case AKEYCODE_BUTTON_L1: return B_L1;
    case AKEYCODE_BUTTON_R1: return B_R1;
    case AKEYCODE_BUTTON_START: return B_START;
    case AKEYCODE_BUTTON_SELECT: return B_SELECT;
    case AKEYCODE_DPAD_UP: return B_UP;
    case AKEYCODE_DPAD_DOWN: return B_DOWN;
    case AKEYCODE_DPAD_LEFT: return B_LEFT;
    case AKEYCODE_DPAD_RIGHT: return B_RIGHT;
    }
    return -1;
}

// Parabolic sine on [-pi, pi]: no libm import needed.
static float fast_sin(float x) {
    const float pi = 3.14159265f;
    while (x > pi) x -= 2 * pi;
    while (x < -pi) x += 2 * pi;
    float y = 1.27323954f * x - 0.405284735f * x * (x < 0 ? -x : x);
    return 0.225f * (y * (y < 0 ? -y : y) - y) + y;
}

static aaudio_data_callback_result_t on_audio(AAudioStream *s, void *user, void *data,
                                              int32_t frames) {
    (void)s; (void)user;
    float *out = data;
    // Tone while A (Cross) is held; pitch follows the right stick, volume R2.
    int on = (held >> B_A) & 1;
    float freq = 440.0f * (1.0f - right_y * 0.5f);
    float volume = on ? 0.15f + 0.35f * r2 : 0.0f;
    float step = 2 * 3.14159265f * freq / 48000.0f;
    for (int32_t i = 0; i < frames; i++) {
        float v = fast_sin(phase) * volume;
        phase += step;
        if (phase > 3.14159265f) phase -= 2 * 3.14159265f;
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

__attribute__((visibility("default")))
int game_init(void) {
    AAudioStreamBuilder *b = NULL;
    audio_state = AAudio_createStreamBuilder(&b);
    if (audio_state != AAUDIO_OK) return audio_state;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, on_audio, NULL);
    audio_state = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (audio_state != AAUDIO_OK) return audio_state;
    audio_state = AAudioStream_requestStart(stream);
    return audio_state;
}

// The NativeActivity-style input callback: 1 = handled.
__attribute__((visibility("default")))
int32_t game_on_input(const AInputEvent *e) {
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int b = button_of(AKeyEvent_getKeyCode(e));
        if (b < 0) return 0;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) {
            held |= 1u << b;
            last_button = b;
        } else {
            held &= ~(1u << b);
        }
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(e) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        stick_x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
        stick_y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        right_y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        l2 = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
        r2 = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        return 1;
    }
    return 0;
}

static void fill(droid_surface *s, int rx, int ry, int rw, int rh, uint32_t c) {
    for (int y = ry < 0 ? 0 : ry; y < ry + rh && y < (int)s->height; y++)
        for (int x = rx < 0 ? 0 : rx; x < rx + rw && x < (int)s->width; x++)
            *(uint32_t *)((uint8_t *)s->pixels + s->tile_offset((uint32_t)x, (uint32_t)y)) = c;
}

static const uint32_t button_colors[B_COUNT] = {
    0xFFFF9050, 0xFF3030FF, 0xFFFF60D0, 0xFF40E040, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFF00D0FF, 0xFF00D0FF, 0xFFC0C0C0, 0xFFC0C0C0, 0xFFC0C0C0, 0xFFC0C0C0,
};

void jni_draw_results(droid_surface *s);

static float px = 860, py = 465;

__attribute__((visibility("default")))
void droid_draw_frame(droid_surface *s) {
    // Move with the left stick or the D-pad; L2 = slow, nothing = normal.
    float speed = 14.0f * (1.0f - 0.7f * l2);
    float dx = stick_x, dy = stick_y;
    if ((held >> B_LEFT) & 1) dx = -1;
    if ((held >> B_RIGHT) & 1) dx = 1;
    if ((held >> B_UP) & 1) dy = -1;
    if ((held >> B_DOWN) & 1) dy = 1;
    px += dx * speed;
    py += dy * speed;
    if (px < 0) px = 0;
    if (py < 84) py = 84;
    if (px > s->width - 200) px = s->width - 200;
    if (py > s->height - 150) py = s->height - 150;

    fill(s, 0, 0, (int)s->width, (int)s->height, 0xFF2E1A1A);
    uint32_t box = last_button >= 0 ? button_colors[last_button] : 0xFF0CC616;
    fill(s, (int)px, (int)py, 200, 150, box);

    // One square per button across the top: lit while held.
    for (int b = 0; b < B_COUNT; b++) {
        uint32_t c = ((held >> b) & 1) ? button_colors[b] : 0xFF404040;
        fill(s, 40 + b * 70, 0, 56, 30, c);
    }
    // Audio status light: green = AAudio stream running, red = failed.
    fill(s, (int)s->width - 70, 0, 56, 30, audio_state == AAUDIO_OK ? 0xFF00FF00 : 0xFF0000FF);
    jni_draw_results(s);
}

// ---- Phase 3: Java through JNI, as a real game talks to its Activity -------

#include <jni.h>
#include <string.h>
#include <android/log.h>

enum { T_STATIC_STRING, T_OBJECT_METHOD, T_STATIC_FIELD, T_INT_ARGS, T_BYTE_ARRAY,
       T_NEW_STRING, T_BOOLEAN, T_FLOAT, T_COUNT };
static volatile uint32_t jni_passed;
static volatile int jni_ran;

static void check(int test, int ok, const char *what) {
    if (ok) jni_passed |= 1u << test;
    __android_log_print(ANDROID_LOG_INFO, "droid", "JNI test %d %s: %s", test, what,
                        ok ? "PASS" : "FAIL");
}

static jint native_init(JNIEnv *env, jclass game, jobject activity) {
    jni_ran = 1;

    jmethodID m = (*env)->GetStaticMethodID(env, game, "getDeviceName", "()Ljava/lang/String;");
    jstring name = (jstring)(*env)->CallStaticObjectMethod(env, game, m);
    const char *text = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    check(T_STATIC_STRING, text && strcmp(text, "PlayStation 5") == 0, "static String method");
    if (text) {
        __android_log_print(ANDROID_LOG_INFO, "droid", "device name: %s", text);
        (*env)->ReleaseStringUTFChars(env, name, text);
    }

    jclass activity_class = (*env)->GetObjectClass(env, activity);
    m = (*env)->GetMethodID(env, activity_class, "getFilesDir", "()Ljava/lang/String;");
    jstring dir = (jstring)(*env)->CallObjectMethod(env, activity, m);
    text = dir ? (*env)->GetStringUTFChars(env, dir, NULL) : NULL;
    check(T_OBJECT_METHOD, text && text[0] == '/', "Activity.getFilesDir");
    if (text) (*env)->ReleaseStringUTFChars(env, dir, text);

    jclass version = (*env)->FindClass(env, "android/os/Build$VERSION");
    jfieldID f = (*env)->GetStaticFieldID(env, version, "SDK_INT", "I");
    jint sdk = (*env)->GetStaticIntField(env, version, f);
    check(T_STATIC_FIELD, sdk == 30, "Build.VERSION.SDK_INT");

    m = (*env)->GetStaticMethodID(env, game, "addNumbers", "(II)I");
    check(T_INT_ARGS, (*env)->CallStaticIntMethod(env, game, m, 40, 2) == 42, "int args");

    jbyteArray bytes = (*env)->NewByteArray(env, 16);
    jbyte fill[16];
    for (int i = 0; i < 16; i++) fill[i] = (jbyte)i;
    (*env)->SetByteArrayRegion(env, bytes, 0, 16, fill);
    jbyte *got = (*env)->GetByteArrayElements(env, bytes, NULL);
    int sum = 0;
    for (int i = 0; got && i < 16; i++) sum += got[i];
    check(T_BYTE_ARRAY, (*env)->GetArrayLength(env, bytes) == 16 && sum == 120, "byte[]");
    if (got) (*env)->ReleaseByteArrayElements(env, bytes, got, 0);

    jstring hello = (*env)->NewStringUTF(env, "hello");
    check(T_NEW_STRING, (*env)->GetStringUTFLength(env, hello) == 5, "NewStringUTF");

    m = (*env)->GetStaticMethodID(env, game, "isTV", "()Z");
    check(T_BOOLEAN, (*env)->CallStaticBooleanMethod(env, game, m) == JNI_TRUE, "boolean");

    m = (*env)->GetStaticMethodID(env, game, "getDensity", "()F");
    float density = (*env)->CallStaticFloatMethod(env, game, m);
    check(T_FLOAT, density > 1.99f && density < 2.01f, "float");

    m = (*env)->GetStaticMethodID(env, game, "vibrate", "(J)V");
    (*env)->CallStaticVoidMethod(env, game, m, (jlong)250);
    return (jint)jni_passed;
}

__attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_ERR;
    jclass game = (*env)->FindClass(env, "com/ps5/android/Game");
    static const JNINativeMethod methods[] = {
        { "nativeInit", "(Landroid/app/Activity;)I", (void *)native_init },
    };
    if ((*env)->RegisterNatives(env, game, methods, 1) != JNI_OK) return JNI_ERR;
    __android_log_write(ANDROID_LOG_INFO, "droid", "JNI_OnLoad: natives registered");
    return JNI_VERSION_1_6;
}

__attribute__((visibility("default")))
void jni_draw_results(droid_surface *s) {
    // A second row: one square per JNI test, green = pass, red = fail.
    for (int t = 0; t < T_COUNT; t++) {
        uint32_t c = !jni_ran ? 0xFF404040 : ((jni_passed >> t) & 1) ? 0xFF00FF00 : 0xFF0000FF;
        fill(s, 40 + t * 70, 44, 56, 30, c);
    }
}
