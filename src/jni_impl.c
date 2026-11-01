// jni_impl.c — what our fake Java answers (FalsoJNI's method and field
// tables), plus RegisterNatives, which FalsoJNI ignores but games rely on.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "FalsoJNI.h"
#include "FalsoJNI_Impl.h"
#include "game_settings.h"
#include "android_shim.h"
#include "jni_impl.h"
#include "java_layer.h"

// ---- Java methods ----------------------------------------------------------

enum {
    M_GET_DEVICE_NAME = 1,
    M_GET_FILES_DIR,
    M_ADD_NUMBERS,
    M_IS_TV,
    M_GET_DENSITY,
    M_VIBRATE,
    // Godot 3 (org.godotengine.godot.Godot / GodotIO)
    M_G_STRING_EMPTY, M_G_OBJECT, M_G_NULL, M_G_DATA_DIR, M_G_LOCALE, M_G_MODEL, M_G_SAFE_AREA,
    M_G_UNIQUE_ID, M_G_SYSTEM_DIR, M_G_GLES_VERSION, M_G_ZERO, M_G_DPI, M_G_TRUE, M_G_FALSE,
    M_G_IGNORED, M_G_ALERT, M_G_FORCE_QUIT, M_G_RESTART,
    // Android Context and friends (Unity)
    M_U_OBJ, M_U_FILES, M_U_CACHE, M_U_OBB, M_U_DATA, M_U_PATH, M_U_PACKAGE, M_U_APK,
    M_U_LANGUAGE, M_U_COUNTRY,
};

static jobject getDeviceName(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "PlayStation 5");
}

static jobject getFilesDir(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, game_data_dir);
}

static jint addNumbers(jmethodID id, va_list args) {
    (void)id;
    jint a = va_arg(args, jint);
    jint b = va_arg(args, jint);
    return a + b;
}

static jboolean isTV(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_TRUE;
}

static jfloat getDensity(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 2.0f;
}

static void vibrate(jmethodID id, va_list args) {
    (void)id;
    jlong ms = va_arg(args, jlong);
    droid_log("java: vibrate(%lld ms)", (long long)ms);
}

// ---- Godot 3's Java side -----------------------------------------------------

static jobject g_string_empty(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, ""); }
static jobject g_object(jmethodID id, va_list a) { (void)id; (void)a; return (jobject)strdup("java/lang/Object"); }
static jobject g_null(jmethodID id, va_list a) { (void)id; (void)a; return NULL; }
static jobject g_data_dir(jmethodID id, va_list a) { (void)id; (void)a; char p[300]; return jni->NewStringUTF(&jni, data_path(p, sizeof(p), "godot")); }
static jobject g_locale(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, "en_US"); }
static jobject g_model(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, "PlayStation 5"); }
static jobject g_unique_id(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, "PS5"); }
static jobject g_system_dir(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, game_dir); }
static jobject g_safe_area(jmethodID id, va_list a) {
    (void)id; (void)a;
    jintArray arr = jni->NewIntArray(&jni, 4);
    const jint xywh[4] = { 0, 0, droid_game_w, droid_game_h };
    jni->SetIntArrayRegion(&jni, arr, 0, 4, xywh);
    return arr;
}
static jint g_gles_version(jmethodID id, va_list a) { (void)id; (void)a; return 0x00030000; }
static jint g_zero(jmethodID id, va_list a) { (void)id; (void)a; return 0; }
static jint g_dpi(jmethodID id, va_list a) { (void)id; (void)a; return 160; }
static jboolean g_true(jmethodID id, va_list a) { (void)id; (void)a; return JNI_TRUE; }
static jboolean g_false(jmethodID id, va_list a) { (void)id; (void)a; return JNI_FALSE; }
static void g_ignored(jmethodID id, va_list a) { (void)id; (void)a; }
static void g_alert(jmethodID id, va_list a) {
    (void)id;
    const char *text = java_text(va_arg(a, jobject));
    const char *title = java_text(va_arg(a, jobject));
    droid_log("godot alert: %s: %s", title ? title : "", text ? text : "");
}
static void g_force_quit(jmethodID id, va_list a) {
    (void)id; (void)a;
    droid_log("godot: forceQuit");
    exit(0);
}
static void g_restart(jmethodID id, va_list a) {
    (void)id; (void)a;
    droid_log("godot: restart asked (GL context lost?)");
}

// ---- an Android Context for engines like Unity ------------------------------------
// Fake objects are strings: "file:<path>" for java.io.File, else a class name.
// fjni_this (patched into FalsoJNI) says which object a method is called on.

const char *java_package = "com.ps5.game";
int droid_java_version_code; // the GameActivity.VersionCode field (UE4 reads it)
const char *droid_java_version_name = "";
const char *java_apk_path = APP0 "/game.apk";
const char *java_lib_dir = APP0 "/data/apk-lib";

static char obj_generic[] = "java/lang/Object";
static jobject file_obj(const char *path) {
    char buf[300];
    snprintf(buf, sizeof(buf), "file:%s", path);
    mkdir(path, 0777);
    return (jobject)strdup(buf);
}
static const char *this_file(void) {
    const char *t = (const char *)fjni_this;
    return t && strncmp(t, "file:", 5) == 0 ? t + 5 : NULL;
}
static jobject u_obj(jmethodID id, va_list a) { (void)id; (void)a; return (jobject)obj_generic; }
static jobject u_files_dir(jmethodID id, va_list a) { (void)id; (void)a; char p[300]; return file_obj(data_path(p, sizeof(p), "files")); }
static jobject u_cache_dir(jmethodID id, va_list a) { (void)id; (void)a; char p[300]; return file_obj(data_path(p, sizeof(p), "cache")); }
static jobject u_obb_dir(jmethodID id, va_list a) { (void)id; (void)a; char p[300]; return file_obj(game_path(p, sizeof(p), "obb")); }
static jobject u_data_dir(jmethodID id, va_list a) { (void)id; (void)a; return file_obj(game_dir); }
static jobject u_path(jmethodID id, va_list a) {
    (void)id; (void)a;
    const char *p = this_file();
    return jni->NewStringUTF(&jni, p ? p : game_dir);
}
static jobject u_package(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, java_package); }
static jobject u_apk_path(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, java_apk_path); }
static jobject u_language(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, "en"); }
static jobject u_country(jmethodID id, va_list a) { (void)id; (void)a; return jni->NewStringUTF(&jni, "US"); }

NameToMethodID nameToMethodId[] = {
    { M_GET_DEVICE_NAME, "getDeviceName", METHOD_TYPE_OBJECT },
    { M_GET_FILES_DIR, "getFilesDir", METHOD_TYPE_OBJECT },
    { M_ADD_NUMBERS, "addNumbers", METHOD_TYPE_INT },
    { M_IS_TV, "isTV", METHOD_TYPE_BOOLEAN },
    { M_GET_DENSITY, "getDensity", METHOD_TYPE_FLOAT },
    { M_VIBRATE, "vibrate", METHOD_TYPE_VOID },
    { M_G_STRING_EMPTY, "getClipboard", METHOD_TYPE_OBJECT },
    { M_G_STRING_EMPTY, "getInputFallbackMapping", METHOD_TYPE_OBJECT },
    { M_G_NULL, "getGrantedPermissions", METHOD_TYPE_OBJECT },
    { M_G_OBJECT, "getSurface", METHOD_TYPE_OBJECT },
    { M_G_OBJECT, "getClassLoader", METHOD_TYPE_OBJECT },
    { M_G_DATA_DIR, "getDataDir", METHOD_TYPE_OBJECT },
    { M_G_LOCALE, "getLocale", METHOD_TYPE_OBJECT },
    { M_G_MODEL, "getModel", METHOD_TYPE_OBJECT },
    { M_G_SAFE_AREA, "getWindowSafeArea", METHOD_TYPE_OBJECT },
    { M_G_UNIQUE_ID, "getUniqueID", METHOD_TYPE_OBJECT },
    { M_G_SYSTEM_DIR, "getSystemDir", METHOD_TYPE_OBJECT },
    { M_G_GLES_VERSION, "getGLESVersionCode", METHOD_TYPE_INT },
    { M_G_ZERO, "openURI", METHOD_TYPE_INT },
    { M_G_DPI, "getScreenDPI", METHOD_TYPE_INT },
    { M_G_ZERO, "getScreenOrientation", METHOD_TYPE_INT },
    { M_G_TRUE, "requestPermission", METHOD_TYPE_BOOLEAN },
    { M_G_TRUE, "requestPermissions", METHOD_TYPE_BOOLEAN },
    { M_G_TRUE, "isActivityResumed", METHOD_TYPE_BOOLEAN },
    { M_G_FALSE, "isVideoPlaying", METHOD_TYPE_BOOLEAN },
    { M_G_IGNORED, "onVideoInit", METHOD_TYPE_VOID },
    { M_G_IGNORED, "setKeepScreenOn", METHOD_TYPE_VOID },
    { M_G_IGNORED, "setClipboard", METHOD_TYPE_VOID },
    { M_G_IGNORED, "initInputDevices", METHOD_TYPE_VOID },
    { M_G_IGNORED, "onGodotSetupCompleted", METHOD_TYPE_VOID },
    { M_G_IGNORED, "onGodotMainLoopStarted", METHOD_TYPE_VOID },
    { M_G_IGNORED, "showKeyboard", METHOD_TYPE_VOID },
    { M_G_IGNORED, "hideKeyboard", METHOD_TYPE_VOID },
    { M_G_IGNORED, "setScreenOrientation", METHOD_TYPE_VOID },
    { M_G_IGNORED, "playVideo", METHOD_TYPE_VOID },
    { M_G_IGNORED, "pauseVideo", METHOD_TYPE_VOID },
    { M_G_IGNORED, "stopVideo", METHOD_TYPE_VOID },
    { M_G_ALERT, "alert", METHOD_TYPE_VOID },
    { M_G_FORCE_QUIT, "forceQuit", METHOD_TYPE_VOID },
    { M_G_RESTART, "restart", METHOD_TYPE_VOID },
    // Context / Activity / File / PackageManager / Locale (Unity and others)
    { M_U_OBJ, "getApplicationContext", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getPackageManager", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getApplicationInfo", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getPackageInfo", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getAssets", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getResources", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getDisplayMetrics", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getConfiguration", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getContentResolver", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getSystemService", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getWindowManager", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getDefaultDisplay", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getWindow", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getDecorView", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getIntent", METHOD_TYPE_OBJECT },
    { M_U_OBJ, "getDefault", METHOD_TYPE_OBJECT },
    { M_U_FILES, "getFilesDir", METHOD_TYPE_OBJECT },
    { M_U_FILES, "getExternalFilesDir", METHOD_TYPE_OBJECT },
    { M_U_CACHE, "getCacheDir", METHOD_TYPE_OBJECT },
    { M_U_CACHE, "getExternalCacheDir", METHOD_TYPE_OBJECT },
    { M_U_OBB, "getObbDir", METHOD_TYPE_OBJECT },
    { M_U_DATA, "getDataDir", METHOD_TYPE_OBJECT },
    { M_U_PATH, "getPath", METHOD_TYPE_OBJECT },
    { M_U_PATH, "getAbsolutePath", METHOD_TYPE_OBJECT },
    { M_U_PATH, "getCanonicalPath", METHOD_TYPE_OBJECT },
    { M_U_PACKAGE, "getPackageName", METHOD_TYPE_OBJECT },
    { M_U_APK, "getPackageCodePath", METHOD_TYPE_OBJECT },
    { M_U_APK, "getPackageResourcePath", METHOD_TYPE_OBJECT },
    { M_U_LANGUAGE, "getLanguage", METHOD_TYPE_OBJECT },
    { M_U_COUNTRY, "getCountry", METHOD_TYPE_OBJECT },
    { M_G_TRUE, "exists", METHOD_TYPE_BOOLEAN },
    { M_G_TRUE, "mkdirs", METHOD_TYPE_BOOLEAN },
    { M_G_TRUE, "isDirectory", METHOD_TYPE_BOOLEAN },
    { M_G_FALSE, "hasSystemFeature", METHOD_TYPE_BOOLEAN },
    { M_G_FALSE, "isFinishing", METHOD_TYPE_BOOLEAN },
    { M_G_FALSE, "getBoolean", METHOD_TYPE_BOOLEAN },
    { M_G_ZERO, "getInt", METHOD_TYPE_INT },
    { M_G_ZERO, "getRequestedOrientation", METHOD_TYPE_INT },
    { M_G_ZERO, "getRotation", METHOD_TYPE_INT },
    { M_G_NULL, "getString", METHOD_TYPE_OBJECT },
};

MethodsBoolean methodsBoolean[] = { { M_IS_TV, isTV }, { M_G_TRUE, g_true }, { M_G_FALSE, g_false } };
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = { { M_GET_DENSITY, getDensity } };
MethodsInt methodsInt[] = {
    { M_ADD_NUMBERS, addNumbers }, { M_G_GLES_VERSION, g_gles_version }, { M_G_ZERO, g_zero },
    { M_G_DPI, g_dpi },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { M_GET_DEVICE_NAME, getDeviceName },
    { M_GET_FILES_DIR, getFilesDir },
    { M_G_STRING_EMPTY, g_string_empty }, { M_G_OBJECT, g_object }, { M_G_NULL, g_null },
    { M_G_DATA_DIR, g_data_dir }, { M_G_LOCALE, g_locale }, { M_G_MODEL, g_model },
    { M_G_SAFE_AREA, g_safe_area }, { M_G_UNIQUE_ID, g_unique_id }, { M_G_SYSTEM_DIR, g_system_dir },
    { M_U_OBJ, u_obj }, { M_U_FILES, u_files_dir }, { M_U_CACHE, u_cache_dir }, { M_U_OBB, u_obb_dir },
    { M_U_DATA, u_data_dir }, { M_U_PATH, u_path }, { M_U_PACKAGE, u_package }, { M_U_APK, u_apk_path },
    { M_U_LANGUAGE, u_language }, { M_U_COUNTRY, u_country },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { M_VIBRATE, vibrate }, { M_G_IGNORED, g_ignored }, { M_G_ALERT, g_alert },
    { M_G_FORCE_QUIT, g_force_quit }, { M_G_RESTART, g_restart },
};

// ---- Java fields -----------------------------------------------------------

enum { F_SDK_INT = 1, F_GODOT_IO, F_GODOT_NET_UTILS, F_SOURCE_DIR, F_PUBLIC_SOURCE_DIR,
       F_NATIVE_LIB_DIR, F_DATA_DIR, F_SPLIT_DIRS, F_SPLIT_PUBLIC_DIRS, F_META_DATA, F_FLAGS, F_TARGET_SDK,
       F_VERSION_NAME, F_VERSION_CODE, F_WIDTH, F_HEIGHT, F_DENSITY_DPI, F_DENSITY, F_XDPI, F_YDPI,
       F_MODEL, F_MANUFACTURER, F_BRAND, F_DEVICE, F_PRODUCT, F_HARDWARE, F_FINGERPRINT, F_RELEASE,
       F_CURRENT_ACTIVITY, F_SO_UNSPECIFIED, F_SO_LANDSCAPE, F_SO_PORTRAIT, F_SO_USER, F_SO_BEHIND,
       F_SO_SENSOR, F_SO_NOSENSOR, F_SO_SENSOR_LANDSCAPE, F_SO_SENSOR_PORTRAIT, F_SO_REVERSE_LANDSCAPE,
       F_SO_REVERSE_PORTRAIT, F_SO_FULL_SENSOR, F_SO_USER_LANDSCAPE, F_SO_USER_PORTRAIT, F_SO_FULL_USER,
       F_SO_LOCKED, F_UNITY_PLAYER };

// Godot reads these static fields off its Godot class (GodotIO, GodotNetUtils).
static char godot_io_object[] = "org/godotengine/godot/GodotIO";
static char godot_net_object[] = "org/godotengine/godot/utils/GodotNetUtils";

NameToFieldID nameToFieldId[] = {
    { F_SDK_INT, "SDK_INT", FIELD_TYPE_INT },
    { F_GODOT_IO, "io", FIELD_TYPE_OBJECT },
    { F_GODOT_NET_UTILS, "netUtils", FIELD_TYPE_OBJECT },
    { F_SOURCE_DIR, "sourceDir", FIELD_TYPE_OBJECT },
    { F_PUBLIC_SOURCE_DIR, "publicSourceDir", FIELD_TYPE_OBJECT },
    { F_NATIVE_LIB_DIR, "nativeLibraryDir", FIELD_TYPE_OBJECT },
    { F_DATA_DIR, "dataDir", FIELD_TYPE_OBJECT },
    { F_SPLIT_DIRS, "splitSourceDirs", FIELD_TYPE_OBJECT },
    { F_SPLIT_PUBLIC_DIRS, "splitPublicSourceDirs", FIELD_TYPE_OBJECT }, // Unity finds its asset packs here
    { F_META_DATA, "metaData", FIELD_TYPE_OBJECT },
    { F_FLAGS, "flags", FIELD_TYPE_INT },
    { F_TARGET_SDK, "targetSdkVersion", FIELD_TYPE_INT },
    { F_VERSION_NAME, "versionName", FIELD_TYPE_OBJECT },
    { F_VERSION_CODE, "versionCode", FIELD_TYPE_INT },
    { F_WIDTH, "widthPixels", FIELD_TYPE_INT },
    { F_HEIGHT, "heightPixels", FIELD_TYPE_INT },
    { F_DENSITY_DPI, "densityDpi", FIELD_TYPE_INT },
    { F_DENSITY, "density", FIELD_TYPE_FLOAT },
    { F_XDPI, "xdpi", FIELD_TYPE_FLOAT },
    { F_YDPI, "ydpi", FIELD_TYPE_FLOAT },
    { F_MODEL, "MODEL", FIELD_TYPE_OBJECT },
    { F_MANUFACTURER, "MANUFACTURER", FIELD_TYPE_OBJECT },
    { F_BRAND, "BRAND", FIELD_TYPE_OBJECT },
    { F_DEVICE, "DEVICE", FIELD_TYPE_OBJECT },
    { F_PRODUCT, "PRODUCT", FIELD_TYPE_OBJECT },
    { F_HARDWARE, "HARDWARE", FIELD_TYPE_OBJECT },
    { F_FINGERPRINT, "FINGERPRINT", FIELD_TYPE_OBJECT },
    { F_RELEASE, "RELEASE", FIELD_TYPE_OBJECT },
    { F_CURRENT_ACTIVITY, "currentActivity", FIELD_TYPE_OBJECT },
    // ActivityInfo's orientations: Unity reads them and asks Activity.setRequestedOrientation with
    // one (all 0 = landscape before; the portrait games' wish never showed)
    { F_SO_UNSPECIFIED, "SCREEN_ORIENTATION_UNSPECIFIED", FIELD_TYPE_INT },
    { F_SO_LANDSCAPE, "SCREEN_ORIENTATION_LANDSCAPE", FIELD_TYPE_INT },
    { F_SO_PORTRAIT, "SCREEN_ORIENTATION_PORTRAIT", FIELD_TYPE_INT },
    { F_SO_USER, "SCREEN_ORIENTATION_USER", FIELD_TYPE_INT },
    { F_SO_BEHIND, "SCREEN_ORIENTATION_BEHIND", FIELD_TYPE_INT },
    { F_SO_SENSOR, "SCREEN_ORIENTATION_SENSOR", FIELD_TYPE_INT },
    { F_SO_NOSENSOR, "SCREEN_ORIENTATION_NOSENSOR", FIELD_TYPE_INT },
    { F_SO_SENSOR_LANDSCAPE, "SCREEN_ORIENTATION_SENSOR_LANDSCAPE", FIELD_TYPE_INT },
    { F_SO_SENSOR_PORTRAIT, "SCREEN_ORIENTATION_SENSOR_PORTRAIT", FIELD_TYPE_INT },
    { F_SO_REVERSE_LANDSCAPE, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE", FIELD_TYPE_INT },
    { F_SO_REVERSE_PORTRAIT, "SCREEN_ORIENTATION_REVERSE_PORTRAIT", FIELD_TYPE_INT },
    { F_SO_FULL_SENSOR, "SCREEN_ORIENTATION_FULL_SENSOR", FIELD_TYPE_INT },
    { F_SO_USER_LANDSCAPE, "SCREEN_ORIENTATION_USER_LANDSCAPE", FIELD_TYPE_INT },
    { F_SO_USER_PORTRAIT, "SCREEN_ORIENTATION_USER_PORTRAIT", FIELD_TYPE_INT },
    { F_SO_FULL_USER, "SCREEN_ORIENTATION_FULL_USER", FIELD_TYPE_INT },
    { F_SO_LOCKED, "SCREEN_ORIENTATION_LOCKED", FIELD_TYPE_INT },
    // UnityPlayerActivity's player: the activity stands in for it (Dragon Ball Legends' web view plugin
    // reads it; null, its C# threw and the title screen's tap went nowhere)
    { F_UNITY_PLAYER, "mUnityPlayer", FIELD_TYPE_OBJECT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = { { F_DENSITY, 1.0f }, { F_XDPI, 160.0f }, { F_YDPI, 160.0f } };
FieldsInt fieldsInt[] = {
    { F_SDK_INT, 30 }, // Android 11
    { F_FLAGS, 0 }, { F_TARGET_SDK, 33 }, { F_VERSION_CODE, 1 }, { F_WIDTH, 1920 },
    { F_HEIGHT, 1080 }, { F_DENSITY_DPI, 160 },
    { F_SO_UNSPECIFIED, -1 }, { F_SO_LANDSCAPE, 0 }, { F_SO_PORTRAIT, 1 }, { F_SO_USER, 2 }, { F_SO_BEHIND, 3 },
    { F_SO_SENSOR, 4 }, { F_SO_NOSENSOR, 5 }, { F_SO_SENSOR_LANDSCAPE, 6 }, { F_SO_SENSOR_PORTRAIT, 7 },
    { F_SO_REVERSE_LANDSCAPE, 8 }, { F_SO_REVERSE_PORTRAIT, 9 }, { F_SO_FULL_SENSOR, 10 },
    { F_SO_USER_LANDSCAPE, 11 }, { F_SO_USER_PORTRAIT, 12 }, { F_SO_FULL_USER, 13 }, { F_SO_LOCKED, 14 },
};
// Strings are made at jni_setup (FalsoJNI strings are heap memory).
FieldsObject fieldsObject[] = {
    { F_GODOT_IO, (jobject)godot_io_object }, { F_GODOT_NET_UTILS, (jobject)godot_net_object },
    { F_SOURCE_DIR, NULL }, { F_PUBLIC_SOURCE_DIR, NULL }, { F_NATIVE_LIB_DIR, NULL },
    { F_DATA_DIR, NULL }, { F_SPLIT_DIRS, NULL }, { F_SPLIT_PUBLIC_DIRS, NULL }, { F_META_DATA, (jobject)obj_generic },
    { F_VERSION_NAME, NULL }, { F_MODEL, NULL }, { F_MANUFACTURER, NULL }, { F_BRAND, NULL },
    { F_DEVICE, NULL }, { F_PRODUCT, NULL }, { F_HARDWARE, NULL }, { F_FINGERPRINT, NULL },
    { F_RELEASE, NULL }, { F_CURRENT_ACTIVITY, NULL }, { F_UNITY_PLAYER, NULL },
};
FieldsLong fieldsLong[] = {};

// DisplayMetrics' widthPixels / heightPixels: the game's screen (the phone view's, view_decide)
void jni_set_screen(int w, int h) {
    for (size_t i = 0; i < sizeof(fieldsInt) / sizeof(fieldsInt[0]); i++) {
        if (fieldsInt[i].id == F_WIDTH) fieldsInt[i].value = w;
        if (fieldsInt[i].id == F_HEIGHT) fieldsInt[i].value = h;
    }
}
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES

// ---- RegisterNatives -------------------------------------------------------

#define MAX_NATIVES 512
static struct {
    char name[96];
    char signature[160];
    void *fn;
} natives[MAX_NATIVES];
static int native_count;

static jint register_natives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                             jint count) {
    (void)env; (void)clazz;
    if (count < 0 || count > MAX_NATIVES || !methods) {
        droid_log("java: RegisterNatives of %d methods at %p: refused", count, (void *)methods);
        return JNI_ERR;
    }
    for (jint i = 0; i < count && native_count < MAX_NATIVES; i++) {
        // a slot with no name or signature (Genshin Impact's table has them): left out
        if (!methods[i].name || !methods[i].signature) {
            droid_log("java: RegisterNatives slot %d of %d: no name / signature (%p %p), left out", i, count,
                      (const void *)methods[i].name, (const void *)methods[i].signature);
            continue;
        }
        strncpy(natives[native_count].name, methods[i].name, sizeof(natives[0].name) - 1);
        strncpy(natives[native_count].signature, methods[i].signature,
                sizeof(natives[0].signature) - 1);
        natives[native_count].fn = methods[i].fnPtr;
        droid_log("java: RegisterNatives %s %s -> %p", methods[i].name, methods[i].signature,
                  methods[i].fnPtr);
        native_count++;
    }
    return JNI_OK;
}

void *jni_find_native(const char *name) {
    for (int i = 0; i < native_count; i++)
        if (strcmp(natives[i].name, name) == 0) return natives[i].fn;
    return NULL;
}

// its signature as registered ("" if none): one name, other arguments by Unity version
const char *jni_native_signature(const char *name) {
    for (int i = 0; i < native_count; i++)
        if (strcmp(natives[i].name, name) == 0) return natives[i].signature;
    return "";
}

static void set_field(int id, jobject value) {
    for (size_t i = 0; i < sizeof(fieldsObject) / sizeof(fieldsObject[0]); i++)
        if (fieldsObject[i].id == id) fieldsObject[i].value = value;
}

// The game's version, from its manifest (PackageInfo.versionName / versionCode): games
// send it to their servers, and "1.0" got Shadow Fight 2 an "update your game"
void jni_set_version(const char *name, long code) {
    droid_java_version_code = (int)code;
    droid_java_version_name = strdup(name ? name : "");
    if (name && *name) set_field(F_VERSION_NAME, jni->NewStringUTF(&jni, name));
    for (size_t i = 0; i < sizeof(fieldsInt) / sizeof(fieldsInt[0]); i++)
        if (fieldsInt[i].id == F_VERSION_CODE && code > 0) fieldsInt[i].value = (int)code;
}

// The app's paths (call again when they are known: APK path, package, splits).
void jni_set_app(const char *package, const char *apk_path, const char *lib_dir,
                 const char *const *splits, int split_count, void *activity) {
    java_package = strdup(package);
    java_apk_path = strdup(apk_path);
    java_lib_dir = strdup(lib_dir);
    jobject s = jni->NewStringUTF(&jni, java_apk_path);
    set_field(F_SOURCE_DIR, s);
    set_field(F_PUBLIC_SOURCE_DIR, s);
    set_field(F_NATIVE_LIB_DIR, jni->NewStringUTF(&jni, java_lib_dir));
    set_field(F_DATA_DIR, jni->NewStringUTF(&jni, game_data_dir));
    if (split_count > 0) {
        jobjectArray arr = jni->NewObjectArray(&jni, split_count, NULL, NULL);
        for (int i = 0; i < split_count; i++)
            jni->SetObjectArrayElement(&jni, arr, i, jni->NewStringUTF(&jni, splits[i]));
        set_field(F_SPLIT_DIRS, arr);
        set_field(F_SPLIT_PUBLIC_DIRS, arr);
    }
    if (activity) set_field(F_CURRENT_ACTIVITY, (jobject)activity), set_field(F_UNITY_PLAYER, (jobject)activity);
}

void jni_setup(void) {
    jni_init();
    ((struct JNINativeInterface *)jni)->RegisterNatives = register_natives;
    set_field(F_VERSION_NAME, jni->NewStringUTF(&jni, "1.0"));
    set_field(F_MODEL, jni->NewStringUTF(&jni, "PlayStation 5"));
    set_field(F_MANUFACTURER, jni->NewStringUTF(&jni, "Sony"));
    set_field(F_BRAND, jni->NewStringUTF(&jni, "PlayStation"));
    set_field(F_DEVICE, jni->NewStringUTF(&jni, "ps5"));
    set_field(F_PRODUCT, jni->NewStringUTF(&jni, "ps5"));
    set_field(F_HARDWARE, jni->NewStringUTF(&jni, "ps5"));
    set_field(F_FINGERPRINT, jni->NewStringUTF(&jni, "sony/ps5/ps5:11/RP1A/1:user/release-keys"));
    set_field(F_RELEASE, jni->NewStringUTF(&jni, "11"));
}
