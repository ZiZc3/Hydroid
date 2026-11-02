// java_layer.c â€” Java objects and methods for the game, above FalsoJNI.
// FalsoJNI answers methods by name only and has no objects; engines like
// Unity need real ones: StringBuilder, Class.forName, InputStream over the
// APK, IsInstanceOf. Methods here are found by class + name + signature;
// everything else still goes to FalsoJNI (jni_impl.c's tables).
//
// An object of ours is a jobj (magic first). FalsoJNI's objects are C strings
// (a jclass is its name, strdup'd), which our code tells apart by the magic.

#include "imangi_xls.h"
#include "firebase_rc.h"
#include "java_layer.h"
#include "video_tex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "FalsoJNI.h"
#include "game_settings.h"
#include "android_shim.h"
#include "arm_cpu.h"
#include "arm_modules.h"
#include "soloader.h"

#define JOBJ_MAGIC 0x4a4f424au // "JOBJ"

typedef struct {
    uint32_t magic;
    const char *cls;
    char *buf; // StringBuilder text
    size_t len, cap;
    void *asset; // InputStream: an AAsset
    int64_t mark;
    jobject ref;     // a second object (Handler's callback, Message's target Handler)
    jvalue field[4]; // per-object fields (Message: what, arg1, arg2, obj)
} jobj;

typedef jvalue (*jfn)(jobject self, const jvalue *a);
typedef struct {
    const char *cls, *name, *sig;
    jfn fn;
} jmeth;

// FalsoJNI's placeholder handles (not memory)
static int placeholder(const void *p) {
    uintptr_t u = (uintptr_t)p;
    return u < 0x10000 || u == 0x42424242 || u == 0x69696969 || u >= 0x0000800000000000ull;
}

static jobj *as_obj(jobject o) {
    jobj *j = (jobj *)o;
    return !placeholder(j) && j->magic == JOBJ_MAGIC ? j : NULL;
}

// Every class handle we gave out (FindClass, GetObjectClass): a class's own
// class is java/lang/Class.
#define CLASS_SET 8192
static const void *class_set[CLASS_SET];
static pthread_mutex_t class_lock = PTHREAD_MUTEX_INITIALIZER;

static int class_slot(const void *p) { return (int)(((uintptr_t)p >> 4) * 2654435761u % CLASS_SET); }

void java_note_class(jclass c) {
    if (placeholder(c)) return;
    pthread_mutex_lock(&class_lock);
    for (int i = class_slot(c), n = 0; n < CLASS_SET; i = (i + 1) % CLASS_SET, n++) {
        if (class_set[i] == c) break;
        if (!class_set[i]) {
            class_set[i] = c;
            break;
        }
    }
    pthread_mutex_unlock(&class_lock);
}

static int is_class(const void *c) {
    if (placeholder(c)) return 0;
    int found = 0;
    pthread_mutex_lock(&class_lock);
    for (int i = class_slot(c), n = 0; n < CLASS_SET && class_set[i]; i = (i + 1) % CLASS_SET, n++)
        if (class_set[i] == c) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&class_lock);
    return found;
}

static jclass find_class(const char *name) {
    jclass c = jni->FindClass(&jni, name);
    java_note_class(c);
    return c;
}

static jobj *new_obj(const char *cls) {
    jobj *j = calloc(1, sizeof(*j));
    j->magic = JOBJ_MAGIC;
    j->cls = cls;
    return j;
}

static jvalue L(jobject o) { jvalue v; v.l = o; return v; }
static jvalue I(jint i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue J(jlong x) { jvalue v; v.j = x; return v; }
static jvalue Z(jboolean b) { jvalue v; v.j = 0; v.z = b; return v; }
static jvalue V(void) { jvalue v; v.j = 0; return v; }

static jobject jstr(const char *s) { return jni->NewStringUTF(&jni, s); }

// FalsoJNI's java.lang.String is a JavaString { utf16, utf8 } of two arrays
// carrying JDA_MAGIC. A raw C string (8 bytes of text) is never a canonical
// user pointer, so the two can't be confused.
#define JDA_MAGIC 0xB00B1E5u
typedef struct {
    uint32_t magic;
    void *array;
    jsize len;
    int type;
} jda_t;
typedef struct {
    jda_t *utf16, *utf8;
} jstring_t;

static int user_ptr(const void *p) {
    uintptr_t u = (uintptr_t)p;
    return u >= 0x10000 && u < 0x0000800000000000ull && u != 0x42424242 && u != 0x69696969;
}

int java_is_string(jobject o) {
    if (!user_ptr(o)) return 0;
    const jstring_t *s = (const jstring_t *)o;
    return user_ptr(s->utf8) && user_ptr(s->utf16) && s->utf8->magic == JDA_MAGIC;
}

// The text of a String, or of a raw C string standing in for one.
const char *java_text(jobject o) {
    if (java_is_string(o)) return (const char *)((const jstring_t *)o)->utf8->array;
    return user_ptr(o) ? (const char *)o : "";
}

static const char *cstr(jobject s) { return java_text(s); }
jobject java_new_string(const char *s) { return jstr(s ? s : ""); }

// ---- java.lang.Class / ClassLoader ---------------------------------------------------

static jvalue class_for_name(jobject self, const jvalue *a) {
    (void)self;
    char name[256];
    snprintf(name, sizeof(name), "%s", cstr(a[0].l));
    for (char *p = name; *p; p++)
        if (*p == '.') *p = '/';
    return L(find_class(name));
}
// loadClass on a class loader object of FalsoJNI's (its class: java/lang/Object): a class, as
// ClassLoader's. The stub's empty object was then used as a class (King's helpers looked up on
// "JBOJ", our object's magic: Candy Crush crashed in GetMethodID on the console). King only.
static jvalue object_load_class(jobject self, const jvalue *a) { return class_for_name(self, a); }

static jvalue class_get_name(jobject self, const jvalue *a) {
    (void)a;
    char name[256];
    snprintf(name, sizeof(name), "%s", !placeholder(self) ? (const char *)self : "java/lang/Object");
    for (char *p = name; *p; p++)
        if (*p == '/') *p = '.';
    return L(jstr(name));
}

// ---- java.lang.StringBuilder ---------------------------------------------------------

static void sb_add(jobj *s, const char *t, size_t n) {
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = 0;
}

static jvalue sb_new(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jobj *s = new_obj("java/lang/StringBuilder");
    sb_add(s, "", 0);
    return L(s);
}
static jvalue sb_new_str(jobject self, const jvalue *a) {
    jvalue v = sb_new(self, a);
    sb_add(as_obj(v.l), cstr(a[0].l), strlen(cstr(a[0].l)));
    return v;
}
static jvalue sb_new_cap(jobject self, const jvalue *a) { (void)a; return sb_new(self, NULL); }

static jvalue sb_append_str(jobject self, const jvalue *a) {
    jobj *s = as_obj(self);
    if (s) {
        jobj *other = as_obj(a[0].l); // a StringBuilder appended (CharSequence)
        const char *t = other ? (other->buf ? other->buf : "") : a[0].l ? java_text(a[0].l) : "null";
        sb_add(s, t, strlen(t));
    }
    return L(self);
}
static jvalue sb_append_int(jobject self, const jvalue *a) {
    char t[32];
    snprintf(t, sizeof(t), "%d", a[0].i);
    if (as_obj(self)) sb_add(as_obj(self), t, strlen(t));
    return L(self);
}
static jvalue sb_append_long(jobject self, const jvalue *a) {
    char t[32];
    snprintf(t, sizeof(t), "%lld", (long long)a[0].j);
    if (as_obj(self)) sb_add(as_obj(self), t, strlen(t));
    return L(self);
}
static jvalue sb_append_char(jobject self, const jvalue *a) {
    char t[4] = { (char)a[0].c, 0 };
    if (as_obj(self)) sb_add(as_obj(self), t, 1);
    return L(self);
}
static jvalue sb_append_bool(jobject self, const jvalue *a) {
    const char *t = a[0].z ? "true" : "false";
    if (as_obj(self)) sb_add(as_obj(self), t, strlen(t));
    return L(self);
}
static jvalue sb_append_float(jobject self, const jvalue *a) {
    char t[48];
    snprintf(t, sizeof(t), "%g", (double)a[0].f);
    if (as_obj(self)) sb_add(as_obj(self), t, strlen(t));
    return L(self);
}
static jvalue sb_append_double(jobject self, const jvalue *a) {
    char t[48];
    snprintf(t, sizeof(t), "%g", a[0].d);
    if (as_obj(self)) sb_add(as_obj(self), t, strlen(t));
    return L(self);
}
static jvalue sb_length(jobject self, const jvalue *a) {
    (void)a;
    return I(as_obj(self) ? (jint)as_obj(self)->len : 0);
}

// Object.toString(): a StringBuilder's text, a String itself, else the class name.
static jvalue obj_to_string(jobject self, const jvalue *a) {
    (void)a;
    if (java_is_string(self)) return L(self);
    jobj *s = as_obj(self);
    if (s) return L(jstr(s->buf && strcmp(s->cls, "java/lang/reflect/Proxy") != 0 ? s->buf : s->cls));
    return L(jstr(!placeholder(self) ? (const char *)self : "null"));
}

// ---- AssetManager.open -> InputStream over the APK's assets/ -------------------------

typedef void *(*asset_open_fn)(void *, const char *, int);
typedef int (*asset_read_fn)(void *, void *, size_t);
typedef long (*asset_len_fn)(void *);
typedef int64_t (*asset_seek_fn)(void *, int64_t, int);
typedef void (*asset_close_fn)(void *);

static void *asset_fn(const char *name) { return droid_resolve(name); }

void apk_each(const char *prefix, void (*fn)(const char *name, void *ctx), void *ctx); // apk.c

// AssetManager.list(dir): the names right under assets/<dir>, in the APK and its splits
// (Genshin Impact checks a file is there this way: none, and its resource index "wasn't there")
typedef struct {
    size_t skip;
    char *names[4096];
    int n;
} am_list_ctx;
static void am_list_visit(const char *name, void *ctx) {
    am_list_ctx *c = ctx;
    const char *rest = name + c->skip;
    size_t len = strcspn(rest, "/");
    if (!len || c->n >= (int)(sizeof(c->names) / sizeof(c->names[0]))) return;
    for (int i = 0; i < c->n; i++) // (a folder: once)
        if (strlen(c->names[i]) == len && !strncmp(c->names[i], rest, len)) return;
    c->names[c->n++] = strndup(rest, len);
}
static void unity_send_message(const char *object, const char *method, const char *text);

// text as a JSON string's inside (quotes, backslashes, control characters escaped)
static void json_escape(const char *in, char *out, size_t n) {
    size_t k = 0;
    for (; *in && k + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, n - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k] = 0;
}

// An answer to an invoke the game waits on (a callback index >= 0), as ComboForUnity sends it:
// MiHoYoSDK.OnGetInvokeResponse {"index": i, "data": "<the SDK's result JSON>"}
static void combo_answer(int index, const char *result) {
    static char esc[16384], msg[16800];
    json_escape(result, esc, sizeof(esc));
    snprintf(msg, sizeof(msg), "{\"index\":%d,\"data\":\"%s\"}", index, esc);
    droid_log("combo: answer %d -> MiHoYoSDK.OnGetInvokeResponse %.300s", index, result);
    unity_send_message("MiHoYoSDK", "OnGetInvokeResponse", msg);
}

// The account: none on this console (the SDK's login window is Java views): the game hears so
static void combo_login(int index) {
    droid_log("combo: login_login: no account on this console");
    combo_answer(index, "{\"ret\":-103,\"msg\":\"no account on this console\"}");
}

// miHoYo's DeviceInfo.Device storage sizes (bytes): the game's data folder's disk. Unanswered
// they were 0 and Genshin Impact's resource download thought the disk full. As statvfs
// (libc_extra.c) when the disk can't be asked: 128 GB, 64 GB free
#include <sys/statvfs.h>
static jlong storage_bytes(int free_only) {
    struct statvfs s;
    if (statvfs(game_data_dir[0] ? game_data_dir : game_dir, &s) == 0 && s.f_blocks)
        return (jlong)(free_only ? s.f_bavail : s.f_blocks) * (jlong)s.f_frsize;
    return free_only ? (64LL << 30) : (128LL << 30);
}
static jvalue device_storage_total(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jlong n = storage_bytes(0);
    droid_log("java: DeviceInfo storage total = %lld MB", (long long)(n >> 20));
    return J(n);
}
static jvalue device_storage_free(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jlong n = storage_bytes(1);
    droid_log("java: DeviceInfo storage free = %lld MB", (long long)(n >> 20));
    return J(n);
}

// NVIDIA's NvUtil (Rockstar's ports still use it): its app-local values, which the game's
// GameActivityBase.onCreate sets - STORAGE_ROOT (getExternalFilesDir) and STORAGE_ROOT_BASE (the
// part before /Android). Android's own paths: the file calls map them into the game's folder.
// Unanswered, GTA San Andreas built every path on "" and read "/gta_sa.set"
#define NV_VALUES 16
static char nv_key[NV_VALUES][64], nv_value[NV_VALUES][300];
static int nv_count;
static int nv_find(const char *key) {
    if (!nv_count) { // the two onCreate sets
        extern const char *java_package; // jni_impl.c
        snprintf(nv_key[0], sizeof(nv_key[0]), "STORAGE_ROOT");
        snprintf(nv_value[0], sizeof(nv_value[0]), "/storage/emulated/0/Android/data/%s/files", java_package);
        snprintf(nv_key[1], sizeof(nv_key[1]), "STORAGE_ROOT_BASE");
        snprintf(nv_value[1], sizeof(nv_value[1]), "/storage/emulated/0");
        nv_count = 2;
        // getExternalFilesDir makes its folder: the game writes there at once, unchecked (GTA San
        // Andreas: fopen(".../files/CINFO.BIN", "wb") NULL, then fwrite on it - a crash at 0x8)
        const char *droid_abs(const char *path, char *buf, size_t n); // linux_abi.c
        char real[512];
        const char *r = droid_abs(nv_value[0], real, sizeof(real));
        char made[512];
        snprintf(made, sizeof(made), "%s", r ? r : nv_value[0]);
        for (char *p = made + 1; *p; p++)
            if (*p == '/') {
                *p = 0;
                mkdir(made, 0777);
                *p = '/';
            }
        mkdir(made, 0777);
        droid_log("java: STORAGE_ROOT %s (made: %s)", nv_value[0], made);
    }
    for (int i = 0; key && i < nv_count; i++)
        if (!strcmp(nv_key[i], key)) return i;
    return -1;
}
static jvalue nv_instance(jobject self, const jvalue *a) { (void)self; (void)a; return L(new_obj("com/nvidia/devtech/NvUtil")); }
// Rovio's permissions helper (Angry Birds 2): its C# throws "Failed to get androidPermissionsPlugin
// from UnityPlayer" on null, and the game never finishes its start ("Oops! Something went wrong!")
static jvalue rovio_permissions(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(new_obj("com/rovio/androidpermissionsplugin/AndroidPermissions"));
}
static jvalue nv_has(jobject self, const jvalue *a) { (void)self; return Z(nv_find(cstr(a[0].l)) >= 0); }
static jvalue nv_get(jobject self, const jvalue *a) {
    (void)self;
    const char *key = cstr(a[0].l);
    int i = nv_find(key);
    droid_log("java: NvUtil.getAppLocalValue(%s) = %s", key ? key : "(null)", i >= 0 ? nv_value[i] : "(none)");
    return L(i >= 0 ? jstr(nv_value[i]) : NULL);
}
static jvalue nv_set(jobject self, const jvalue *a) {
    (void)self;
    const char *key = cstr(a[0].l), *value = cstr(a[1].l);
    if (!key) return V();
    int i = nv_find(key);
    if (i < 0 && nv_count < NV_VALUES) snprintf(nv_key[i = nv_count++], sizeof(nv_key[0]), "%s", key);
    if (i >= 0) snprintf(nv_value[i], sizeof(nv_value[0]), "%s", value ? value : "");
    return V();
}

// Rockstar's GamePlatformServices (GTA San Andreas 2.11's oswrapper): the requests the engine
// waits on, completed on its game thread (gtasa_host.c), offline
#include "gtasa_host.h"
static jvalue rs_initial(jobject self, const jvalue *a) { (void)self; (void)a; gtasa_post(GTASA_INITIAL, 0); return V(); }
static jvalue rs_gate(jobject self, const jvalue *a) { (void)self; gtasa_post(GTASA_GATE, a[0].i); return V(); }
static jvalue rs_sign_in(jobject self, const jvalue *a) { (void)self; (void)a; gtasa_post(GTASA_SIGN_IN, 0); return V(); }
static jvalue rs_sign_out(jobject self, const jvalue *a) { (void)self; (void)a; gtasa_post(GTASA_SIGN_OUT, 0); return V(); }
static jvalue rs_cloud_disabled(jobject self, const jvalue *a) { (void)self; (void)a; gtasa_post(GTASA_CLOUD_DISABLED, 0); return V(); }
static jvalue rs_deletion(jobject self, const jvalue *a) { (void)self; (void)a; gtasa_post(GTASA_DELETION, 0); return V(); }
static jvalue rs_playlist(jobject self, const jvalue *a) { (void)self; (void)a; gtasa_post(GTASA_PLAYLIST, 0); return V(); }
static jvalue rs_http(jobject self, const jvalue *a) { // offline: every request fails
    (void)self;
    droid_log("gtasa: HTTP request %d (%s): offline, failed", a[0].i, cstr(a[1].l) ? cstr(a[1].l) : "?");
    gtasa_post(GTASA_HTTP, a[0].i);
    return V();
}

// what an invoke with a callback gets: login_init and login_login their own, every other the
// SDK's own default (its test mode answers so: ret 0, msg "")
static void combo_reply(const char *what, int index) {
    if (!what) return;
    if (!strcmp(what, "login_init")) {
        // the SDK's start (ComboForUnity sends it to OnInitResponse, ret 0 = ready). Without it
        // the game waited in its login scene
        const char *ok = "{\"ret\":0,\"msg\":\"\",\"data\":\"\"}";
        droid_log("combo: login_init -> MiHoYoSDK.OnInitResponse %s", ok);
        unity_send_message("MiHoYoSDK", "OnInitResponse", ok);
        return;
    }
    if (index < 0) return;
    if (!strcmp(what, "login_login")) combo_login(index);
    else combo_answer(index, "{\"ret\":0,\"msg\":\"\"}");
}

// miHoYo's Combo SDK (Genshin Impact: its login, init): what the game asks, word for word
static jvalue combo_invoke(jobject self, const jvalue *a) {
    (void)self;
    const char *what = cstr(a[0].l), *json = cstr(a[1].l);
    droid_log("combo: invoke(%s, %.600s, %d)", what ? what : "(null)", json ? json : "(null)", a[2].i);
    combo_reply(what, a[2].i);
    return L(NULL);
}
// Tencent GME (Genshin Impact's voice chat): its log folder (it reads the answer as text)
static jvalue gme_default_dir(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char dir[400];
    game_path(dir, sizeof(dir), "files/gme");
    mkdir(dir, 0777);
    return L(jstr(dir));
}
// invoke with an object (SDKInit's, login's): what it is, word for word (a String, or its class)
static jvalue combo_invoke_obj(jobject self, const jvalue *a) {
    (void)self;
    const char *what = cstr(a[0].l), *s = cstr(a[1].l);
    droid_log("combo: invoke-object(%s, %s%.600s, callback %d)", what ? what : "(null)",
              s ? "" : "(not a string) ", s ? s : "", a[2].i);
    combo_reply(what, a[2].i);
    return L(NULL);
}
// the Combo SDK's answers the game reads at once (its device and SDK facts): asked word for word
static jvalue combo_invoke_return(jobject self, const jvalue *a) {
    (void)self;
    const char *what = cstr(a[0].l), *arg = cstr(a[1].l);
    // the store channel (the dispatch query's channel_id / sub_channel_id): miHoYo's own global
    // build is 1 / 0. Without them the game never sent its region list request (error 4204)
    static const char *const answers[][2] = {
        { "info_get_channel_id", "1" },
        { "info_get_sub_channel_id", "0" },
    };
    for (size_t i = 0; what && i < sizeof(answers) / sizeof(answers[0]); i++)
        if (!strcmp(what, answers[i][0])) {
            droid_log("combo: invokeReturn(%s) = %s", what, answers[i][1]);
            return L(jstr(answers[i][1]));
        }
    // the account's (no account here): no device id, the login screen's default name
    static const char *const from_account[][2] = {
        { "info_get_device_id", "" },
        { "login_get_asterisk_name", "Traveler" },
    };
    for (size_t i = 0; what && i < sizeof(from_account) / sizeof(from_account[0]); i++)
        if (!strcmp(what, from_account[i][0])) {
            droid_log("combo: invokeReturn(%s) = %s", what, from_account[i][1]);
            return L(jstr(from_account[i][1]));
        }
    droid_log("combo: invokeReturn(%s, %.300s): not known, null", what ? what : "(null)", arg ? arg : "(not a string)");
    return L(NULL);
}
static jvalue combo_set_version(jobject self, const jvalue *a) {
    (void)self;
    droid_log("combo: setUnityVersion(%s)", cstr(a[0].l) ? cstr(a[0].l) : "(null)");
    return L(NULL);
}

static jvalue am_list(jobject self, const jvalue *a) {
    (void)self;
    const char *dir = cstr(a[0].l);
    char prefix[600];
    snprintf(prefix, sizeof(prefix), "assets/%s%s", dir ? dir : "", dir && *dir && dir[strlen(dir) - 1] != '/' ? "/" : "");
    am_list_ctx *c = calloc(1, sizeof(*c));
    if (!c) return L(NULL);
    c->skip = strlen(prefix);
    apk_each(prefix, am_list_visit, c);
    jobjectArray arr = jni->NewObjectArray(&jni, c->n, NULL, NULL);
    for (int i = 0; i < c->n; i++) {
        jni->SetObjectArrayElement(&jni, arr, i, jstr(c->names[i]));
        free(c->names[i]);
    }
    static int told;
    if (told++ < 20) droid_log("java: AssetManager.list(%s): %d", dir ? dir : "", c->n);
    free(c);
    return L(arr);
}
// java.lang.reflect.Array (Unity's reflection reads Java arrays with it)
static jvalue array_get_length(jobject self, const jvalue *a) {
    (void)self;
    jvalue v;
    v.j = 0;
    v.i = a[0].l ? jni->GetArrayLength(&jni, (jarray)a[0].l) : 0;
    return v;
}
static jvalue array_get(jobject self, const jvalue *a) {
    (void)self;
    return L(a[0].l ? jni->GetObjectArrayElement(&jni, (jobjectArray)a[0].l, a[1].i) : NULL);
}
// Array.newInstance(class, n) / Array.set(array, i, value): how Unity's C# hands Java a String[]
// (Dragon Ball Legends' permission list): an array of objects, its elements set
static jvalue array_new(jobject self, const jvalue *a) {
    (void)self;
    return L(jni->NewObjectArray(&jni, a[1].i > 0 ? a[1].i : 0, NULL, NULL));
}
static jvalue array_set(jobject self, const jvalue *a) {
    (void)self;
    if (a[0].l) jni->SetObjectArrayElement(&jni, (jobjectArray)a[0].l, a[1].i, a[2].l);
    return V();
}

static jvalue am_open(jobject self, const jvalue *a) {
    (void)self;
    const char *path = cstr(a[0].l);
    void *asset = ((asset_open_fn)asset_fn("AAssetManager_open"))(NULL, path, 2);
    droid_log("java: AssetManager.open(%s) -> %s", path, asset ? "found" : "not in the APK");
    if (!asset) return L(NULL); // (a real one throws FileNotFoundException)
    jobj *in = new_obj("android/content/res/AssetManager$AssetInputStream");
    in->asset = asset;
    return L(in);
}

static jvalue in_read_arr(jobject self, const jvalue *a, jint off, jint len) {
    jobj *in = as_obj(self);
    if (!in || !in->asset || !a[0].l) return I(-1);
    if (len <= 0) return I(0);
    char *tmp = malloc((size_t)len);
    int n = ((asset_read_fn)asset_fn("AAsset_read"))(in->asset, tmp, (size_t)len);
    if (n > 0) jni->SetByteArrayRegion(&jni, (jbyteArray)a[0].l, off, n, (const jbyte *)tmp);
    free(tmp);
    return I(n > 0 ? n : -1);
}
static jvalue in_read_all(jobject self, const jvalue *a) {
    jint len = a[0].l ? jni->GetArrayLength(&jni, (jarray)a[0].l) : 0;
    return in_read_arr(self, a, 0, len);
}
static jvalue in_read_part(jobject self, const jvalue *a) { return in_read_arr(self, a, a[1].i, a[2].i); }
static jvalue in_read_byte(jobject self, const jvalue *a) {
    (void)a;
    jobj *in = as_obj(self);
    unsigned char c;
    if (!in || !in->asset) return I(-1);
    return I(((asset_read_fn)asset_fn("AAsset_read"))(in->asset, &c, 1) == 1 ? c : -1);
}
static jvalue in_available(jobject self, const jvalue *a) {
    (void)a;
    jobj *in = as_obj(self);
    if (!in || !in->asset) return I(0);
    long left = ((asset_len_fn)asset_fn("AAsset_getRemainingLength"))(in->asset);
    return I(left > 0x7fffffff ? 0x7fffffff : (jint)left);
}
static jvalue in_skip(jobject self, const jvalue *a) {
    jobj *in = as_obj(self);
    if (!in || !in->asset) return J(0);
    long left = ((asset_len_fn)asset_fn("AAsset_getRemainingLength"))(in->asset);
    jlong n = a[0].j < left ? a[0].j : left;
    ((asset_seek_fn)asset_fn("AAsset_seek64"))(in->asset, n, 1 /* SEEK_CUR */);
    return J(n);
}
static jvalue in_mark(jobject self, const jvalue *a) {
    (void)a;
    jobj *in = as_obj(self);
    if (in && in->asset) in->mark = ((asset_seek_fn)asset_fn("AAsset_seek64"))(in->asset, 0, 1);
    return V();
}
static jvalue in_reset(jobject self, const jvalue *a) {
    (void)a;
    jobj *in = as_obj(self);
    if (in && in->asset) ((asset_seek_fn)asset_fn("AAsset_seek64"))(in->asset, in->mark, 0);
    return V();
}
static jvalue in_close(jobject self, const jvalue *a) {
    (void)a;
    jobj *in = as_obj(self);
    if (in && in->asset) {
        ((asset_close_fn)asset_fn("AAsset_close"))(in->asset);
        in->asset = NULL;
    }
    return V();
}
static jvalue yes(jobject self, const jvalue *a) { (void)self; (void)a; return Z(JNI_TRUE); }

// ---- java.util.Scanner (Unity reads boot.config: new Scanner(in, "UTF-8").useDelimiter("\\z").next())

static jvalue sc_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *sc = new_obj("java/util/Scanner");
    sc->asset = a[0].l; // the InputStream (one of ours)
    return L(sc);
}

// The rest of the stream as one string (delimiter "\z" = end of input: all of it).
static jvalue sc_next(jobject self, const jvalue *a) {
    (void)a;
    jobj *sc = as_obj(self);
    jobj *in = sc ? as_obj((jobject)sc->asset) : NULL;
    if (!in || !in->asset) return L(NULL); // (NoSuchElementException)
    size_t cap = 4096, len = 0;
    char *text = malloc(cap);
    for (;;) {
        if (len + 4096 + 1 > cap) text = realloc(text, cap *= 2);
        int n = ((asset_read_fn)asset_fn("AAsset_read"))(in->asset, text + len, 4096);
        if (n <= 0) break;
        len += (size_t)n;
    }
    text[len] = 0;
    sc->len = len; // remembered: hasNext() is false afterwards
    sc->mark = 1;
    jobject s = jstr(text);
    free(text);
    return L(s);
}

static jvalue sc_has_next(jobject self, const jvalue *a) {
    (void)a;
    jobj *sc = as_obj(self);
    jobj *in = sc ? as_obj((jobject)sc->asset) : NULL;
    if (!in || !in->asset || sc->mark) return Z(JNI_FALSE);
    long left = ((asset_len_fn)asset_fn("AAsset_getRemainingLength"))(in->asset);
    return Z(left > 0 ? JNI_TRUE : JNI_FALSE);
}

static jvalue sc_self(jobject self, const jvalue *a) { (void)a; return L(self); }
static jvalue sc_close(jobject self, const jvalue *a) {
    jobj *sc = as_obj(self);
    if (sc) in_close((jobject)sc->asset, a);
    return V();
}

// ---- java.lang.String (Unity sizes buffers with String.length()) -------------------------

#include <ctype.h>

static const char *self_text(jobject self) { return java_text(self); }

static jvalue s_length(jobject self, const jvalue *a) { (void)a; return I((jint)strlen(self_text(self))); }
static jvalue s_is_empty(jobject self, const jvalue *a) { (void)a; return Z(self_text(self)[0] == 0); }
static jvalue s_equals(jobject self, const jvalue *a) {
    return Z(a[0].l && strcmp(self_text(self), java_text(a[0].l)) == 0);
}
static jvalue s_equals_ic(jobject self, const jvalue *a) {
    return Z(a[0].l && strcasecmp(self_text(self), java_text(a[0].l)) == 0);
}
static jvalue s_hash(jobject self, const jvalue *a) {
    (void)a;
    jint h = 0;
    for (const unsigned char *t = (const unsigned char *)self_text(self); *t; t++) h = 31 * h + *t;
    return I(h);
}
static jvalue s_char_at(jobject self, const jvalue *a) {
    const char *t = self_text(self);
    jvalue v;
    v.j = 0;
    v.c = (a[0].i >= 0 && (size_t)a[0].i < strlen(t)) ? (jchar)(unsigned char)t[a[0].i] : 0;
    return v;
}
static jvalue s_get_bytes(jobject self, const jvalue *a) {
    (void)a;
    const char *t = self_text(self);
    jsize n = (jsize)strlen(t);
    jbyteArray arr = jni->NewByteArray(&jni, n);
    if (n) jni->SetByteArrayRegion(&jni, arr, 0, n, (const jbyte *)t);
    return L(arr);
}
static jobject sub(const char *t, size_t from, size_t to) {
    size_t n = strlen(t);
    if (from > n) from = n;
    if (to > n) to = n;
    if (to < from) to = from;
    char *c = strndup(t + from, to - from);
    jobject r = jstr(c);
    free(c);
    return r;
}
static jvalue s_substring1(jobject self, const jvalue *a) {
    return L(sub(self_text(self), (size_t)(a[0].i < 0 ? 0 : a[0].i), (size_t)-1));
}
static jvalue s_substring2(jobject self, const jvalue *a) {
    return L(sub(self_text(self), (size_t)(a[0].i < 0 ? 0 : a[0].i), (size_t)(a[1].i < 0 ? 0 : a[1].i)));
}
static jvalue s_index_of_str(jobject self, const jvalue *a) {
    const char *t = self_text(self), *f = strstr(t, java_text(a[0].l));
    return I(f ? (jint)(f - t) : -1);
}
static jvalue s_index_of_char(jobject self, const jvalue *a) {
    const char *t = self_text(self), *f = strchr(t, a[0].i);
    return I(f ? (jint)(f - t) : -1);
}
static jvalue s_last_index_of_char(jobject self, const jvalue *a) {
    const char *t = self_text(self), *f = strrchr(t, a[0].i);
    return I(f ? (jint)(f - t) : -1);
}
static jvalue s_starts_with(jobject self, const jvalue *a) {
    const char *p = java_text(a[0].l);
    return Z(strncmp(self_text(self), p, strlen(p)) == 0);
}
static jvalue s_ends_with(jobject self, const jvalue *a) {
    const char *t = self_text(self), *p = java_text(a[0].l);
    size_t n = strlen(t), m = strlen(p);
    return Z(m <= n && strcmp(t + n - m, p) == 0);
}
static jvalue s_contains(jobject self, const jvalue *a) { return Z(strstr(self_text(self), java_text(a[0].l)) != NULL); }
static jvalue s_case(jobject self, int upper) {
    char *c = strdup(self_text(self));
    for (char *p = c; *p; p++) *p = (char)(upper ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
    jobject r = jstr(c);
    free(c);
    return L(r);
}
static jvalue s_lower(jobject self, const jvalue *a) { (void)a; return s_case(self, 0); }
static jvalue s_upper(jobject self, const jvalue *a) { (void)a; return s_case(self, 1); }
static jvalue s_trim(jobject self, const jvalue *a) {
    (void)a;
    const char *t = self_text(self);
    size_t from = 0, to = strlen(t);
    while (from < to && (unsigned char)t[from] <= ' ') from++;
    while (to > from && (unsigned char)t[to - 1] <= ' ') to--;
    return L(sub(t, from, to));
}
static jvalue s_concat(jobject self, const jvalue *a) {
    const char *x = self_text(self), *y = java_text(a[0].l);
    char *c = malloc(strlen(x) + strlen(y) + 1);
    strcpy(c, x);
    strcat(c, y);
    jobject r = jstr(c);
    free(c);
    return L(r);
}
static jvalue s_replace(jobject self, const jvalue *a) {
    const char *t = self_text(self), *from = java_text(a[0].l), *to = java_text(a[1].l);
    size_t fl = strlen(from), tl = strlen(to);
    if (!fl) return L(self);
    size_t cap = strlen(t) * (tl > fl ? tl / fl + 2 : 1) + 1, n = 0;
    char *out = malloc(cap);
    for (const char *p = t; *p;) {
        if (strncmp(p, from, fl) == 0) {
            if (n + tl + 1 > cap) out = realloc(out, cap = (n + tl + 1) * 2);
            memcpy(out + n, to, tl);
            n += tl;
            p += fl;
        } else {
            if (n + 2 > cap) out = realloc(out, cap *= 2);
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    jobject r = jstr(out);
    free(out);
    return L(r);
}
static jvalue s_value_of_int(jobject self, const jvalue *a) {
    (void)self;
    char t[24];
    snprintf(t, sizeof(t), "%d", a[0].i);
    return L(jstr(t));
}

// ---- platform answers Unity asks for at startup ---------------------------------------

extern const char *java_lib_dir; // jni_impl.c: where the APK's libraries are copied

// ClassLoader.findLibrary("il2cpp"): the path Unity then dlopen()s
static jvalue cl_find_library(jobject self, const jvalue *a) {
    (void)self;
    char path[512];
    snprintf(path, sizeof(path), "%s/lib%s.so", java_lib_dir, java_text(a[0].l));
    droid_log("java: findLibrary(%s) -> %s", java_text(a[0].l), path);
    return L(jstr(path));
}

static jvalue get_class(jobject self, const jvalue *a) { (void)a; return L(java_class_of(self)); }
static jvalue storage_mounted(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("mounted")); }
static jvalue answer_false(jobject self, const jvalue *a) { (void)self; (void)a; return Z(JNI_FALSE); }
static jvalue answer_void(jobject self, const jvalue *a) { (void)self; (void)a; return V(); }
// Activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_*): the game's wish, logged and
// kept in <game>/orientation.txt ("portrait" / "landscape") for the phone view's Auto
static jvalue act_set_orientation(jobject self, const jvalue *a) {
    (void)self;
    const int o = a[0].i;
    const int portrait = o == 1 || o == 7 || o == 9 || o == 12;
    const int landscape = o == 0 || o == 6 || o == 8 || o == 11;
    droid_log("java: the game asks for orientation %d (%s)", o, portrait ? "portrait" : landscape ? "landscape" : "any");
    if (portrait || landscape) {
        char p[700];
        FILE *f = fopen(game_path(p, sizeof(p), "orientation.txt"), "w");
        if (f) { fputs(portrait ? "portrait\n" : "landscape\n", f); fclose(f); }
    }
    return V();
}
static jvalue answer_true(jobject self, const jvalue *a) { (void)self; (void)a; return Z(JNI_TRUE); }
static jvalue answer_null(jobject self, const jvalue *a) { (void)self; (void)a; return L(NULL); }
// AudioManager.getProperty: a phone's output rate and buffer size (our OpenSL mixer's: 48 kHz,
// 256 frames). CRI's sound engine (Dragon Ball Legends) sizes its buffers from them: unanswered,
// it sent empty ones and never played a sound. Its keys are stub constants: they read as their names
// Integer.parseInt: CRI's sound engine reads the two above as text and turns them into numbers
static jvalue integer_parse(jobject self, const jvalue *a) {
    (void)self;
    const char *t = java_text(a[0].l);
    return I(t ? (jint)strtol(t, NULL, 10) : 0);
}
static jvalue audio_property(jobject self, const jvalue *a) {
    (void)self;
    const char *key = java_text(a[0].l);
    const char *v = !key ? NULL : strstr(key, "FRAMES_PER_BUFFER") ? "256" : strstr(key, "SAMPLE_RATE") ? "48000" : NULL;
    droid_log("java: AudioManager.getProperty(%s) -> %s", key ? key : "?", v ? v : "null");
    return L(v ? jstr(v) : NULL);
}
static jvalue answer_zero(jobject self, const jvalue *a) { (void)self; (void)a; return I(0); }
static jvalue answer_zero_long(jobject self, const jvalue *a) { (void)self; (void)a; return J(0); }
static jvalue answer_self(jobject self, const jvalue *a) { (void)a; return L(self); }
static jvalue answer_empty_string(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("")); }

// android.view.Display: the PS5's 1920x1080 at 60 Hz
static jvalue display_get(jobject self, const jvalue *a) { (void)self; (void)a; return L(new_obj("android/view/Display")); }
static jvalue display_width(jobject self, const jvalue *a) { (void)self; (void)a; return I(droid_game_w); }
static jvalue display_height(jobject self, const jvalue *a) { (void)self; (void)a; return I(droid_game_h); }
static jvalue display_refresh(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jvalue v;
    v.j = 0;
    v.f = 60.0f;
    return v;
}
// getRealMetrics(DisplayMetrics): FalsoJNI's DisplayMetrics fields (widthPixels...) are fixed already
static jvalue language_tag(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en-US")); }

extern const char *java_apk_path; // jni_impl.c
extern const char *java_package; // jni_impl.c
extern int droid_java_version_code; // jni_impl.c
extern const char *droid_java_version_name; // jni_impl.c
static jvalue pm_object(jobject self, const jvalue *a) {
    (void)self; (void)a;
    static jobject pm;
    if (!pm) pm = (jobject)new_obj("com/epicgames/ue4/PackageManager"); // (Unreal only: others keep FalsoJNI's)
    return L(pm);
}
static jvalue pi_object(jobject self, const jvalue *a) {
    (void)self; (void)a;
    static jobject pi;
    if (!pi) pi = (jobject)new_obj("com/epicgames/ue4/PackageInfo");
    return L(pi);
}
static jvalue ai_object(jobject self, const jvalue *a) {
    (void)self; (void)a;
    static jobject ai;
    if (!ai) ai = (jobject)new_obj("com/epicgames/ue4/ApplicationInfo");
    return L(ai);
}
static jvalue ctx_package_name(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr(java_package ? java_package : ""));
}
static jvalue ctx_version_code(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jvalue v; v.i = droid_java_version_code;
    return v;
}
static jvalue env_ext_storage(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr("file:/sdcard"));
}
static jvalue ctx_dir_files(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char p[300], u[310];
    snprintf(u, sizeof(u), "file:%s", game_path(p, sizeof(p), "files"));
    mkdir(p, 0777);
    return L(jstr(u));
}
static jvalue ctx_dir_cache(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char p[300], u[310];
    snprintf(u, sizeof(u), "file:%s", game_path(p, sizeof(p), "cache"));
    mkdir(p, 0777);
    return L(jstr(u));
}
static jvalue ctx_dir_obb(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char p[300], u[310];
    snprintf(u, sizeof(u), "file:%s", game_path(p, sizeof(p), "obb"));
    return L(jstr(u));
}
static jvalue ctx_dir_data(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr(game_dir));
}
static jvalue ctx_apk_path(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr(java_apk_path ? java_apk_path : ""));
}

// GameActivity.AndroidThunkJava_GetMetaData* (Unreal): the manifest's <meta-data> values. Unreal
// counts on its own (ue4.displaymetrics.dpi split on commas, element 0 read unchecked: a null
// answer was a read at 0 in ComputePhysicalScreenDensity)
#include "apk.h"
#include "game_file.h"
static int ue_meta(jobject key, char *out, size_t n, long *num) {
    static uint8_t *man;
    static size_t man_len;
    if (!man) man = apk_load("AndroidManifest.xml", &man_len);
    const char *k = cstr(key);
    int got = 0;
    // the keys GameActivity.java works out itself instead of reading the manifest
    if (k && !strcmp(k, "ue4.displaymetrics.dpi")) { // DisplayMetrics xdpi,ydpi: a phone's 320
        snprintf(out, n, "320.0,320.0");
        *num = 320;
        got = 1;
    } else if (k && !strcmp(k, "ue4.getUsedMemory")) { // bytes in use: a small game's 1 GB
        snprintf(out, n, "%lld", 1LL << 30);
        *num = 1L << 30;
        got = 1;
    } else {
        got = man && k && axml_meta(man, man_len, k, out, n, num);
    }
    static int told;
    if (told++ < 40) droid_log("java: manifest meta-data %s = %s", k ? k : "(null)", got ? out : "(none)");
    return got;
}
static jvalue ue_meta_string(jobject self, const jvalue *a) {
    (void)self;
    char v[512];
    long num = 0;
    return L(ue_meta(a[0].l, v, sizeof(v), &num) ? jstr(v) : NULL);
}
static jvalue ue_meta_int(jobject self, const jvalue *a) {
    (void)self;
    char v[512];
    long num = 0;
    jvalue r;
    r.j = 0;
    r.i = ue_meta(a[0].l, v, sizeof(v), &num) ? (jint)num : 0;
    return r;
}
static jvalue ue_meta_bool(jobject self, const jvalue *a) {
    (void)self;
    char v[512];
    long num = 0;
    return Z(ue_meta(a[0].l, v, sizeof(v), &num) && (num != 0 || !strcmp(v, "true")));
}
static jvalue ue_meta_has(jobject self, const jvalue *a) {
    (void)self;
    char v[512];
    long num = 0;
    return Z(ue_meta(a[0].l, v, sizeof(v), &num));
}

static jvalue obb_dirs(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jobjectArray arr = jni->NewObjectArray(&jni, 1, NULL, NULL);
    char obb[300], url[310];
    snprintf(url, sizeof(url), "file:%s", game_path(obb, sizeof(obb), "obb"));
    jni->SetObjectArrayElement(&jni, arr, 0, (jobject)strdup(url));
    return L(arr);
}

// ---- SharedPreferences (Unity's PlayerPrefs = the game's saves) --------------------------
// One store per name, kept in APP0/data/prefs/<name>.txt as lines "<type> <key>\t<value>"
// (type: i int, l long, f float, b boolean, s string).

typedef struct {
    char type;
    char *key, *value;
} pref;

typedef struct prefs_store {
    char name[128];
    pref *items;
    int count, cap;
    struct prefs_store *next;
} prefs_store;

static prefs_store *stores;
static pthread_mutex_t prefs_lock = PTHREAD_MUTEX_INITIALIZER;

static void prefs_path(const prefs_store *st, char *out, size_t n) {
    snprintf(out, n, "%s/prefs/%s.txt", game_data_dir, st->name);
}

static void prefs_put_raw(prefs_store *st, char type, const char *key, const char *value) {
    for (int i = 0; i < st->count; i++)
        if (strcmp(st->items[i].key, key) == 0) {
            free(st->items[i].value);
            st->items[i].value = strdup(value);
            st->items[i].type = type;
            return;
        }
    if (st->count == st->cap) st->items = realloc(st->items, sizeof(pref) * (size_t)(st->cap = st->cap ? st->cap * 2 : 16));
    st->items[st->count].type = type;
    st->items[st->count].key = strdup(key);
    st->items[st->count].value = strdup(value);
    st->count++;
}

static prefs_store *prefs_open(const char *name) {
    pthread_mutex_lock(&prefs_lock);
    prefs_store *st = stores;
    while (st && strcmp(st->name, name) != 0) st = st->next;
    if (!st) {
        st = calloc(1, sizeof(*st));
        snprintf(st->name, sizeof(st->name), "%s", name);
        st->next = stores;
        stores = st;
        char path[512];
        prefs_path(st, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (f) {
            char line[4096];
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                char *tab = strchr(line, '\t');
                if (strlen(line) < 3 || line[1] != ' ' || !tab) continue;
                *tab = 0;
                prefs_put_raw(st, line[0], line + 2, tab + 1);
            }
            fclose(f);
        }
        droid_log("java: SharedPreferences %s (%d saved values)", name, st->count);
    }
    pthread_mutex_unlock(&prefs_lock);
    return st;
}

static void prefs_save(prefs_store *st) {
    char path[512];
    char dir[300];
    mkdir(data_path(dir, sizeof(dir), "prefs"), 0777);
    prefs_path(st, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < st->count; i++)
        fprintf(f, "%c %s\t%s\n", st->items[i].type, st->items[i].key, st->items[i].value);
    fclose(f);
}

static const pref *prefs_find(prefs_store *st, const char *key) {
    for (int i = 0; i < st->count; i++)
        if (strcmp(st->items[i].key, key) == 0) return &st->items[i];
    return NULL;
}

static prefs_store *store_of(jobject self) {
    jobj *o = as_obj(self);
    return o ? (prefs_store *)o->asset : NULL;
}

static jvalue sp_get(jobject self, const jvalue *a) {
    (void)a;
    jobj *p = new_obj("android/content/SharedPreferences");
    p->asset = prefs_open(self ? java_text(a[0].l) : "prefs");
    return L(p);
}
static jvalue sp_get_activity(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jobj *p = new_obj("android/content/SharedPreferences");
    p->asset = prefs_open("activity");
    return L(p);
}
static jvalue sp_edit(jobject self, const jvalue *a) {
    (void)a;
    jobj *e = new_obj("android/content/SharedPreferences$Editor");
    e->asset = store_of(self);
    return L(e);
}

// A value is read back only as the type it was saved with: on Android getString of an int
// throws (ClassCastException) and Unity's PlayerPrefs gives the default. (Subway read its
// int language setting as the culture "1".)
#define SP_FIND(t)                                                  \
    prefs_store *st = store_of(self);                               \
    const pref *p = st ? prefs_find(st, java_text(a[0].l)) : NULL; \
    if (p && p->type != (t)) p = NULL
static jvalue sp_get_int(jobject self, const jvalue *a) { SP_FIND('i'); return I(p ? atoi(p->value) : a[1].i); }
static jvalue sp_get_long(jobject self, const jvalue *a) { SP_FIND('l'); return J(p ? strtoll(p->value, NULL, 10) : a[1].j); }
static jvalue sp_get_float(jobject self, const jvalue *a) {
    SP_FIND('f');
    jvalue v;
    v.j = 0;
    v.f = p ? strtof(p->value, NULL) : a[1].f;
    return v;
}
static jvalue sp_get_bool(jobject self, const jvalue *a) { SP_FIND('b'); return Z(p ? p->value[0] == '1' : a[1].z); }
static jvalue sp_get_string(jobject self, const jvalue *a) { SP_FIND('s'); return L(p ? jstr(p->value) : a[1].l); }
static jvalue sp_contains(jobject self, const jvalue *a) {
    prefs_store *st = store_of(self);
    return Z(st && prefs_find(st, java_text(a[0].l)) != NULL);
}

static void ed_put(jobject self, const jvalue *a, char type, const char *value) {
    prefs_store *st = store_of(self);
    if (!st) return;
    pthread_mutex_lock(&prefs_lock);
    prefs_put_raw(st, type, java_text(a[0].l), value);
    pthread_mutex_unlock(&prefs_lock);
}
static jvalue ed_put_int(jobject self, const jvalue *a) {
    char t[24];
    snprintf(t, sizeof(t), "%d", a[1].i);
    ed_put(self, a, 'i', t);
    return L(self);
}
static jvalue ed_put_long(jobject self, const jvalue *a) {
    char t[32];
    snprintf(t, sizeof(t), "%lld", (long long)a[1].j);
    ed_put(self, a, 'l', t);
    return L(self);
}
static jvalue ed_put_float(jobject self, const jvalue *a) {
    char t[48];
    snprintf(t, sizeof(t), "%.9g", (double)a[1].f);
    ed_put(self, a, 'f', t);
    return L(self);
}
static jvalue ed_put_bool(jobject self, const jvalue *a) {
    ed_put(self, a, 'b', a[1].z ? "1" : "0");
    return L(self);
}
static jvalue ed_put_string(jobject self, const jvalue *a) {
    ed_put(self, a, 's', a[1].l ? java_text(a[1].l) : "");
    return L(self);
}
// Mortar's KeyStore (Halfbrick's Java: a key-value store kept on the device): a store of ours,
// saved like SharedPreferences (prefs/MortarKeyStore.txt). Jetpack Joyride Classic reads it while it loads.
static jvalue mortar_ks_get(jobject self, const jvalue *a) { // static String GetValue(String key)
    (void)self;
    prefs_store *st = prefs_open("MortarKeyStore");
    const char *key = java_text(a[0].l);
    pthread_mutex_lock(&prefs_lock);
    const pref *p = key ? prefs_find(st, key) : NULL;
    char *v = p ? strdup(p->value) : NULL;
    pthread_mutex_unlock(&prefs_lock);
    jvalue r = L(v ? jstr(v) : NULL);
    free(v);
    return r;
}
static jvalue mortar_ks_set(jobject self, const jvalue *a) { // static boolean SetValue(String key, String value)
    (void)self;
    prefs_store *st = prefs_open("MortarKeyStore");
    const char *key = java_text(a[0].l);
    if (!key) return Z(JNI_FALSE);
    pthread_mutex_lock(&prefs_lock);
    prefs_put_raw(st, 's', key, a[1].l ? java_text(a[1].l) : "");
    prefs_save(st);
    pthread_mutex_unlock(&prefs_lock);
    return Z(JNI_TRUE);
}

// Mortar's Halfbrick ID: HBSupport.RequestHBID() asks for it, Java answers later through the game's
// GooglePlayServicesUtility.OnHBIDResult(String) (Jetpack Joyride Classic's loading waits for it).
// The answer: an id of this console, made once and kept (MortarKeyStore "hydroid_hbid"); the call
// itself is made by mortar_host.c on its next frame, on the game's thread, as Java would.
volatile int droid_mortar_hbid_wanted;
static jvalue mortar_request_hbid(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_mortar_hbid_wanted = 1;
    droid_log("java: mortar RequestHBID: answered on the next frame");
    return V();
}
// GooglePlaySignIn.SignIn(activity): no Google Play Games here - started, then failed (a phone
// without it); mortar_host.c calls the game's OnSignInStarted / OnSignInFailed on its next frame.
volatile int droid_mortar_signin_wanted; // 1 the quiet sign-in, 2 the manual one (a button / a retry)
static jvalue mortar_sign_in_manually(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_mortar_signin_wanted |= 2;
    droid_log("java: mortar GooglePlaySignIn.SignInManually: no Google Play Games (fails on the next frame)");
    return V();
}
static jvalue mortar_sign_in(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_mortar_signin_wanted |= 1;
    droid_log("java: mortar GooglePlaySignIn.SignIn: no Google Play Games (fails on the next frame)");
    return V();
}
// GooglePlaySnapshotManager.Open(context, name, create) -> a request id; Java answers later through
// the game's OnSnapshotOpened(id, result). No Google Play Games here: the result says isSuccess false
// (every other field null) and the game keeps its save on the console (Jetpack Joyride Classic's
// UserManager waited for this answer: its loading screen forever). mortar_host.c sends it.
#define MORTAR_ANSWERS 32
volatile int droid_mortar_answer_kind[MORTAR_ANSWERS], droid_mortar_answer_id[MORTAR_ANSWERS], droid_mortar_answers;
static int mortar_request_next;
static jvalue mortar_queue(int kind, const char *what) { // a request id; its answer queued for mortar_host.c
    const int id = ++mortar_request_next;
    if (droid_mortar_answers < MORTAR_ANSWERS) {
        droid_mortar_answer_kind[droid_mortar_answers] = kind;
        droid_mortar_answer_id[droid_mortar_answers] = id;
        droid_mortar_answers++;
    }
    droid_log("java: mortar %s: request %d, fails on the next frame (no Google Play Games)", what, id);
    jvalue r;
    r.j = 0;
    r.i = id;
    return r;
}
static jvalue mortar_snapshot_open(jobject self, const jvalue *a) { // (context, name, create)
    (void)self;
    (void)a;
    return mortar_queue(1, "GooglePlaySnapshotManager.Open");
}
// GooglePlayAchievementsManager.Load(activity, force) -> a request id, answered through the game's
// OnLoadCompleted(id, result): isSuccessful false, achievements an empty List (the game reads its
// size() whatever the result: null would crash). Jetpack Joyride Classic's menu waited on it.
static jvalue mortar_achievements_load(jobject self, const jvalue *a) {
    (void)self;
    (void)a;
    return mortar_queue(2, "GooglePlayAchievementsManager.Load");
}
static jobj *new_list(const char *cls); // (below: the java/util lists)
jobject droid_mortar_achievements_failed(void) {
    jobj *r = new_obj("com/halfbrick/mortar/AchievementsLoadResult");
    r->field[1].l = (jobject)new_list("java/util/ArrayList");
    return (jobject)r;
}
jobject droid_mortar_snapshot_failed(void) { // the result: isSuccess false, nothing else
    return (jobject)new_obj("com/halfbrick/mortar/SnapshotOpenResult");
}

// FirebaseAuth.getCurrentUser(): the stubs' empty FirebaseUser meant "signed in" - Jetpack Joyride
// Classic then asked for its token (getIdToken, a Task never done) and its Halfbrick+ check kept the
// title menu behind its "processing" overlay. There: null, nobody signed in (an offline phone). Every
// other game keeps the empty user it had (Fruit Ninja plays with it).
static jvalue fb_current_user(jobject self, const jvalue *a) {
    (void)self; (void)a;
    if (java_package && !strncmp(java_package, "com.halfbrick.jetpack", 21)) return L(NULL);
    return L((jobject)new_obj("com/google/firebase/auth/FirebaseUser"));
}

jobject droid_mortar_hbid(void) {
    prefs_store *st = prefs_open("MortarKeyStore");
    char id[40];
    pthread_mutex_lock(&prefs_lock);
    const pref *p = prefs_find(st, "hydroid_hbid");
    if (p) {
        snprintf(id, sizeof(id), "%s", p->value);
    } else {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        uint64_t x = (uint64_t)t.tv_sec * 1000000007u ^ (uint64_t)t.tv_nsec ^ 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < 32; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            id[i] = "0123456789abcdef"[x & 15];
        }
        id[32] = 0;
        prefs_put_raw(st, 's', "hydroid_hbid", id);
        prefs_save(st);
    }
    pthread_mutex_unlock(&prefs_lock);
    return jstr(id);
}

static jvalue ed_remove(jobject self, const jvalue *a) {
    prefs_store *st = store_of(self);
    if (st) {
        pthread_mutex_lock(&prefs_lock);
        for (int i = 0; i < st->count; i++)
            if (strcmp(st->items[i].key, java_text(a[0].l)) == 0) {
                free(st->items[i].key);
                free(st->items[i].value);
                st->items[i] = st->items[--st->count];
                break;
            }
        pthread_mutex_unlock(&prefs_lock);
    }
    return L(self);
}
static jvalue ed_clear(jobject self, const jvalue *a) {
    (void)a;
    prefs_store *st = store_of(self);
    if (st) {
        pthread_mutex_lock(&prefs_lock);
        for (int i = 0; i < st->count; i++) {
            free(st->items[i].key);
            free(st->items[i].value);
        }
        st->count = 0;
        pthread_mutex_unlock(&prefs_lock);
    }
    return L(self);
}
static jvalue ed_apply(jobject self, const jvalue *a) {
    (void)a;
    prefs_store *st = store_of(self);
    if (st) {
        pthread_mutex_lock(&prefs_lock);
        prefs_save(st);
        pthread_mutex_unlock(&prefs_lock);
    }
    return V();
}
static jvalue ed_commit(jobject self, const jvalue *a) {
    ed_apply(self, a);
    return Z(JNI_TRUE);
}

// getAll(): Map -> entrySet() -> iterator() -> Map.Entry (key, boxed value)
static jvalue sp_get_all(jobject self, const jvalue *a) {
    (void)a;
    jobj *m = new_obj("java/util/Map");
    m->asset = store_of(self);
    return L(m);
}
static jvalue m_entries(jobject self, const jvalue *a);
static jvalue l_iterator(jobject self, const jvalue *a);
static jvalue map_entry_set(jobject self, const jvalue *a) {
    if (as_obj(self) && as_obj(self)->mark == 0x4d415020) return m_entries(self, a); // a HashMap / Bundle
    (void)a;
    jobj *s = new_obj("java/util/Set");
    s->asset = as_obj(self) ? as_obj(self)->asset : NULL;
    return L(s);
}
static jvalue set_iterator(jobject self, const jvalue *a) {
    if (as_obj(self) && (as_obj(self)->mark == 0x56494557 || as_obj(self)->mark == 0x4c495354)) return l_iterator(self, a);
    (void)a;
    jobj *it = new_obj("java/util/Iterator");
    it->asset = as_obj(self) ? as_obj(self)->asset : NULL;
    it->mark = 0; // next index
    return L(it);
}
static int view_count(jobj *it);
static jobject view_item(jobj *it, int n);
static jvalue it_has_next(jobject self, const jvalue *a) {
    (void)a;
    jobj *it = as_obj(self);
    if (it && it->field[1].i == 1) return Z(it->mark < view_count(it)); // a list / map's
    prefs_store *st = it ? (prefs_store *)it->asset : NULL;
    return Z(st && it->mark < st->count);
}
static jvalue it_next(jobject self, const jvalue *a) {
    (void)a;
    jobj *it = as_obj(self);
    if (it && it->field[1].i == 1) return L(it->mark < view_count(it) ? view_item(it, (int)it->mark++) : NULL);
    prefs_store *st = it ? (prefs_store *)it->asset : NULL;
    if (!st || it->mark >= st->count) return L(NULL);
    jobj *e = new_obj("java/util/Map$Entry");
    e->asset = &st->items[it->mark++];
    return L(e);
}
static jvalue entry_key(jobject self, const jvalue *a) {
    (void)a;
    jobj *e = as_obj(self);
    if (e && !e->asset) return L(e->field[0].l); // a map's entry
    return L(e ? jstr(((pref *)e->asset)->key) : NULL);
}
static jvalue entry_value(jobject self, const jvalue *a) {
    (void)a;
    jobj *e = as_obj(self);
    if (e && !e->asset) return L(e->field[1].l); // a map's entry
    if (!e) return L(NULL);
    const pref *p = e->asset;
    if (p->type == 's') return L(jstr(p->value));
    const char *cls = p->type == 'i' ? "java/lang/Integer" : p->type == 'l' ? "java/lang/Long"
                    : p->type == 'f' ? "java/lang/Float" : "java/lang/Boolean";
    jobj *box = new_obj(cls);
    box->buf = strdup(p->value);
    return L(box);
}
static const char *box_text(jobject self) {
    jobj *b = as_obj(self);
    return b && b->buf ? b->buf : "0";
}
static jvalue box_int(jobject self, const jvalue *a) { (void)a; return I(atoi(box_text(self))); }
static jvalue box_long(jobject self, const jvalue *a) { (void)a; return J(strtoll(box_text(self), NULL, 10)); }
static jvalue box_float(jobject self, const jvalue *a) {
    (void)a;
    jvalue v;
    v.j = 0;
    v.f = strtof(box_text(self), NULL);
    return v;
}
static jvalue box_bool(jobject self, const jvalue *a) { (void)a; return Z(box_text(self)[0] == '1'); }

// ---- com.google.androidgamesdk.ChoreographerCallback: vsync for Unity's frame pacer ------
// Native code makes one with a cookie and posts it; at the next 60 Hz tick our
// "Choreographer" thread calls the native nOnChoreographer(cookie, frameTimeNanos).

void *jni_find_native(const char *name); // jni_impl.c (RegisterNatives)
void *arm_jni_env(void);                 // arm_imports.c

static jobject proxy_call(jobject proxy, const char *method, int argc, const jobject *args);
static jobject box_long_new(int64_t v);

#define MAX_FRAME_CBS 64
static jobject frame_cbs[MAX_FRAME_CBS];
static int64_t frame_due[MAX_FRAME_CBS]; // ns
static int frame_cb_count;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t frame_once = PTHREAD_ONCE_INIT;

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// The screen's refresh, as the game's swaps see it: a swap with interval 1 returns
// just after a vblank (gles_bridge.c tells us). The ticks keep to that rhythm, not
// to a clock of their own (two clocks drifting: frames shown 1 or 3 refreshes, judder).
#define VSYNC_NS 16666667LL
static volatile int64_t vsync_seen; // ns of the last vblank a swap waited for

void java_vsync(int64_t t) { vsync_seen = t; }

// The PS5 screen's own vblank (gles_bridge.c sets it once the screen is up): the
// ticks wait for it - 60 a second (every second vblank at 120 Hz). NULL: our clock.
int (*droid_wait_vblank)(void);
volatile int64_t droid_vblank_ns; // the last real vblank the Choreographer saw (MONOTONIC ns)
int droid_vblank_hz = 60;

// the first tick after now, on the vblank grid (+1 ms: work starts right after it)
static int64_t next_tick(int64_t prev) {
    int64_t now = now_ns(), v = vsync_seen;
    if (!v || now - v > 1000000000LL) return prev + VSYNC_NS; // no swaps lately: own clock
    int64_t at = v + 1000000;
    if (at <= now) at += ((now - at) / VSYNC_NS + 1) * VSYNC_NS;
    if (at - prev < VSYNC_NS / 2) at += VSYNC_NS; // never two ticks in one refresh
    return at;
}

// The tick's frame time, as Android's Choreographer gives it: on the screen's rhythm,
// not the moment our thread woke (that wobbles a few ms, and Unity's 30 fps pacer then
// takes 35 ms for one frame, 31 for the next). A slow follower of the real ticks.
static int64_t smooth_tick(int64_t woke) {
    static int64_t t, period;
    if (!t || woke - t > 4 * VSYNC_NS || woke < t) { // first, or after a gap: start again
        t = woke;
        period = VSYNC_NS;
        return t;
    }
    int64_t ahead = t + period, err = woke - ahead;
    int64_t steps = 1;
    while (err > period / 2 && steps < 4) { // a tick missed: as many periods on
        ahead += period;
        err -= period;
        steps++;
    }
    t = ahead + err / 16;    // follow slowly: the wobble goes, the real rhythm stays
    period += err / 256;     // and learn the true period (59.94 Hz or 60)
    if (period < VSYNC_NS * 9 / 10) period = VSYNC_NS * 9 / 10;
    if (period > VSYNC_NS * 11 / 10) period = VSYNC_NS * 11 / 10;
    return t;
}

static void *choreographer_thread(void *arg) {
    (void)arg;
    typedef void (*on_frame_fn)(void *env, jobject self, jlong cookie, jlong nanos);
    on_frame_fn on_frame = NULL;
    int64_t next = now_ns();
    arm_thread_role("Choreographer");
    for (;;) {
        arm_sig_point(); // the game's GC pause, between ticks
        int (*vblank)(void) = droid_wait_vblank;
        int on_vblank = 0;
        // vblanks per tick: the screen's rate as measured. Vulkan games' screen is taken for 60 Hz
        // (gles_bridge.c droid_screen_ready), but the PS5 may run it at 120: a tick at each vblank
        // was then 120 a second with frame times 16.7 ms apart, and Unity's pacer made 60 fps that
        // looked like 30 (2026-10-31, Subway Surfers). Of each 120 vblank waits: over 40% under
        // 11.5 ms apart is 120 Hz (two vblanks a tick), none 60 Hz.
        static int per_tick; // 0: as droid_vblank_hz says
        static int64_t last_v, shorts, waits;
        if (vblank) {
            on_vblank = 1;
            const int per = per_tick ? per_tick : droid_vblank_hz >= 120 ? 2 : 1;
            for (int i = 0; i < per; i++) {
                if (vblank() != 0) {
                    droid_log("java: vblank wait failed: the Choreographer keeps its own clock");
                    droid_wait_vblank = NULL;
                    on_vblank = 0;
                    break;
                }
                const int64_t tv = now_ns(), d = last_v ? tv - last_v : 0;
                last_v = tv;
                if (d > 2000000 && d < 40000000) {
                    shorts += d < 11500000;
                    if (++waits == 120) {
                        const int p = shorts * 100 > waits * 40 ? 2 : 1;
                        if (p != per_tick)
                            droid_log("java: the screen refreshes at %s: the Choreographer ticks every %d vblank(s), 60 a second",
                                      p == 2 ? "120 Hz" : "60 Hz", p);
                        per_tick = p;
                        shorts = waits = 0;
                    }
                }
            }
            if (on_vblank) {
                int64_t at = now_ns();
                static int64_t first_at, count;
                if (!first_at) first_at = at;
                if (++count % 600 == 0) // the screen's real refresh, measured
                    droid_log("java: vblank: %.2f Hz measured over %lld waits",
                              count * 1e9 / (double)(at - first_at), (long long)count);
                droid_vblank_ns = at;
            }
            next = smooth_tick(now_ns());
        }
        if (!on_vblank) {
            next = next_tick(next);
            int64_t wait = next - now_ns();
            if (wait > 0) {
                struct timespec ts = { (time_t)(wait / 1000000000LL), (long)(wait % 1000000000LL) };
                nanosleep(&ts, NULL);
            } else {
                next = now_ns();
            }
        }
        if (!on_frame) on_frame = (on_frame_fn)jni_find_native("nOnChoreographer");
        static unsigned long ticks;
        if (++ticks % 120 == 1) droid_log("java: Choreographer tick %lu, %d callback(s) waiting", ticks, frame_cb_count);
        jobject due[MAX_FRAME_CBS];
        int n = 0;
        pthread_mutex_lock(&frame_lock);
        for (int i = 0; i < frame_cb_count;) { // a posted callback fires once
            if (frame_due[i] <= next) {
                due[n++] = frame_cbs[i];
                frame_cbs[i] = frame_cbs[--frame_cb_count];
                frame_due[i] = frame_due[frame_cb_count];
            } else {
                i++;
            }
        }
        pthread_mutex_unlock(&frame_lock);
        for (int i = 0; i < n; i++) {
            jobj *cb = as_obj(due[i]);
            if (cb && strcmp(cb->cls, "com/google/androidgamesdk/ChoreographerCallback") == 0) {
                if (on_frame) on_frame(arm_jni_env(), due[i], (jlong)cb->mark, (jlong)next);
            } else { // a FrameCallback proxy: doFrame(long frameTimeNanos)
                jobject t = box_long_new(next);
                proxy_call(due[i], "doFrame", 1, &t);
            }
        }
    }
    return NULL;
}

static void start_choreographer(void) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 2 << 20);
    pthread_t t;
    pthread_create(&t, &a, choreographer_thread, NULL);
    droid_log("java: Choreographer running (60 Hz)");
}

static void post_frame(jobject self, int64_t delay_ns) {
    pthread_once(&frame_once, start_choreographer);
    pthread_mutex_lock(&frame_lock);
    if (frame_cb_count < MAX_FRAME_CBS) {
        frame_cbs[frame_cb_count] = self;
        frame_due[frame_cb_count] = now_ns() + delay_ns;
        frame_cb_count++;
    }
    pthread_mutex_unlock(&frame_lock);
}

static jvalue cc_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *cb = new_obj("com/google/androidgamesdk/ChoreographerCallback");
    cb->mark = a[0].j; // the cookie
    droid_log("java: ChoreographerCallback(cookie %#llx)", (unsigned long long)a[0].j);
    return L(cb);
}
static jvalue cc_post(jobject self, const jvalue *a) { (void)a; post_frame(self, 0); return V(); }
static jvalue cc_post_delayed(jobject self, const jvalue *a) { post_frame(self, a[0].j * 1000000LL); return V(); }

// ---- Unity's Java proxies: interface objects whose methods run native code --------------
// ReflectionHelper.newProxyInstance(player, ptr, iface): a call to the proxy's method
// is nativeProxyInvoke(ptr, methodName, args) (registered by libunity).

static jclass find_class(const char *name);

static jvalue new_proxy(jobject self, const jvalue *a) {
    (void)self;
    jobj *p = new_obj("java/lang/reflect/Proxy");
    p->mark = a[1].j; // Unity's handle
    return L(p);
}

// JNIBridge.newInterfaceProxy(ptr, Class[] interfaces): its calls go to the native
// JNIBridge.invoke(ptr, interface, Method, args); the Method object only has a name.
static jvalue new_bridge_proxy(jobject self, const jvalue *a) {
    (void)self;
    jobj *p = new_obj("java/lang/reflect/Proxy");
    p->mark = a[0].j;
    p->asset = (void *)1; // the JNIBridge kind
    // all its interfaces (Unity's UnityChoreographer one is Handler.Callback + FrameCallback)
    int n = a[1].l ? jni->GetArrayLength(&jni, (jarray)a[1].l) : 0;
    jobject *ifs = calloc(n > 0 ? n : 1, sizeof(jobject));
    for (int i = 0; i < n; i++) ifs[i] = jni->GetObjectArrayElement(&jni, (jobjectArray)a[1].l, i);
    p->ref = n > 0 ? ifs[0] : NULL;
    p->buf = (char *)ifs;
    p->len = n;
    return L(p);
}
// ---- interface methods: real IDs, calls go to the proxy ------------------------------------
// Callbacks / listeners / Runnable have no code here; an ID per (class, name, signature)
// makes a call on one of Unity's proxies reach the proxy (boxed args, unboxed result),
// and FromReflectedMethod(Method) maps the proxy's Method back to the same ID.

typedef struct {
    char cls[96], name[64], sig[160];
    jobject reflected; // its one java.lang.reflect.Method (Unity compares them by identity)
    int stub;          // a method we have no code for (java_stub_method): it answers 0 / null
} dyn_method;
#define MAX_DYN 4096
static dyn_method dyn[MAX_DYN];
static int dyn_count;
static pthread_mutex_t dyn_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_interface_like(const char *cls) {
    return strcmp(cls, "java/lang/Runnable") == 0 || strstr(cls, "Callback") || strstr(cls, "Listener");
}

jmethodID java_dynamic_method(jclass c, const char *name, const char *sig) {
    const char *cls = !placeholder(c) ? (const char *)c : NULL;
    if (!cls || java_is_string((jobject)c) || !is_interface_like(cls) || name[0] == '<') return NULL;
    pthread_mutex_lock(&dyn_lock);
    dyn_method *found = NULL;
    for (int i = 0; i < dyn_count && !found; i++) {
        if (strcmp(dyn[i].cls, cls) != 0 || strcmp(dyn[i].name, name) != 0) continue;
        if (!dyn[i].sig[0]) snprintf(dyn[i].sig, sizeof(dyn[i].sig), "%s", sig); // made by FromReflectedMethod
        if (strcmp(dyn[i].sig, sig) == 0) found = &dyn[i];
    }
    if (!found && dyn_count < MAX_DYN) {
        found = &dyn[dyn_count++];
        snprintf(found->cls, sizeof(found->cls), "%s", cls);
        snprintf(found->name, sizeof(found->name), "%s", name);
        snprintf(found->sig, sizeof(found->sig), "%s", sig);
        droid_log("java: interface method %s.%s%s (calls go to proxies)", cls, name, sig);
    }
    pthread_mutex_unlock(&dyn_lock);
    return (jmethodID)found;
}

// A Java method we have no code for: an ID anyway, as on a phone, where it exists (plugins
// like Firebase look up dozens at start and give up on a missing one). Called, it does
// nothing and answers 0 / null (logged, the first time each).
jmethodID java_stub_method(jclass c, const char *name, const char *sig) {
    const char *cls = c && !placeholder(c) && !java_is_string((jobject)c) ? (const char *)c : "?";
    pthread_mutex_lock(&dyn_lock);
    dyn_method *found = NULL;
    for (int i = 0; i < dyn_count && !found; i++)
        if (dyn[i].stub && strcmp(dyn[i].cls, cls) == 0 && strcmp(dyn[i].name, name) == 0 &&
            strcmp(dyn[i].sig, sig) == 0)
            found = &dyn[i];
    if (!found && dyn_count < MAX_DYN) {
        found = &dyn[dyn_count++];
        snprintf(found->cls, sizeof(found->cls), "%s", cls);
        snprintf(found->name, sizeof(found->name), "%s", name);
        snprintf(found->sig, sizeof(found->sig), "%s", sig);
        found->stub = 1;
    }
    pthread_mutex_unlock(&dyn_lock);
    return (jmethodID)found;
}

static int is_dynamic(jmethodID id) {
    return (const dyn_method *)id >= dyn && (const dyn_method *)id < dyn + MAX_DYN;
}

// The Method object of an interface method: made once, the same one every time.
static jobject dyn_reflected(dyn_method *d) {
    if (!d->reflected) {
        jobj *m = new_obj("java/lang/reflect/Method");
        m->buf = strdup(d->name);
        m->ref = find_class(d->cls); // its interface
        d->reflected = (jobject)m;
    }
    return d->reflected;
}

// (iface, name) -> that interface method (made if Unity hasn't asked for it yet)
static dyn_method *dyn_find(const char *iface, const char *name) {
    dyn_method *d = NULL;
    pthread_mutex_lock(&dyn_lock);
    for (int i = 0; i < dyn_count && !d; i++)
        if (strcmp(dyn[i].name, name) == 0 && (!iface || strcmp(dyn[i].cls, iface) == 0)) d = &dyn[i];
    if (!d && iface && dyn_count < MAX_DYN) {
        d = &dyn[dyn_count++];
        snprintf(d->cls, sizeof(d->cls), "%s", iface);
        snprintf(d->name, sizeof(d->name), "%s", name);
        d->sig[0] = 0;
    }
    pthread_mutex_unlock(&dyn_lock);
    return d;
}

// ToReflectedMethod(cls, id): an interface method's own Method object
jobject java_to_reflected(jmethodID id) {
    return is_dynamic(id) ? dyn_reflected((dyn_method *)id) : NULL;
}

static jvalue method_equals(jobject self, const jvalue *a) { return Z(self == a[0].l); }
static jvalue method_hash(jobject self, const jvalue *a) { (void)a; return I((jint)((uintptr_t)self >> 4)); }

// ---- java.io.File: a path (a String, as getFilesDir gives); FileOutputStream: an fd ---------
static jvalue file_new(jobject self, const jvalue *a) { // (String)
    (void)self;
    return L(jstr(cstr(a[0].l) ? cstr(a[0].l) : ""));
}
// a File: its path ("file:<path>" is how getObbDirs and others mark one - Genshin Impact took
// "file:" for part of the path, walked up it and never reached "/": out of stack)
static const char *fpath(jobject o) {
    const char *p = cstr(o);
    return p && !strncmp(p, "file:", 5) ? p + 5 : p;
}
static jvalue file_new_child(jobject self, const jvalue *a) { // (File / String parent, String name)
    (void)self;
    const char *dir = fpath(a[0].l), *name = cstr(a[1].l);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir ? dir : "", name ? name : "");
    return L(jstr(path));
}
static jvalue file_path(jobject self, const jvalue *a) { // (a "file:" one: its path without the mark)
    (void)a;
    const char *p = cstr(self), *f = fpath(self);
    return L(p && f != p ? jstr(f) : self);
}
static jvalue file_exists(jobject self, const jvalue *a) {
    (void)a;
    struct stat st;
    return Z(fpath(self) && stat(fpath(self), &st) == 0);
}
static jvalue file_is_dir(jobject self, const jvalue *a) {
    (void)a;
    struct stat st;
    return Z(fpath(self) && stat(fpath(self), &st) == 0 && S_ISDIR(st.st_mode));
}
static jvalue file_length(jobject self, const jvalue *a) {
    (void)a;
    struct stat st;
    jvalue v;
    v.j = fpath(self) && stat(fpath(self), &st) == 0 ? (jlong)st.st_size : 0;
    return v;
}
static jvalue file_delete(jobject self, const jvalue *a) {
    (void)a;
    const char *p = fpath(self);
    return Z(p && (unlink(p) == 0 || rmdir(p) == 0));
}
static jvalue file_mkdirs(jobject self, const jvalue *a) {
    (void)a;
    const char *p = fpath(self);
    if (!p) return Z(0);
    char t[1024];
    snprintf(t, sizeof(t), "%s", p);
    for (char *c = t + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(t, 0777); *c = '/'; }
    return Z(mkdir(t, 0777) == 0);
}
static jvalue file_mkdir(jobject self, const jvalue *a) { (void)a; return Z(fpath(self) && mkdir(fpath(self), 0777) == 0); }

static jobject fos_open(const char *path, int append) {
    int fd = path ? open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644) : -1;
    static int logged;
    if (logged++ < 20) droid_log("java: FileOutputStream %s -> fd %d", path ? path : "(null)", fd);
    jobj *o = new_obj("java/io/FileOutputStream");
    o->mark = fd;
    return (jobject)o;
}
static jvalue fos_new(jobject self, const jvalue *a) { (void)self; return L(fos_open(cstr(a[0].l), 0)); }
static jvalue fos_new_append(jobject self, const jvalue *a) { (void)self; return L(fos_open(cstr(a[0].l), a[1].z)); }
static jvalue fos_write_part(jobject self, const jvalue *a) { // ([B, offset, count)
    jobj *o = as_obj(self);
    if (!o || o->mark < 0 || !a[0].l || a[2].i <= 0) return V();
    jbyte *b = jni->GetByteArrayElements(&jni, (jbyteArray)a[0].l, NULL);
    if (b) {
        if (write((int)o->mark, b + a[1].i, (size_t)a[2].i) < 0) droid_log("java: FileOutputStream write failed");
        jni->ReleaseByteArrayElements(&jni, (jbyteArray)a[0].l, b, JNI_ABORT);
    }
    return V();
}
static jvalue fos_write_all(jobject self, const jvalue *a) { // ([B)
    jvalue b[3];
    b[0] = a[0];
    b[1].i = 0;
    b[2].i = a[0].l ? jni->GetArrayLength(&jni, (jarray)a[0].l) : 0;
    return fos_write_part(self, b);
}
static jvalue fos_write_byte(jobject self, const jvalue *a) {
    jobj *o = as_obj(self);
    unsigned char c = (unsigned char)a[0].i;
    if (o && o->mark >= 0 && write((int)o->mark, &c, 1) < 0) droid_log("java: FileOutputStream write failed");
    return V();
}
static jvalue fos_close(jobject self, const jvalue *a) {
    (void)a;
    jobj *o = as_obj(self);
    if (o && o->mark >= 0) {
        close((int)o->mark);
        o->mark = -1;
    }
    return V();
}

// Subway Surfers' save-data helper (KilooPlatformAndroidBridge): where its user data lives
static jvalue userdata_path(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char p[600];
    return L(jstr(data_path(p, sizeof(p), "files")));
}

// Subway Surfers' vibration (com.sybo.nativevibration.VibrationPlugin): on the DualSense's motors.
// Everything "supported" (it asks only about custom waveforms, and with a no it never vibrates).
void droid_pad_rumble(float strength, int ms); // app_main.c
static jvalue vib_yes(jobject self, const jvalue *a) { // (the game asked: logged once)
    (void)self; (void)a;
    static int told;
    if (!told++) droid_log("java: vibration: the game asked whether it's supported: yes");
    return Z(1);
}
static jvalue vib_ms(jobject self, const jvalue *a) { // Vibrate(int ms)
    (void)self;
    const int ms = a[0].i < 10 ? 10 : a[0].i > 1000 ? 1000 : a[0].i;
    static int told;
    if (told++ < 5) droid_log("java: vibration: %d ms", a[0].i);
    droid_pad_rumble(0.6f, ms);
    return V();
}
// VibrateWithWaveForm(long[] timings, int[] amplitudes, int repeat): Android's waveform - segments of
// a length (ms) and a strength (0-255, -1 the default). On the DualSense: one buzz as long as the
// pattern up to its last strong segment, at its strongest (a repeating one: once, 1 s at most).
static jvalue vib_wave(jobject self, const jvalue *a) {
    (void)self;
    const int n = a[0].l ? jni->GetArrayLength(&jni, (jarray)a[0].l) : 0;
    const int na = a[1].l ? jni->GetArrayLength(&jni, (jarray)a[1].l) : 0;
    const jlong *t = n ? jni->GetLongArrayElements(&jni, (jlongArray)a[0].l, NULL) : NULL;
    const jint *amp = na ? jni->GetIntArrayElements(&jni, (jintArray)a[1].l, NULL) : NULL;
    long total = 0, upto = 0;
    int strongest = 0;
    for (int i = 0; t && i < n; i++) {
        total += t[i] > 0 ? (long)t[i] : 0;
        const int s = amp && i < na ? (amp[i] < 0 ? 160 : amp[i]) : 160;
        if (s > 0) {
            upto = total;
            if (s > strongest) strongest = s;
        }
    }
    static int told;
    if (told++ < 8) droid_log("java: vibration waveform: %d segments, %ld ms on, strongest %d, repeat %d", n, upto, strongest, a[2].i);
    if (upto > 0 && strongest > 0) droid_pad_rumble(strongest / 255.0f, (int)(upto > 1000 ? 1000 : upto));
    return V();
}
static jvalue vib_effect(jobject self, const jvalue *a) { // VibrateWithPredefinedEffect(String)
    (void)self;
    const char *e = a[0].l ? cstr(a[0].l) : "";
    float s = 0.5f;
    int ms = 40;
    if (droid_strcasestr(e, "heavy")) s = 0.9f, ms = 70;
    else if (droid_strcasestr(e, "double")) s = 0.6f, ms = 90;
    else if (droid_strcasestr(e, "tick") || droid_strcasestr(e, "light") || droid_strcasestr(e, "soft")) s = 0.3f, ms = 20;
    static char told[8][32];
    for (int i = 0; i < 8; i++) { // each effect's name once in the log
        if (!strncmp(told[i], e, sizeof(told[i]) - 1)) break;
        if (!told[i][0]) {
            snprintf(told[i], sizeof(told[i]), "%s", e);
            droid_log("java: vibration effect \"%s\": %.1f for %d ms", e, s, ms);
            break;
        }
    }
    droid_pad_rumble(s, ms);
    return V();
}

// Android's own Vibrator (Unity's Handheld.Vibrate, Rovio's rumbler): on the DualSense's motors,
// for VIBRATOR_GAMES (per_game). VibrationEffect.createOneShot / createWaveform keep their length
// and strength; vibrate(effect) plays the last one made (games make it and play it at once).
#define VIBRATOR_GAMES "com.nekki.vector,com.rovio.baba"
static int vib_last_ms = 40;
static float vib_last_s = 0.6f;
static jvalue avib_has(jobject self, const jvalue *a) { // Vibrator.hasVibrator / hasAmplitudeControl
    (void)self; (void)a;
    static int told;
    if (!told++) droid_log("java: Vibrator: the game asked about the vibrator: yes (the DualSense)");
    return Z(1);
}
static jvalue avib_one_shot(jobject self, const jvalue *a) { // VibrationEffect.createOneShot(long ms, int amp)
    (void)self;
    vib_last_ms = a[0].j < 10 ? 10 : a[0].j > 1000 ? 1000 : (int)a[0].j;
    vib_last_s = a[1].i < 0 ? 0.6f : a[1].i / 255.0f;
    return L((jobject)new_obj("android/os/VibrationEffect"));
}
static jvalue avib_wave(jobject self, const jvalue *a) { // VibrationEffect.createWaveform(long[], int[]?, int)
    (void)self;
    const int n = a[0].l ? jni->GetArrayLength(&jni, (jarray)a[0].l) : 0;
    const jlong *t = n ? jni->GetLongArrayElements(&jni, (jlongArray)a[0].l, NULL) : NULL;
    long total = 0;
    for (int i = 0; t && i < n; i++) total += t[i] > 0 ? (long)t[i] : 0;
    vib_last_ms = total < 10 ? 10 : total > 1000 ? 1000 : (int)total;
    vib_last_s = 0.6f;
    return L((jobject)new_obj("android/os/VibrationEffect"));
}
static jvalue avib_vibrate(jobject self, const jvalue *a) { // vibrate(long ms) / vibrate(VibrationEffect...)
    (void)self;
    int ms = vib_last_ms;
    float st = vib_last_s;
    static int told;
    if (told++ < 10) droid_log("java: Vibrator.vibrate: %d ms at %.2f (the DualSense)", ms, st);
    droid_pad_rumble(st, ms);
    (void)a;
    return V();
}
static jvalue avib_vibrate_ms(jobject self, const jvalue *a) { // vibrate(long ms)
    vib_last_ms = a[0].j < 10 ? 10 : a[0].j > 1000 ? 1000 : (int)a[0].j;
    vib_last_s = 0.6f;
    return avib_vibrate(self, a);
}

// ---- the trusted root certificates (HTTPS): Android's Java trust store ----------------
// Unity's TLS asks Java for them: TrustManagerFactory -> getTrustManagers -> an
// X509TrustManager -> getAcceptedIssuers -> each X509Certificate's getEncoded (DER).
// Ours: Mozilla's list (curl's cacert.pem, MPL-2.0) in the app's assets/certs.
typedef struct {
    unsigned char *der;
    int len;
} root_cert;
static root_cert *roots;
static int root_count;

static int b64(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void load_roots_once(void) { // the PEM file, read once (load_roots)
    FILE *f = fopen(APP0 "/assets/certs/cacert.pem", "r");
    if (!f) {
        droid_log("java: no root certificates (assets/certs/cacert.pem): HTTPS servers won't be trusted");
        return;
    }
    char line[256];
    unsigned char *der = NULL;
    int len = 0, cap = 0, in = 0;
    unsigned bits = 0;
    int nbits = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "-----BEGIN CERTIFICATE-----")) {
            in = 1;
            len = 0;
            bits = 0;
            nbits = 0;
            cap = 4096;
            der = malloc((size_t)cap);
            continue;
        }
        if (strstr(line, "-----END CERTIFICATE-----")) {
            if (in && der) {
                root_cert *r = realloc(roots, (size_t)(root_count + 1) * sizeof(root_cert));
                if (r) {
                    roots = r;
                    roots[root_count].der = der;
                    roots[root_count].len = len;
                    root_count++;
                    der = NULL;
                }
            }
            free(der);
            der = NULL;
            in = 0;
            continue;
        }
        if (!in || !der) continue;
        for (char *c = line; *c; c++) {
            int v = b64((unsigned char)*c);
            if (v < 0) continue;
            bits = (bits << 6) | (unsigned)v;
            nbits += 6;
            if (nbits >= 8) {
                nbits -= 8;
                if (len == cap) {
                    unsigned char *g = realloc(der, (size_t)cap * 2);
                    if (!g) break;
                    der = g;
                    cap *= 2;
                }
                der[len++] = (unsigned char)(bits >> nbits);
            }
        }
    }
    free(der);
    fclose(f);
    droid_log("java: %d root certificates for HTTPS", root_count);
}

static void load_roots(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_roots_once);
}

static jvalue tmf_algorithm(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("PKIX")); }
static jvalue tmf_instance(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L((jobject)new_obj("javax/net/ssl/TrustManagerFactory"));
}
static jvalue tmf_trust_managers(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jobjectArray arr = jni->NewObjectArray(&jni, 1, NULL, NULL);
    jni->SetObjectArrayElement(&jni, arr, 0, (jobject)new_obj("javax/net/ssl/X509TrustManager"));
    return L((jobject)arr);
}
static jvalue tm_issuers(jobject self, const jvalue *a) {
    (void)self; (void)a;
    load_roots();
    jobjectArray arr = jni->NewObjectArray(&jni, root_count, NULL, NULL);
    for (int i = 0; i < root_count; i++) {
        jobj *c = new_obj("java/security/cert/X509Certificate");
        c->mark = i;
        jni->SetObjectArrayElement(&jni, arr, i, (jobject)c);
    }
    return L((jobject)arr);
}
static jvalue cert_encoded(jobject self, const jvalue *a) {
    (void)a;
    jobj *c = as_obj(self);
    if (!c || c->mark < 0 || c->mark >= root_count) return L(NULL);
    const root_cert *r = &roots[c->mark];
    jbyteArray b = jni->NewByteArray(&jni, r->len);
    jni->SetByteArrayRegion(&jni, b, 0, r->len, (const jbyte *)r->der);
    return L((jobject)b);
}

// ---- Uri.decode: %XX escapes -------------------------------------------------------------
static int b16(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static jvalue uri_decode(jobject self, const jvalue *a) {
    (void)self;
    const char *in = cstr(a[0].l);
    if (!in) return L(NULL);
    size_t n = strlen(in);
    char *out = malloc(n + 1);
    if (!out) return L(NULL);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        int hi = i + 2 < n + 1 && in[i] == '%' ? b16(in[i + 1]) : -1;
        int lo = hi >= 0 ? b16(in[i + 2]) : -1;
        if (lo >= 0) {
            out[k++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            out[k++] = in[i];
        }
    }
    out[k] = 0;
    jvalue v = L(jstr(out));
    free(out);
    return v;
}

// Uri.encode: everything but letters, digits and _-!.~'()* as %XX (UTF-8 bytes), as Android.
// (It answered its own class before: "android/net/Uri" for every string, and Unity's
// PlayerPrefs encodes every key with it, so all of a game's settings were one.)
static jvalue uri_encode(jobject self, const jvalue *a) {
    (void)self;
    const char *in = cstr(a[0].l);
    if (!in) return L(NULL);
    size_t n = strlen(in);
    char *out = malloc(n * 3 + 1);
    if (!out) return L(NULL);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("_-!.~'()*", c)) {
            out[k++] = (char)c;
        } else {
            out[k++] = '%';
            out[k++] = "0123456789ABCDEF"[c >> 4];
            out[k++] = "0123456789ABCDEF"[c & 15];
        }
    }
    out[k] = 0;
    jvalue v = L(jstr(out));
    free(out);
    return v;
}

// ---- a device ID: one per console, random, kept in the app's data -----------------------
// (Settings.Secure ANDROID_ID, and games' own helpers that read it: Shadow Fight 2's
// DeviceUniqueID.GetID came back empty)
static char the_id[17];

static void make_device_id(void) {
    FILE *f = fopen(APP0 "/data/device_id.txt", "r");
    if (f) {
        if (fscanf(f, "%16s", the_id) != 1) the_id[0] = 0;
        fclose(f);
    }
    if (strlen(the_id) == 16) return;
    unsigned long long x = (unsigned long long)time(NULL) * 6364136223846793005ull ^ (unsigned long long)(uintptr_t)&f;
    for (int i = 0; i < 16; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        the_id[i] = "0123456789abcdef"[(x >> 59) & 15];
    }
    the_id[16] = 0;
    f = fopen(APP0 "/data/device_id.txt", "w");
    if (f) {
        fprintf(f, "%s\n", the_id);
        fclose(f);
    }
    droid_log("java: device id %s (new)", the_id);
}

static const char *android_device_id(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, make_device_id);
    return the_id;
}

// Rovio's Beacon SDK (Angry Birds 2): com.rovio.beacon.DeviceInfo, as its Java gives them (read
// with Apktool): the files folder, and the device facts as JSON. Null for either threw in
// Beacon.DeviceInfo's type initializer, the SDK never started ("Oops! Something went wrong!").
static const char *android_device_id(void);
static jvalue beacon_data_path(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char p[300];
    game_path(p, sizeof(p), "files");
    mkdir(p, 0777);
    return L(jstr(p));
}
static jvalue beacon_platform_ids(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char j[1024];
    snprintf(j, sizeof(j),
             "{\"androidId\":\"%s\",\"buildId\":\"Build/RQ3A.211001.001\",\"installSource\":\"com.android.vending\","
             "\"bundleId\":\"%s\",\"carrierName\":\"\",\"networkType\":\"wifi\",\"installationTime\":1760000000000,"
             "\"lastUpdatedTime\":1760000000000,\"displayWidth\":1920,\"displayHeight\":1080,\"displayPPI\":160,"
             "\"locale\":\"en_US\",\"osName\":\"Android\",\"osVersion\":\"11\",\"deviceModel\":\"PlayStation 5\","
             "\"deviceManufacturer\":\"Sony\",\"isChromeOS\":false,\"isDemoMode\":false}",
             android_device_id(), java_package ? java_package : "");
    return L(jstr(j));
}

static jvalue device_id_answer(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr(android_device_id()));
}
// Vector 2's com/nekki/DeviceUniqueID.GetID (null: its startup stopped on a
// NotImplementedException after the server checks); its own entry, gated per game
static jvalue nekki_v2_device_id(jobject self, const jvalue *a) { return device_id_answer(self, a); }

// Settings.Secure.getString(resolver, name): android_id; others unknown (null)
static jvalue secure_string(jobject self, const jvalue *a) {
    (void)self;
    const char *name = cstr(a[1].l);
    return L(name && !strcmp(name, "android_id") ? jstr(android_device_id()) : NULL);
}

// ---- the language and region: English, US (a locale plugin's own answers) ---------------
static jvalue locale_language(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en")); }
static jvalue locale_region(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("US")); }
static jvalue locale_id(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en_US")); }
static jvalue locale_currency(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("USD")); }
static jvalue locale_currency_symbol(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("$")); }

// ---- PackageManager.hasSystemFeature: what this "device" has --------------------------
// Vulkan (unless the game's settings say GLES: then Unity picks OpenGL ES), a touchscreen,
// Wi-Fi; no phone, camera, GPS or sensors.
extern int droid_vulkan_wanted; // app_main.c
#define PLAY_PC_GAMES "com.nekki.vector,com.rovio.baba" // (Angry Birds 2: its gamepad code, 2026-11-01) // (the same as unity_host.c PAD_KEYBOARD_GAMES)
static jvalue has_feature(jobject self, const jvalue *a) {
    (void)self;
    const char *f = cstr(a[0].l);
    int yes = 0;
    if (f) {
        if (!strncmp(f, "android.hardware.vulkan", 23)) yes = droid_vulkan_wanted;
        else if (!strncmp(f, "android.hardware.touchscreen", 28) || !strcmp(f, "android.hardware.wifi") ||
                 !strcmp(f, "android.hardware.faketouch") || !strncmp(f, "android.hardware.opengles", 25) ||
                 !strcmp(f, "android.hardware.gamepad"))
            yes = 1;
        else if (!strcmp(f, "android.hardware.sensor.accelerometer") || !strcmp(f, "android.hardware.sensor.gyroscope"))
            yes = droid_motion_wanted(); // (sensors.c: the DualSense's motion)
        // Google Play Games on PC: games built for it turn on their keyboard play only there
        // (Vector: its W A S D / Q in levels; its menu keys work anywhere). unity_host.c
        // PAD_KEYBOARD_GAMES sends the DualSense as those keys.
        else if (!strcmp(f, "com.google.android.play.feature.HPE_EXPERIENCE"))
            yes = droid_game_in(PLAY_PC_GAMES) != 0;
        static int told;
        if (told++ < 30) droid_log("java: hasSystemFeature(%s) -> %s", f, yes ? "yes" : "no");
    }
    return Z(yes);
}

// ---- PackageManager: which store installed us (games check it; Google Play's answer)
static jvalue install_source(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L((jobject)new_obj("android/content/pm/InstallSourceInfo"));
}
static jvalue play_store(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("com.android.vending")); }

// The APK's signing certificate, Base64(SHA-1) (app_main.c reads it from the APK): Nekki's
// SecurityManager.GetSignature (Shadow Fight 2 hashes it; null stopped the start)
char droid_apk_signature[64];
static jvalue apk_signature_answer(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr(droid_apk_signature));
}

// PackageInfo.signingInfo / .signatures: the APK's certificate (Shadow Fight 3 hashes it and
// sends it with create_player: an empty one was "InvalidFingerprint", "download from the store")
extern unsigned char *droid_apk_cert;
extern unsigned droid_apk_cert_len;
static jobject sig_array(void) {
    jobjectArray arr = jni->NewObjectArray(&jni, 1, NULL, NULL);
    jni->SetObjectArrayElement(&jni, arr, 0, (jobject)new_obj("android/content/pm/Signature"));
    return arr;
}
jobject java_signing_info(void) { return (jobject)new_obj("android/content/pm/SigningInfo"); }
jobject java_signatures(void) { return sig_array(); }
static jvalue signers(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(sig_array());
}
static jvalue sig_bytes(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jsize n = (jsize)droid_apk_cert_len;
    jbyteArray b = jni->NewByteArray(&jni, n);
    if (n) jni->SetByteArrayRegion(&jni, b, 0, n, (const jbyte *)droid_apk_cert);
    return L(b);
}
static jvalue sig_chars(jobject self, const jvalue *a) { // toCharsString: the bytes in hex
    (void)self; (void)a;
    char *t = malloc(droid_apk_cert_len * 2 + 1);
    if (!t) return L(jstr(""));
    for (unsigned i = 0; i < droid_apk_cert_len; i++) sprintf(t + 2 * i, "%02x", droid_apk_cert[i]);
    t[droid_apk_cert_len * 2] = 0;
    jobject r = jstr(t);
    free(t);
    return L(r);
}
static jvalue sig_hash(jobject self, const jvalue *a) { // Arrays.hashCode of the bytes, as Java's
    (void)self; (void)a;
    int32_t h = 1;
    for (unsigned i = 0; i < droid_apk_cert_len; i++) h = (int32_t)((uint32_t)h * 31u + (uint32_t)(int8_t)droid_apk_cert[i]);
    return I(h);
}

// ---- Google's consent library (UMP): no consent form here, consent OBTAINED (3), ads may be
// requested; a game waits for its consent step before it goes on (Shadow Fight 2: black)
static jvalue ump_info(jobject self, const jvalue *a) {
    (void)self; (void)a;
    static jobject info;
    if (!info) info = (jobject)new_obj("com/google/android/ump/ConsentInformation");
    return L(info);
}
static jvalue ump_status(jobject self, const jvalue *a) { (void)self; (void)a; return I(3); }
// Google's Unity UMP plugin (com.google.unity.ump.UnityConsentForm(activity, callback)): no form
// needed (consent obtained above), dismissed at once without an error - Temple Run 2's screen sat
// dimmed behind the form that never came.
static jvalue ump_form_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *f = new_obj("com/google/unity/ump/UnityConsentForm");
    f->field[0].l = a[1].l; // the C# callback
    return L((jobject)f);
}
static jvalue ump_form_show(jobject self, const jvalue *a) {
    (void)a;
    jobj *f = as_obj(self);
    droid_log("java: UMP consent form: not needed (dismissed, no error)");
    jobject none = NULL;
    if (f && f->field[0].l) proxy_call(f->field[0].l, "onConsentFormDismissed", 1, &none);
    return V();
}
// Google Mobile Ads, the "next gen" Unity plugin (com.google.unity.ads.nextgen.UnityMobileAds):
// initialize(activity, listener) -> listener.onAdapterInitializationComplete(status) at once, the
// status with no adapters (no ad network here). Temple Run 2 waited for it behind its dimmed screen.
static jobj *new_map(const char *cls);
static jvalue gma_init(jobject self, const jvalue *a) {
    (void)self;
    droid_log("java: Google Mobile Ads initialize: done (no adapters)");
    jobject status = (jobject)new_obj("com/google/android/libraries/ads/mobile/sdk/initialization/InitializationStatus");
    if (a[1].l) proxy_call(a[1].l, "onAdapterInitializationComplete", 1, &status);
    return V();
}
static jvalue gma_status_map(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_map("java/util/HashMap")); }
// requestConsentInfoUpdate(activity, parameters, onSuccess, onFailure): done, onSuccess
static jvalue ump_update(jobject self, const jvalue *a) {
    (void)self;
    droid_log("java: UMP consent update: done (consent obtained)");
    proxy_call(a[2].l, "onConsentInfoUpdateSuccess", 0, NULL);
    return V();
}

// Rovio's Beacon billing (Angry Birds 2): GooglePlayBillingProvider(listener).initialize(skus, subs)
// answers on the listener; with no Google Play billing here: onInitializeError, as a phone without
// the Play Store. Nothing answered, the game's startup waited on it.
static jvalue rovio_billing_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *o = new_obj("com/rovio/beacon/billing/GooglePlayBillingProvider");
    o->field[0].l = a[0].l;
    return L((jobject)o);
}
static jvalue rovio_billing_init(jobject self, const jvalue *a) {
    (void)a;
    jobj *o = as_obj(self);
    jobject why = jstr("BILLING_UNAVAILABLE");
    droid_log("java: Rovio billing initialize: no Google Play billing (onInitializeError)");
    if (o && o->field[0].l) proxy_call(o->field[0].l, "onInitializeError", 1, &why);
    return V();
}

// SurfaceTexture / Surface: a video's picture for the game's texture (video_tex.c). Unity's
// VideoPlayer: SurfaceTexture(its texture) -> Surface(it) -> the decoder's output; on each
// onFrameAvailable it calls updateTexImage on its GL thread. Video games only (VIDEO_GAMES).
#define ST_CLASS "android/graphics/SurfaceTexture"
static video_surface *st_of(jobject st) {
    jobj *o = as_obj(st);
    return o && o->cls && !strcmp(o->cls, ST_CLASS) ? (video_surface *)(intptr_t)o->field[0].j : NULL;
}
static jvalue st_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *o = new_obj(ST_CLASS);
    o->field[0].j = (jlong)(intptr_t)vsurf_new((unsigned)a[0].i);
    return L((jobject)o);
}
static jvalue st_listener(jobject self, const jvalue *a) {
    vsurf_set_listener(st_of(self), a[0].l, self);
    return V();
}
static jvalue st_update(jobject self, const jvalue *a) {
    (void)a;
    vsurf_update(st_of(self));
    return V();
}
static jvalue st_matrix(jobject self, const jvalue *a) { // as a decoder's picture: upside down in t
    (void)self;
    static const jfloat m[16] = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1 };
    if (a[0].l) jni->SetFloatArrayRegion(&jni, (jfloatArray)a[0].l, 0, 16, m);
    return V();
}
static jvalue st_timestamp(jobject self, const jvalue *a) {
    (void)a;
    return J(vsurf_timestamp_ns(st_of(self)));
}
static jvalue st_nothing(jobject self, const jvalue *a) { (void)self; (void)a; return V(); }
static jvalue surface_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *o = new_obj("android/view/Surface");
    o->field[0].l = a[0].l;
    return L((jobject)o);
}
// mediandk.c's ANativeWindow_fromSurface: the video picture behind a Surface, or NULL
void *java_surface_video(jobject surface) {
    jobj *o = as_obj(surface);
    if (!o || !o->cls || strcmp(o->cls, "android/view/Surface")) return NULL;
    return st_of(o->field[0].l);
}
// UnityPlayer.showVideoPlayer(file, color, control, scaling, isURL, offset, length): our
// full-screen player (video_player.c); Unity waits while it plays. Video player games only.
int video_player_start(const char *file, int bg, int control, int scaling, int is_url, int offset, int length);
static jvalue unity_show_video(jobject self, const jvalue *a) {
    (void)self;
    return Z(video_player_start(cstr(a[0].l), a[1].i, a[2].i, a[3].i, a[4].z, a[5].i, a[6].i) ? JNI_TRUE : JNI_FALSE);
}
void java_frame_available(void *listener, void *self) {
    jobject st = (jobject)self;
    proxy_call((jobject)listener, "onFrameAvailable", 1, &st);
}

// UnityPlayer.UnitySendMessage(object, method, text): Unity's own native (queued for the
// game's main thread), what plugins' Java code answers C# with
static void unity_send_message(const char *object, const char *method, const char *text) {
    typedef void (*send_fn)(void *env, jclass cls, jobject obj, jobject meth, jobject msg);
    static send_fn send;
    if (!send) send = (send_fn)jni_find_native("nativeUnitySendMessage");
    if (!send) return;
    // the text: a byte[] in newer Unity, a String in older (2017: "(...String;String;String;)V";
    // Genshin Impact's got our bytes as a string - a NullReferenceException box at its login)
    const char *jni_native_signature(const char *name); // jni_impl.c
    jobject msg;
    // (the String form for miHoYo's games only; every other game had the bytes and works so.
    // Imangi's too when their Unity takes a String: Harbor Master's 2018.3 read our bytes as
    // garbage, its config JSON failed and it stayed on the loading picture)
    const char *sig = jni_native_signature("nativeUnitySendMessage");
    const int imangi = java_package && strncmp(java_package, "com.imangi.", 11) == 0;
    if (!((droid_quirks & QUIRK_MIHOYO) || (imangi && sig && sig[0])) || strstr(sig ? sig : "", "[B")) {
        jsize n = (jsize)strlen(text);
        jbyteArray b = jni->NewByteArray(&jni, n);
        jni->SetByteArrayRegion(&jni, b, 0, n, (const jbyte *)text);
        msg = b;
    } else {
        msg = jstr(text);
    }
    send(arm_jni_env(), find_class("com/unity3d/player/UnityPlayer"), jstr(object), jstr(method), msg);
}

// Imangi's ImangiXLSHelper (imangi_xls.c: its own code, apktool of Temple Run 2): Parse / ParseList
// decode the game's shipped configs on an AsyncTask and answer by UnitySendMessage to
// "ImangiXLSHelper" (OnParseComplete / OnParseListComplete; "" when it fails, as its onPostExecute)
typedef struct { char *text; int list; } xls_job;
static void *xls_run(void *arg) {
    xls_job *j = arg;
    int n = 0;
    char *r = j->list ? imangi_xls_parse_list(j->text, &n) : imangi_xls_parse(j->text);
    droid_log("java: ImangiXLSHelper.%s: %s (%d file(s), %zu bytes)", j->list ? "ParseList" : "Parse",
              r ? "decoded" : "failed", j->list ? n : 1, r ? strlen(r) : 0);
    droid_log("java: ImangiXLSHelper in %zu chars, out: %.400s", strlen(j->text), r ? r : "");
    unity_send_message("ImangiXLSHelper", j->list ? "OnParseListComplete" : "OnParseComplete", r ? r : "");
    free(r);
    free(j->text);
    free(j);
    return NULL;
}
static void xls_start(const jvalue *a, int list) {
    xls_job *j = malloc(sizeof(*j));
    if (!j) return;
    const char *t = java_text(a[0].l);
    j->text = strdup(t ? t : "");
    j->list = list;
    pthread_t th;
    if (pthread_create(&th, NULL, xls_run, j) == 0) pthread_detach(th);
    else { free(j->text); free(j); }
}
// Imangi's ImangiFileSystem (its Java: getFilesDir / getExternalFilesDir .getAbsolutePath()): the
// game's data folder for both, as Unity's persistentDataPath here. Null was Temple Run 2's
// ArgumentNullException ("path1", its privacy manager's save file) and its loading picture forever.
static char imangi_provider[256];
static const char *ifs_dir(void) { return game_data_dir[0] ? game_data_dir : game_dir; }
static jvalue ifs_data_path(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr(ifs_dir())); }
static jvalue ifs_internal_full(jobject self, const jvalue *a) { // (unused, name): internal/name
    (void)self;
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", ifs_dir(), java_text(a[1].l) ? java_text(a[1].l) : "");
    return L(jstr(p));
}
static jvalue ifs_external_full(jobject self, const jvalue *a) { // (folder, name): external/folder/name
    (void)self;
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s/%s", ifs_dir(), java_text(a[0].l) ? java_text(a[0].l) : "",
             java_text(a[1].l) ? java_text(a[1].l) : "");
    return L(jstr(p));
}

// Sonic Dash (SEGA's gamelib SLGlobal, Hardlight's HLUnityCore): what its Java answers on a
// phone. Null for all of them: an ArgumentNullException (the country) and "Player ID is still
// null or empty" (the Android ID), the loading picture forever. Sonic Dash only (per_game).
static jvalue sega_android_id(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr(android_device_id())); }
static jvalue sega_ad_id(jobject self, const jvalue *a) { // ad tracking off, as a phone can say
    (void)self; (void)a;
    return L(jstr("00000000-0000-0000-0000-000000000000"));
}
static jvalue sega_storage(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr(ifs_dir())); }
static jvalue sega_locale(jobject self, const jvalue *a) { (void)self; (void)a; return I(1); } // LocaleID_US (en_US)
static jvalue sega_connected(jobject self, const jvalue *a) { (void)self; (void)a; return Z(JNI_TRUE); }
static jvalue hl_country(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("US")); }
static jvalue hl_dp_to_px(jobject self, const jvalue *a) { (void)self; return I((jint)(a[0].f * 2.0f + 0.5f)); } // density 2
// Hardlight's Google consent (hlcompliance.ConsentManagementPlatformGoogle): consent not
// required, as Google's UMP answers outside the EU - the game heard nothing after "Agree" and
// waited. Its listener object hears "requested", then "success", as from the phone's UMP.
static char hl_cmp_listener[128];
static jvalue hl_cmp_init(jobject self, const jvalue *a) {
    (void)self;
    snprintf(hl_cmp_listener, sizeof(hl_cmp_listener), "%s", java_text(a[0].l) ? java_text(a[0].l) : "");
    return V();
}
static jvalue hl_cmp_request(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_log("java: Hardlight consent: not required (%s hears requested + success)", hl_cmp_listener);
    if (hl_cmp_listener[0]) {
        unity_send_message(hl_cmp_listener, "Native_OnRequestedConsent", "");
        unity_send_message(hl_cmp_listener, "Native_OnRequestConsentSuccess", "");
    }
    return V();
}
static jvalue hl_cmp_status(jobject self, const jvalue *a) { (void)self; (void)a; return I(1); } // NOT_REQUIRED
// Dragon Ball Legends (Dimps' TRAD): its permission dialogs, answered as a phone that had granted them
// (its Java checks each one; all granted, it tells its object OnRequestPermissionsResult "0" and the
// game goes on: unanswered it waited at its loading screen); its notch insets: none on a TV
static jvalue dimps_permissions(jobject self, const jvalue *a) { // (String[] permissions, String object)
    (void)self;
    const char *obj = java_text(a[1].l);
    droid_log("java: Dimps permissions: granted (%s hears OnRequestPermissionsResult 0)", obj ? obj : "?");
    if (obj && obj[0]) unity_send_message(obj, "OnRequestPermissionsResult", "0");
    return V();
}
static jvalue dimps_permission_11(jobject self, const jvalue *a) { // (String permission, String object)
    (void)self;
    const char *obj = java_text(a[1].l);
    droid_log("java: Dimps permission (11+): granted (%s hears 0)", obj ? obj : "?");
    if (obj && obj[0]) unity_send_message(obj, "OnRequestPermissionsResult", "0");
    return V();
}
static jvalue dimps_zero(jobject self, const jvalue *a) { (void)self; (void)a; return I(0); } // granted / no inset
// the Unity player's view (its mUnityPlayer: the activity stands in for both): gree's web view plugin
// asks for it at the title screen's tap and measures it; null, its C# threw and the tap went nowhere
static jvalue unity_player_view(jobject self, const jvalue *a) { (void)a; return L(self); }
// gree's web view (Dragon Ball Legends' "Create User Data"): there's no browser, so its pages are told
// they started and loaded (its C# polls GetMessage for "CallOnStarted:url" / "CallOnLoaded:url"); the
// pages and scripts it asks for are logged, to see what the game waits on
static char gree_msgs[8][1024];
static int gree_head, gree_count;
static int gree_on_puzzle; // the page loaded last is the game's captcha page (its login.php)
static pthread_mutex_t gree_lock = PTHREAD_MUTEX_INITIALIZER;
static void gree_push(const char *kind, const char *text) {
    pthread_mutex_lock(&gree_lock);
    if (gree_count < 8) {
        snprintf(gree_msgs[(gree_head + gree_count) % 8], sizeof gree_msgs[0], "%s:%s", kind, text ? text : "");
        gree_count++;
    }
    pthread_mutex_unlock(&gree_lock);
}
static jvalue gree_load_url(jobject self, const jvalue *a) {
    (void)self;
    const char *url = java_text(a[0].l);
    droid_log("java: web view: LoadURL %s", url ? url : "(null)");
    gree_on_puzzle = url && strstr(url, "/login.php") != NULL;
    gree_push("CallOnStarted", url);
    gree_push("CallOnLoaded", url);
    return V();
}
static jvalue gree_eval_js(jobject self, const jvalue *a) {
    (void)self;
    const char *js = java_text(a[0].l);
    droid_log("java: web view: EvaluateJS %.900s", js ? js : "(null)");
    return V();
}
// a page's answer to the game: its captcha page, solved in a browser, ends with
// window.location = "unity:<answer>"; that answer, put in data/webview_answer.txt, reaches the
// game as a phone's web view gives it ("CallFromJS:<answer>")
// Only the captcha page gets it (the game's other pages, its privacy page first, would take it);
// the file stays: the game's server took a saved answer again for a new player
static jobject gree_answer(void) {
    if (!gree_on_puzzle) return NULL;
    static char m[8200];
    FILE *f = fopen(APP0 "/data/webview_answer.txt", "r");
    if (!f) return NULL;
    gree_on_puzzle = 0;
    static char t[8192];
    size_t n = fread(t, 1, sizeof t - 1, f);
    fclose(f);
    t[n] = 0;
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ' || t[n - 1] == '"')) t[--n] = 0;
    const char *ans = t;
    while (*ans == ' ' || *ans == '"') ans++;
    if (!strncmp(ans, "unity:", 6)) ans += 6;
    snprintf(m, sizeof m, "CallFromJS:%s", ans);
    droid_log("java: web view: the page's answer (%zu letters) goes to the game", strlen(ans));
    return jstr(m);
}
static jvalue gree_get_message(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char m[1024];
    pthread_mutex_lock(&gree_lock);
    if (!gree_count) { pthread_mutex_unlock(&gree_lock); return L(gree_answer()); }
    memcpy(m, gree_msgs[gree_head], sizeof m);
    gree_head = (gree_head + 1) % 8, gree_count--;
    pthread_mutex_unlock(&gree_lock);
    droid_log("java: web view: message %.200s", m);
    return L(jstr(m));
}
static jvalue gree_destroy(jobject self, const jvalue *a) {
    (void)self; (void)a;
    gree_on_puzzle = 0;
    pthread_mutex_lock(&gree_lock);
    gree_head = gree_count = 0;
    pthread_mutex_unlock(&gree_lock);
    return V();
}
static jvalue gree_true(jobject self, const jvalue *a) { (void)self; (void)a; return I(1); }
static jvalue gree_void(jobject self, const jvalue *a) { (void)self; (void)a; return V(); }
static jvalue hl_language(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en")); }
// UNO (NetEase's com.netease.tools.Utility): free space in MB (0 was its "insufficient storage"),
// its UUID and "app opened" record (SharedPreferences unoConfig on a phone: files of ours here),
// the country. UNO only.
static void uno_file(char *out, size_t n, const char *name) {
    char rel[200], dir[512];
    game_path(dir, sizeof(dir), "files");
    mkdir(dir, 0777);
    snprintf(rel, sizeof(rel), "files/uno_%s", name);
    game_path(out, n, rel);
}
static jobject uno_load(const char *name, const char *dflt) {
    char p[512], v[256];
    uno_file(p, sizeof(p), name);
    FILE *f = fopen(p, "r");
    size_t k = f ? fread(v, 1, sizeof(v) - 1, f) : 0;
    if (f) fclose(f);
    v[k] = 0;
    return jstr(f ? v : dflt);
}
static void uno_save(const char *name, jobject text) {
    char p[512];
    uno_file(p, sizeof(p), name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(java_text(text) ? java_text(text) : "", f);
        fclose(f);
    }
}
static jvalue uno_free_mb(jobject self, const jvalue *a) { (void)self; (void)a; return J(16384); }
static jvalue uno_uuid_load(jobject self, const jvalue *a) { (void)self; (void)a; return L(uno_load("uuid", "")); }
static jvalue uno_uuid_save(jobject self, const jvalue *a) { (void)self; uno_save("uuid", a[0].l); return V(); }
static jvalue uno_opened_load(jobject self, const jvalue *a) { (void)self; (void)a; return L(uno_load("opened", "NO")); }
static jvalue uno_opened_save(jobject self, const jvalue *a) { (void)self; uno_save("opened", a[0].l); return V(); }
static jvalue uno_country(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("US")); }
// Sonic Boom's own plugin (com.sega.sonicboomandroid.plugin): HLUtils' locale (en_US: its US = 2)
// and BoomDeviceIdentifier, a file of the game's files folder ("" if none). Null / false: an
// ArgumentNullException and "Failed to save the boom server identifier on device"
static jvalue boom_locale(jobject self, const jvalue *a) { (void)self; (void)a; return I(2); }
static jvalue boom_locale_text(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en_US")); }
static jvalue boom_country(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("US")); }
static void boom_id_path(char *out, size_t n) { game_path(out, n, "files/sonic_boom_device_identifier"); }
static jvalue boom_id_get(jobject self, const jvalue *a) {
    (void)self; (void)a;
    char p[512], id[128] = "";
    boom_id_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (f) {
        const size_t k = fread(id, 1, sizeof(id) - 1, f);
        id[k] = 0;
        fclose(f);
    }
    return L(jstr(id));
}
static jvalue boom_id_set(jobject self, const jvalue *a) {
    (void)self;
    char p[512], dir[512];
    game_path(dir, sizeof(dir), "files");
    mkdir(dir, 0777);
    boom_id_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(java_text(a[0].l) ? java_text(a[0].l) : "", f);
        fclose(f);
    }
    return Z(f ? JNI_TRUE : JNI_FALSE);
}
// SEGA's Google Play Games sign-in (SLSocialInterface.Authenticate -> GooglePlayInterface): no
// Play Games here, so it ends as a phone's failed sign-in, "Community" hears
// OnGameCenterAuthenticateComplete "false" (online, the PS5 waited on the loading picture for it)
static jvalue sega_gp_auth(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_log("java: SEGA Google Play sign-in: not signed in (Community hears false)");
    unity_send_message("Community", "OnGameCenterAuthenticateComplete", "false");
    return V();
}
static jvalue sega_gp_no(jobject self, const jvalue *a) { (void)self; (void)a; return Z(JNI_FALSE); }
// SEGA's DashServerIdentifier: the account id the game's server gave, in a file of the game's
// files folder ("production": sonic_dash_server_identifier, else ..._<environment>); "" if none
static void dash_id_path(char *out, size_t n, const char *env) {
    char rel[200];
    if (!env || !strcmp(env, "production")) snprintf(rel, sizeof(rel), "files/sonic_dash_server_identifier");
    else snprintf(rel, sizeof(rel), "files/sonic_dash_server_identifier_%s", env);
    game_path(out, n, rel);
}
static jvalue dash_id_get(jobject self, const jvalue *a) {
    (void)self;
    char p[512], id[51] = "";
    dash_id_path(p, sizeof(p), java_text(a[0].l));
    FILE *f = fopen(p, "r");
    if (f) {
        const size_t k = fread(id, 1, 50, f);
        id[k] = 0;
        fclose(f);
    }
    return L(jstr(id));
}
static jvalue dash_id_set(jobject self, const jvalue *a) { // (id, environment)
    (void)self;
    char p[512], dir[512];
    game_path(dir, sizeof(dir), "files");
    mkdir(dir, 0777);
    dash_id_path(p, sizeof(p), java_text(a[1].l));
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(java_text(a[0].l) ? java_text(a[0].l) : "", f);
        fclose(f);
    }
    return Z(f ? JNI_TRUE : JNI_FALSE);
}
static jvalue hl_cmp_complete(jobject self, const jvalue *a) { (void)self; (void)a; return Z(JNI_TRUE); }
static jvalue ifs_set_provider(jobject self, const jvalue *a) {
    (void)self;
    snprintf(imangi_provider, sizeof(imangi_provider), "%s", java_text(a[0].l) ? java_text(a[0].l) : "");
    return V();
}
static jvalue ifs_get_provider(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr(imangi_provider)); }

static jvalue xls_parse(jobject self, const jvalue *a) { (void)self; xls_start(a, 0); return V(); }
static jvalue xls_parse_list(jobject self, const jvalue *a) { (void)self; xls_start(a, 1); return V(); }

// Prime31's Etcetera plugin (com.prime31.EtceteraPlugin: native Android dialogs, toasts, review
// asks), answered as its Java does through UnitySendMessage to "EtceteraAndroidManager". No dialogs
// here: an alert's positive button pressed at once, a prompt finished with its text. Its null
// instance() was Temple Run 1's NullReferenceException in its privacy flow - input off, frozen.
static jvalue p31_instance(jobject self, const jvalue *a) {
    (void)self; (void)a;
    static jobject inst;
    if (!inst) inst = (jobject)new_obj("com/prime31/EtceteraPlugin");
    return L(inst);
}
// The Infinity Blade III / II ports (com.ib3port.game, com.ib2port.game; MIT, iOS UE3 on Android):
// their GameActivity's Java, as their native code calls it on the activity.
// showAlert(id, title, message, buttons[], cancel, style, text): no dialogs here: logged, and
// answered with its cancel button (else the first) through the game's nativeAlertResult, a moment
// later on a thread of its own (as Android's UI thread would: the game waits for it after the call).
// httpRequest (ClashMob's server): offline (null), the game plays without it.
typedef struct { jint id, button; jobject text; } ib_alert;
static void *ib_alert_answer(void *arg) {
    ib_alert *al = arg;
    usleep(100000);
    typedef void (*result_fn)(void *env, jclass cls, jint id, jint button, jobject text);
    static result_fn result;
    if (!result) result = (result_fn)jni_find_native("nativeAlertResult");
    if (result) result(arm_jni_env(), find_class(droid_game_in("com.ib2port.") ? "com/ib2port/game/GameActivity" : droid_game_in("com.ib1port.") ? "com/ib1port/game/GameActivity" : "com/ib3port/game/GameActivity"), al->id, al->button, al->text);
    else droid_log("java: ib alert %d: no nativeAlertResult registered", al->id);
    free(al);
    return NULL;
}
static jvalue ib_show_alert(jobject self, const jvalue *a) {
    (void)self;
    ib_alert *al = malloc(sizeof(*al));
    if (!al) return V();
    al->id = a[0].i, al->button = a[4].i >= 0 ? a[4].i : 0, al->text = a[6].l;
    const char *title = cstr(a[1].l), *msg = cstr(a[2].l);
    droid_log("java: ib alert %d \"%s\": \"%.300s\" (style %d) -> button %d", al->id, title ? title : "", msg ? msg : "",
              a[5].i, al->button);
    pthread_t t;
    if (pthread_create(&t, NULL, ib_alert_answer, al) == 0) pthread_detach(t);
    else free(al);
    return V();
}
static jvalue ib_http(jobject self, const jvalue *a) {
    (void)self;
    const char *method = cstr(a[0].l), *url = cstr(a[1].l);
    static int told;
    if (told++ < 10) droid_log("java: ib httpRequest %s %s: offline", method ? method : "?", url ? url : "?");
    return L(NULL);
}

static jvalue p31_show_alert(jobject self, const jvalue *a) { // (title, message, positive, negative)
    (void)self;
    const char *pos = cstr(a[2].l);
    droid_log("java: Etcetera alert \"%s\": \"%s\" -> \"%s\" pressed", cstr(a[0].l) ? cstr(a[0].l) : "",
              cstr(a[1].l) ? cstr(a[1].l) : "", pos ? pos : "");
    unity_send_message("EtceteraAndroidManager", "alertButtonClicked", pos ? pos : "");
    return V();
}
static jvalue p31_show_prompt(jobject self, const jvalue *a) { // (title, message, hint, text, positive, negative)
    (void)self;
    const char *text = cstr(a[3].l);
    droid_log("java: Etcetera prompt \"%s\" -> \"%s\"", cstr(a[0].l) ? cstr(a[0].l) : "", text ? text : "");
    unity_send_message("EtceteraAndroidManager", "promptFinishedWithText", text ? text : "");
    return V();
}
static jvalue p31_nothing(jobject self, const jvalue *a) { (void)self; (void)a; return V(); }

// Stan's Assets native popups: ShowMessage(title, message, "button|button|...", theme). No
// Android dialogs here: the yes button pressed at once (agree / accept / OK / yes / continue,
// else the first); the plugin answers with its text (Shadow Fight 2's terms of use waited)
static jvalue mnp_show_message(jobject self, const jvalue *a) {
    (void)self;
    const char *title = cstr(a[0].l), *buttons = cstr(a[2].l);
    static const char *const yes[] = { "agree", "accept", "ok", "yes", "continue" };
    char pick[128] = "OK", tok[128];
    int found = 0;
    for (size_t y = 0; y < sizeof(yes) / sizeof(yes[0]) && !found && buttons; y++)
        for (const char *p = buttons; *p && !found;) {
            size_t n = strcspn(p, "|");
            snprintf(tok, sizeof(tok), "%.*s", (int)(n < sizeof(tok) - 1 ? n : sizeof(tok) - 1), p);
            char low[128];
            size_t k = 0;
            for (; tok[k] && k < sizeof(low) - 1; k++) low[k] = (char)tolower((unsigned char)tok[k]);
            low[k] = 0;
            const char *w = strstr(low, yes[y]); // a whole word ("User Agreement" isn't "agree")
            size_t wl = strlen(yes[y]);
            if (w && (w == low || !isalpha((unsigned char)w[-1])) && !isalpha((unsigned char)w[wl])) {
                snprintf(pick, sizeof(pick), "%s", tok);
                found = 1;
            }
            p += n + (p[n] == '|');
        }
    if (!found && buttons && *buttons) snprintf(pick, sizeof(pick), "%.*s", (int)strcspn(buttons, "|"), buttons);
    droid_log("java: popup \"%s\" (%s): \"%s\" pressed", title ? title : "", buttons ? buttons : "", pick);
    unity_send_message("AndroidPopUp", "onPopUpCallBack", pick);
    return V();
}

// UnityPlayer.getNetworkConnectivity: Unity's NetworkReachability, 2 = local network (Wi-Fi /
// Ethernet); 0 told games "no internet" and they skipped their downloads
static jvalue network_lan(jobject self, const jvalue *a) { (void)self; (void)a; return I(settings.offline ? 0 : 2); }

// ConnectivityManager: the console's network as Wi-Fi, connected (none with the game's offline
// setting). Unanswered, NetworkInfo.isConnected was 0: Genshin Impact's "CheckCanReachNetwork
// Failed", error 9905 "no network" before its resource download
// (miHoYo's games only. Every other game keeps what the stubs gave it before - an empty object,
// "not connected" - and works so: Vector waited at "connecting" once these answered for all)
#define NET_OLD (!(droid_quirks & QUIRK_MIHOYO))
static jvalue net_active_info(jobject self, const jvalue *a) {
    (void)self; (void)a;
    if (NET_OLD) return L(new_obj("android/net/NetworkInfo"));
    return L(settings.offline ? NULL : new_obj("android/net/NetworkInfo"));
}
static jvalue net_active_network(jobject self, const jvalue *a) {
    (void)self; (void)a;
    if (NET_OLD) return L(new_obj("android/net/Network"));
    return L(settings.offline ? NULL : new_obj("android/net/Network"));
}
static jvalue net_capabilities(jobject self, const jvalue *a) {
    (void)self; (void)a;
    if (NET_OLD) return L(new_obj("android/net/NetworkCapabilities"));
    return L(settings.offline ? NULL : new_obj("android/net/NetworkCapabilities"));
}
static jvalue net_connected(jobject self, const jvalue *a) { (void)self; (void)a; return Z(NET_OLD ? 0 : !settings.offline); }
static jvalue net_type_wifi(jobject self, const jvalue *a) { (void)self; (void)a; return I(NET_OLD ? 0 : 1); } // TYPE_WIFI
static jvalue net_type_name(jobject self, const jvalue *a) { (void)self; (void)a; return L(NET_OLD ? NULL : jstr("WIFI")); }

// ---- System.load / loadLibrary: the library loaded, its JNI_OnLoad run (once) -----------
// Plugins keep the JavaVM they get there (Firebase: without it, a NULL VM call).
int java_jni_onload(void *so_module_ptr) {
    so_module *m = so_module_ptr;
    static so_module *done[64];
    static int n_done;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!m) return -1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n_done; i++)
        if (done[i] == m) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
    if (n_done < 64) done[n_done++] = m;
    pthread_mutex_unlock(&lock);
    void *onload = so_sym(m, "JNI_OnLoad");
    if (!onload) return 0;
    typedef jint (*onload_fn)(void *, void *);
    jint version = ((onload_fn)arm_callback((uint64_t)(uintptr_t)onload))(arm_java_vm(), NULL);
    droid_log("java: JNI_OnLoad of a library = %#x", version);
    return 0;
}

static void load_library(const char *base) {
    so_module *m = arm_module_load(base);
    droid_log("java: System.load %s -> %s", base, m ? "loaded" : "not in the APK");
    java_jni_onload(m);
}

static jvalue sys_load(jobject self, const jvalue *a) { // (path)
    (void)self;
    const char *path = cstr(a[0].l);
    if (path) {
        const char *base = strrchr(path, '/');
        load_library(base ? base + 1 : path);
    }
    return V();
}

static jvalue sys_load_library(jobject self, const jvalue *a) { // (name): lib<name>.so
    (void)self;
    const char *name = cstr(a[0].l);
    if (name) {
        char base[160];
        snprintf(base, sizeof(base), "lib%s.so", name);
        load_library(base);
    }
    return V();
}

// ---- Unity's ReflectionHelper (unity-classes.jar, Java code we don't have) ----------------
// C#'s AndroidJavaObject / AndroidJavaClass find every method and field through it:
// getMethodID(class, name, signature, static) -> a Method, then FromReflectedMethod -> its ID.
// Ours: the same lookup as GetMethodID, its ID kept in the Method (or Field) object.
// Not found: null (Unity's would throw NoSuchMethodError; C# then gets a zero ID).
jmethodID droid_jni_method_id(jclass c, const char *name, const char *sig, int is_static); // arm_imports.c
jfieldID droid_jni_field_id(jclass c, const char *name, const char *sig, int is_static);
#define REFLECTED_ID ((void *)2) // jobj.asset: a looked-up Method / Field, its ID in mark

static jobject reflected(const char *kind, jobject owner, const char *name, const char *sig, void *id) {
    jobj *m = new_obj(kind);
    m->buf = strdup(name);
    m->ref = owner; // getDeclaringClass
    m->asset = REFLECTED_ID;
    m->mark = (int64_t)(intptr_t)id;
    m->field[0].l = (jobject)strdup(sig); // getFieldSignature
    return (jobject)m;
}

static int reflect_logged;

static jvalue rh_method(jobject self, const jvalue *a) { // (Class, name, signature, isStatic)
    (void)self;
    const char *name = cstr(a[1].l), *sig = cstr(a[2].l);
    if (!a[0].l || !name || !sig) return L(NULL);
    jmethodID id = droid_jni_method_id((jclass)a[0].l, name, sig, a[3].z);
    if (reflect_logged++ < 80)
        droid_log("java: reflect %s%s.%s%s -> %s", a[3].z ? "static " : "",
                  placeholder(a[0].l) || java_is_string(a[0].l) ? "?" : (const char *)a[0].l, name, sig,
                  id ? "found" : "none");
    return L(id ? reflected("java/lang/reflect/Method", a[0].l, name, sig, id) : NULL);
}

static jvalue rh_constructor(jobject self, const jvalue *a) { // (Class, signature)
    (void)self;
    const char *sig = cstr(a[1].l);
    if (!a[0].l || !sig) return L(NULL);
    jmethodID id = droid_jni_method_id((jclass)a[0].l, "<init>", sig, 0);
    if (reflect_logged++ < 80) droid_log("java: reflect new %s -> %s", sig, id ? "found" : "none");
    return L(id ? reflected("java/lang/reflect/Constructor", a[0].l, "<init>", sig, id) : NULL);
}

static jvalue rh_field(jobject self, const jvalue *a) { // (Class, name, signature, isStatic)
    (void)self;
    const char *name = cstr(a[1].l), *sig = cstr(a[2].l);
    if (!a[0].l || !name || !sig) return L(NULL);
    jfieldID id = droid_jni_field_id((jclass)a[0].l, name, sig, a[3].z);
    if (reflect_logged++ < 80)
        droid_log("java: reflect field %s %s -> %s", name, sig, id ? "found" : "none");
    return L(id ? reflected("java/lang/reflect/Field", a[0].l, name, sig, id) : NULL);
}

static jvalue rh_field_signature(jobject self, const jvalue *a) { // (Field)
    (void)self;
    jobj *f = as_obj(a[0].l);
    return L(jstr(f && f->asset == REFLECTED_ID && f->field[0].l ? (const char *)f->field[0].l : ""));
}

static jvalue declaring_class(jobject self, const jvalue *a) {
    (void)a;
    jobj *m = as_obj(self);
    return L(m ? m->ref : NULL);
}

jfieldID java_from_reflected_field(jobject field) {
    jobj *f = as_obj(field);
    return f && f->asset == REFLECTED_ID ? (jfieldID)(intptr_t)f->mark : NULL;
}

// FromReflectedMethod: our Method (name, interface in ref) -> that interface method's ID
jmethodID java_from_reflected(jobject method) {
    jobj *m = as_obj(method);
    if (m && m->asset == REFLECTED_ID) return (jmethodID)(intptr_t)m->mark; // ReflectionHelper's
    if (!m || !m->buf) return NULL;
    const char *iface = !placeholder(m->ref) && !java_is_string(m->ref) ? (const char *)m->ref : NULL;
    jmethodID any = NULL;
    pthread_mutex_lock(&dyn_lock);
    for (int i = 0; i < dyn_count; i++) {
        if (strcmp(dyn[i].name, m->buf) != 0) continue;
        if (iface && strcmp(dyn[i].cls, iface) == 0) {
            any = (jmethodID)&dyn[i];
            break;
        }
        if (!any && !iface) any = (jmethodID)&dyn[i];
    }
    // Not asked for yet (Unity asks FromReflectedMethod first, GetMethodID after):
    // made now, its signature filled in by that GetMethodID.
    if (!any && iface && dyn_count < MAX_DYN) {
        dyn_method *d = &dyn[dyn_count++];
        snprintf(d->cls, sizeof(d->cls), "%s", iface);
        snprintf(d->name, sizeof(d->name), "%s", m->buf);
        d->sig[0] = 0;
        any = (jmethodID)d;
    }
    pthread_mutex_unlock(&dyn_lock);
    return any;
}

static jobject box(char kind, jvalue v) {
    char t[48];
    const char *cls;
    switch (kind) {
    case 'Z': cls = "java/lang/Boolean"; snprintf(t, sizeof(t), "%d", v.z ? 1 : 0); break;
    case 'J': cls = "java/lang/Long"; snprintf(t, sizeof(t), "%lld", (long long)v.j); break;
    case 'F': cls = "java/lang/Float"; snprintf(t, sizeof(t), "%.9g", (double)v.f); break;
    case 'D': cls = "java/lang/Double"; snprintf(t, sizeof(t), "%.17g", v.d); break;
    case 'B': cls = "java/lang/Byte"; snprintf(t, sizeof(t), "%d", (int)v.b); break;
    case 'S': cls = "java/lang/Short"; snprintf(t, sizeof(t), "%d", (int)v.s); break;
    case 'C': cls = "java/lang/Character"; snprintf(t, sizeof(t), "%d", (int)v.c); break;
    default: cls = "java/lang/Integer"; snprintf(t, sizeof(t), "%d", v.i); break;
    }
    jobj *b = new_obj(cls);
    b->buf = strdup(t);
    return (jobject)b;
}

// ---- JavaMap (the debug tool): every Java call we only stub, counted, as a worklist ----
// tools.txt "JavaMap": javamap.txt in the game's folder, every 15 s while new calls come:
// each method we don't really answer (class, name, signature), how often the game called it
// and what it got (0 / null, an empty object, itself) - what the plugin library needs next.
#define JMAP_MAX 1024
static struct { const void *m; const char *cls, *name, *sig, *answer; unsigned long n; } jmap[JMAP_MAX];
static int jmap_count, jmap_dirty;
static pthread_mutex_t jmap_lock = PTHREAD_MUTEX_INITIALIZER;
static int jmap_cmp(const void *a, const void *b) {
    unsigned long x = ((const typeof(jmap[0]) *)a)->n, y = ((const typeof(jmap[0]) *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void javamap_note(const void *m, const char *cls, const char *name, const char *sig, const char *answer) {
    static int on = -1;
    if (on < 0) on = droid_tool("JavaMap");
    if (!on) return;
    pthread_mutex_lock(&jmap_lock);
    int i = 0;
    while (i < jmap_count && jmap[i].m != m) i++;
    if (i == jmap_count && jmap_count < JMAP_MAX) {
        jmap[i].m = m, jmap[i].cls = cls ? cls : "?", jmap[i].name = name, jmap[i].sig = sig;
        jmap[i].answer = answer, jmap[i].n = 0;
        jmap_count++;
        jmap_dirty = 1;
    }
    if (i < jmap_count) jmap[i].n++;
    static int64_t last;
    int64_t t = now_ns();
    if (jmap_dirty && t - last > 15000000000LL) { // the file again (new calls since)
        last = t;
        jmap_dirty = 0;
        static typeof(jmap[0]) copy[JMAP_MAX];
        memcpy(copy, jmap, sizeof(jmap[0]) * (size_t)jmap_count);
        int n = jmap_count;
        qsort(copy, (size_t)n, sizeof(copy[0]), jmap_cmp);
        char path[300];
        FILE *f = fopen(game_path(path, sizeof(path), "javamap.txt"), "w");
        if (f) {
            fprintf(f, "# JavaMap: Java calls this game made that we only stub (most called first)\n"
                       "# calls  answer        class.method signature\n");
            for (int k = 0; k < n; k++)
                fprintf(f, "%7lu  %-12s  %s.%s%s\n", copy[k].n, copy[k].answer, copy[k].cls, copy[k].name, copy[k].sig);
            fclose(f);
            droid_log("JavaMap: %d stubbed Java methods so far (javamap.txt)", n);
        }
    }
    pthread_mutex_unlock(&jmap_lock);
}

static jvalue dyn_call(const dyn_method *m, jobject self, const jvalue *args) {
    jobj *so = as_obj(self);
    if (m->stub && (!strcmp(m->name, "setText") || !strcmp(m->name, "setCaption") ||
                    !strcmp(m->name, "addButton"))) {
        const char *t = cstr(args[0].l);
        droid_log("java: dialog %s = \"%s\"", m->name, t ? t : "(null)");
    }
    // a method the game registered as native itself (RegisterNatives), called through JNI: on
    // Android that runs the native code. Mortar starts its threads so (AndroidThread calls
    // NativeGameLib.native_threadEntry(J) on the new thread: stubbed, the thread never ran and
    // Fruit Ninja 2.8.9's first frame waited on its semaphore for ever). Names "native..." with
    // the registered signature, integer / object arguments only (no float registers here).
    if (m->stub && m->name && !strncmp(m->name, "native", 6) && m->sig) {
        const char *jni_native_signature(const char *name);
        void *fn = jni_find_native(m->name);
        if (fn && !strcmp(jni_native_signature(m->name), m->sig)) {
            uint64_t x[6] = { 0 };
            int argc = 0, ok = 1;
            for (const char *p = m->sig + 1; *p && *p != ')' && ok; p++) {
                char c = *p;
                if (c == '[') {
                    while (*p == '[') p++;
                    c = '[';
                    if (*p == 'L') p = strchr(p, ';');
                } else if (c == 'L') p = strchr(p, ';');
                if (!p || c == 'F' || c == 'D' || argc >= 6) { ok = 0; break; }
                x[argc] = c == 'J' ? (uint64_t)args[argc].j : c == 'L' || c == '[' ? (uint64_t)(uintptr_t)args[argc].l
                        : c == 'Z' ? args[argc].z : c == 'B' ? (uint64_t)(int64_t)args[argc].b
                        : c == 'C' ? args[argc].c : c == 'S' ? (uint64_t)(int64_t)args[argc].s : (uint64_t)(int64_t)args[argc].i;
                argc++;
            }
            if (ok) {
                static int told;
                if (told++ < 20) droid_log("java: %s%s: the game's own native, run", m->name, m->sig);
                typedef uint64_t (*nat6)(void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
                uint64_t r = ((nat6)fn)(arm_jni_env(), self ? self : (jobject)m->cls, x[0], x[1], x[2], x[3], x[4], x[5]);
                jvalue z;
                z.j = (jlong)r;
                return z;
            }
        }
    }
    // a game's own Java log helper (Temple Run 2's ImangiAndroidLog.Log(String), and the like):
    // its text in our log - the game's own story of where it stops (it was swallowed)
    if (m->stub && m->sig && !strncmp(m->sig, "(Ljava/lang/String;", 19) && strstr(m->sig, ")V") &&
        (!strcasecmp(m->name, "log") || !strcmp(m->name, "d") || !strcmp(m->name, "i") || !strcmp(m->name, "w") ||
         !strcmp(m->name, "e") || !strcasecmp(m->name, "debug") || !strcasecmp(m->name, "info") ||
         !strcasecmp(m->name, "warn") || !strcasecmp(m->name, "error") || !strcasecmp(m->name, "logDebug") ||
         !strcasecmp(m->name, "logError"))) {
        int two = !strncmp(m->sig, "(Ljava/lang/String;Ljava/lang/String;", 37); // (tag, text)
        const char *t = cstr(args[two].l);
        droid_log("[game] %s%s%s", two ? cstr(args[0].l) : "", two ? ": " : "", t ? t : "(null)");
        jvalue z;
        z.j = 0;
        return z;
    }
    if (m->stub && !(so && strcmp(so->cls, "java/lang/reflect/Proxy") == 0)) {
        static const dyn_method *told[256];
        static int n_told;
        int seen = 0;
        for (int i = 0; i < n_told && !seen; i++) seen = told[i] == m;
        jvalue z;
        z.j = 0;
        // a constructor, or an object answer (getInstance...): an object of that class, one
        // per method (null would be a NullReferenceException in the C# that asked); strings,
        // arrays and plain Object: null
        const char *ret = strchr(m->sig, ')');
        // Builders (Google Play Billing's BillingClient.newBuilder(...).setListener(...).build(),
        // and the like): newBuilder / builder() make an object of <class>$Builder; on one, every
        // call answers the builder itself, and build() an object of <class>. Before, newBuilder
        // gave a plain Object, setListener on it null, and Shadow Fight 3 stopped on the
        // NullReferenceException ("unrecoverable error, restart game")
        size_t bl = so ? strlen(so->cls) : 0;
        int on_builder = so && bl > 8 && !strcmp(so->cls + bl - 8, "$Builder");
        if (on_builder && ret && ret[1] == 'L') {
            if (!strncmp(m->name, "build", 5)) {
                char made[128];
                snprintf(made, sizeof(made), "%.*s", (int)(bl - 8), so->cls);
                z.l = (jobject)new_obj(strdup(made));
            } else {
                z.l = self;
            }
            javamap_note(m, m->cls, m->name, m->sig, "builder");
            return z;
        }
        if ((!strcmp(m->name, "newBuilder") || !strcmp(m->name, "builder")) && ret && ret[1] == 'L' && m->cls) {
            char made[128];
            snprintf(made, sizeof(made), "%s$Builder", m->cls);
            z.l = (jobject)new_obj(strdup(made));
            if (!seen && n_told < 256) {
                told[n_told++] = m;
                droid_log("java: stub %s.%s called: answers a %s", m->cls, m->name, made);
            }
            javamap_note(m, m->cls, m->name, m->sig, "a builder");
            return z;
        }
        char rcls[128] = { 0 };
        if (strcmp(m->name, "<init>") == 0) snprintf(rcls, sizeof(rcls), "%s", m->cls);
        else if (ret && ret[1] == 'L') {
            const char *e = strchr(ret + 2, ';');
            if (e && e - (ret + 2) < (long)sizeof(rcls)) memcpy(rcls, ret + 2, (size_t)(e - (ret + 2)));
            for (char *c = rcls; *c; c++) if (*c == '.') *c = '/';
            // strings: null (no text to make up). A plain Object answer (C#'s AndroidJavaObject
            // asks so for everything): null ("none", as getLastNotificationIntent is on a normal
            // start), except from what makes or hands out a helper (getInstance, get...Impl,
            // ...Manager, create..., new...): an empty object the C# can call on
            if (!strcmp(rcls, "java/lang/String") || !strcmp(rcls, "java/lang/CharSequence")) rcls[0] = 0;
            if (!strcmp(rcls, "java/lang/Object")) {
                const char *n = m->name;
                size_t nl = strlen(n);
                int maker = !strcmp(n, "getInstance") || !strcmp(n, "getDefault") || !strncasecmp(n, "create", 6) ||
                            !strncmp(n, "new", 3) || strstr(n, "Manager") || (nl > 4 && !strcmp(n + nl - 4, "Impl")) ||
                            !strcmp(n, "GetInstance") || (nl > 8 && !strcmp(n + nl - 8, "Instance"));
                if (!maker) rcls[0] = 0;
            }
            // a lookup ("get X from Y", "the last one", find...): none, as on a normal start
            // (getNotificationFromIntent: an object there meant "opened by a notification")
            const char *n = m->name;
            if (strstr(n, "From") || !strncmp(n, "getLast", 7) || !strncmp(n, "find", 4) || !strncmp(n, "lookup", 6) ||
                !strncmp(n, "peek", 4) || !strncmp(n, "poll", 4))
                rcls[0] = 0;
        }
        // a builder (setTitle(...) answering the same Builder): the object itself, so a chain of
        // calls goes on (notifications, ad requests, analytics events, URIs...)
        if (rcls[0] && so && strcmp(m->name, "<init>") != 0 && m->cls && !strcmp(rcls, m->cls)) {
            if (!seen && n_told < 256) {
                told[n_told++] = m;
                droid_log("java: stub %s.%s%s called: answers itself (a builder)", m->cls, m->name, m->sig);
            }
            z.l = self;
            javamap_note(m, m->cls, m->name, m->sig, "itself");
            return z;
        }
        if (rcls[0] && strcmp(rcls, "?") != 0) {
            static struct { const dyn_method *m; jobject o; } made[512];
            static int n_made;
            static pthread_mutex_t made_lock = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&made_lock);
            for (int i = 0; i < n_made && !z.l; i++)
                if (made[i].m == m && strcmp(m->name, "<init>") != 0) z.l = made[i].o;
            if (!z.l) {
                z.l = (jobject)new_obj(strdup(rcls));
                if (n_made < 512) { made[n_made].m = m; made[n_made].o = z.l; n_made++; }
            }
            pthread_mutex_unlock(&made_lock);
        }
        if (!seen && n_told < 256) {
            told[n_told++] = m;
            droid_log("java: stub %s.%s%s called: answers %s", m->cls, m->name, m->sig,
                      z.l ? "an empty object" : "0 / null");
        }
        javamap_note(m, m->cls, m->name, m->sig, z.l ? "empty object" : "0 / null");
        return z;
    }
    static int logged;
    if (logged < 60) {
        logged++;
        droid_log("java: interface call %s.%s%s on %s", m->cls, m->name, m->sig,
                  as_obj(self) ? as_obj(self)->cls : "a FalsoJNI object");
    }
    char k[16];
    int n = java_arg_kinds(m->sig, k, 16);
    jobject objs[16];
    for (int i = 0; i < n; i++) objs[i] = k[i] == 'L' ? args[i].l : box(k[i], args[i]);
    jobject r = proxy_call(self, m->name, n, objs);
    const char *ret = strchr(m->sig, ')');
    char rk = ret ? ret[1] : 'V';
    jvalue v;
    v.j = 0;
    if (rk == 'L' || rk == '[') v.l = r;
    else if (rk != 'V' && r) {
        jobj *b = as_obj(r);
        const char *t = b && b->buf ? b->buf : "0";
        if (rk == 'J') v.j = strtoll(t, NULL, 10);
        else if (rk == 'F') v.f = strtof(t, NULL);
        else if (rk == 'D') v.d = strtod(t, NULL);
        else if (rk == 'Z') v.z = t[0] == '1';
        else v.i = atoi(t);
    }
    return v;
}

// new Boolean(z) / Integer(i) / Long(j) / Float(f) / Double(d), and valueOf()
static jvalue box_z(jobject self, const jvalue *a) { (void)self; return L(box('Z', a[0])); }
static jvalue box_i(jobject self, const jvalue *a) { (void)self; return L(box('I', a[0])); }
static jvalue box_j(jobject self, const jvalue *a) { (void)self; return L(box('J', a[0])); }
static jvalue box_f(jobject self, const jvalue *a) { (void)self; return L(box('F', a[0])); }
static jvalue box_d(jobject self, const jvalue *a) { (void)self; return L(box('D', a[0])); }

static jvalue method_get_name(jobject self, const jvalue *a) {
    (void)a;
    jobj *m = as_obj(self);
    return L(jstr(m && m->buf ? m->buf : ""));
}

// Which of a JNIBridge proxy's interfaces declares this method: one Unity asked GetMethodID
// for, else a known Android one, else the first.
static jobject bridge_iface(jobj *p, const char *method) {
    jobject *ifs = (jobject *)p->buf;
    int n = (int)p->len;
    if (n <= 1 || !ifs) return p->ref;
    static const struct { const char *method, *iface; } known[] = {
        {"doFrame", "android/view/Choreographer$FrameCallback"},
        {"handleMessage", "android/os/Handler$Callback"},
        {"run", "java/lang/Runnable"},
    };
    pthread_mutex_lock(&dyn_lock);
    jobject hit = NULL;
    for (int i = 0; i < n && !hit; i++) {
        if (placeholder(ifs[i]) || java_is_string(ifs[i])) continue;
        for (int k = 0; k < dyn_count && !hit; k++)
            if (dyn[k].sig[0] && strcmp(dyn[k].name, method) == 0 && strcmp(dyn[k].cls, (const char *)ifs[i]) == 0)
                hit = ifs[i];
    }
    pthread_mutex_unlock(&dyn_lock);
    for (size_t k = 0; !hit && k < sizeof(known) / sizeof(known[0]); k++)
        if (strcmp(known[k].method, method) == 0)
            for (int i = 0; i < n && !hit; i++)
                if (!placeholder(ifs[i]) && !java_is_string(ifs[i]) && strcmp((const char *)ifs[i], known[k].iface) == 0)
                    hit = ifs[i];
    return hit ? hit : p->ref;
}

static jobject proxy_call(jobject proxy, const char *method, int argc, const jobject *args) {
    typedef jobject (*invoke_fn)(void *env, jclass cls, jlong ptr, jobject name, jobject args);
    typedef jobject (*bridge_fn)(void *env, jclass cls, jlong ptr, jobject iface, jobject method, jobject args);
    static invoke_fn invoke;
    static bridge_fn bridge;
    jobj *p = as_obj(proxy);
    if (!p || strcmp(p->cls, "java/lang/reflect/Proxy") != 0) {
        droid_log("java: %s() on a %s: no Java code to run it", method, p ? p->cls : "FalsoJNI object");
        return NULL;
    }
    jobjectArray arr = jni->NewObjectArray(&jni, argc, NULL, NULL);
    for (int i = 0; i < argc; i++) jni->SetObjectArrayElement(&jni, arr, i, args[i]);
    static int logged;
    if (logged < 60) {
        logged++;
        droid_log("java: proxy %s %s() (%d args)", p->asset == (void *)1 ? "JNIBridge" : "ReflectionHelper",
                  method, argc);
    }
    if (p->asset == (void *)1) {
        if (!bridge) bridge = (bridge_fn)jni_find_native("invoke");
        if (!bridge) return NULL;
        jobject cls = bridge_iface(p, method); // the interface declaring it (getDeclaringClass)
        const char *iface = cls && !placeholder(cls) && !java_is_string(cls) ? (const char *)cls : NULL;
        dyn_method *d = dyn_find(iface, method);
        jobject m = d ? dyn_reflected(d) : NULL;
        return bridge(arm_jni_env(), find_class("bitter/jnibridge/JNIBridge"), (jlong)p->mark, cls, m, arr);
    }
    if (!invoke) invoke = (invoke_fn)jni_find_native("nativeProxyInvoke");
    if (!invoke) return NULL;
    return invoke(arm_jni_env(), find_class("com/unity3d/player/ReflectionHelper"), (jlong)p->mark, jstr(method), arr);
}

static jobject box_long_new(int64_t v) {
    char t[32];
    snprintf(t, sizeof(t), "%lld", (long long)v);
    jobj *b = new_obj("java/lang/Long");
    b->buf = strdup(t);
    return (jobject)b;
}

// ---- Looper / Handler / Message / HandlerThread: a thread with a message queue ----------

typedef struct qitem {
    int64_t due;
    jobject runnable, msg;
    jobject target, arg; // or a listener's method to call (Google Tasks' callbacks)
    const char *method;
    struct qitem *next;
} qitem;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    qitem *head;
    int started;
    char name[64];
} looper;

static looper *looper_new(const char *name) {
    looper *l = calloc(1, sizeof(*l));
    pthread_mutex_init(&l->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&l->cond, &ca);
    snprintf(l->name, sizeof(l->name), "%s", name);
    return l;
}

static void looper_post(looper *l, jobject runnable, jobject msg, int64_t delay_ns) {
    if (!l) return;
    qitem *it = calloc(1, sizeof(*it));
    it->due = now_ns() + delay_ns;
    it->runnable = runnable;
    it->msg = msg;
    pthread_mutex_lock(&l->lock);
    qitem **at = &l->head;
    while (*at && (*at)->due <= it->due) at = &(*at)->next;
    it->next = *at;
    *at = it;
    pthread_cond_signal(&l->cond);
    pthread_mutex_unlock(&l->lock);
}

static void dispatch(qitem *it) {
    if (it->method) {
        proxy_call(it->target, it->method, it->arg ? 1 : 0, it->arg ? &it->arg : NULL);
        return;
    }
    if (it->runnable) {
        proxy_call(it->runnable, "run", 0, NULL);
        return;
    }
    jobj *m = as_obj(it->msg);
    jobj *h = m ? as_obj(m->ref) : NULL;
    if (h && h->ref) proxy_call(h->ref, "handleMessage", 1, &it->msg);
    else droid_log("java: a Message (what %d) for a Handler without a callback", m ? m->field[0].i : -1);
}

static looper *main_looper(void);
static void *looper_thread(void *arg) {
    looper *l = arg;
    droid_log("java: Looper thread '%s' running", l->name);
    static char roles[8][80];
    static int n_roles;
    if (n_roles < 8) {
        snprintf(roles[n_roles], sizeof(roles[0]), "Looper %s", l->name);
        arm_thread_role(roles[n_roles++]);
    }
    for (;;) {
        pthread_mutex_lock(&l->lock);
        // waits of at most 5 ms: the game's GC pause (a signal) is taken between them
        while (!l->head || l->head->due > now_ns()) {
            int64_t due = l->head ? l->head->due : now_ns() + 5000000;
            if (due > now_ns() + 5000000) due = now_ns() + 5000000;
            struct timespec ts = { (time_t)(due / 1000000000LL), (long)(due % 1000000000LL) };
            pthread_cond_timedwait(&l->cond, &l->lock, &ts);
            pthread_mutex_unlock(&l->lock);
            arm_sig_point();
            if (l == main_looper()) { // the native looper's sensor queues (Android's main thread serves them)
                void droid_sensors_serve(void);
                droid_sensors_serve();
            }
            pthread_mutex_lock(&l->lock);
        }
        qitem *it = l->head;
        l->head = it->next;
        pthread_mutex_unlock(&l->lock);
        dispatch(it);
        free(it);
    }
    return NULL;
}

static void looper_start(looper *l) {
    if (!l || l->started) return;
    l->started = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 4 << 20);
    pthread_t t;
    pthread_create(&t, &a, looper_thread, l);
}

static looper *main_looper(void);
// a listener's method, called on the main thread after delay_ns (as Android calls them)
static void looper_post_call(looper *l, jobject target, const char *method, jobject arg, int64_t delay_ns) {
    if (!l || !target) return;
    qitem *it = calloc(1, sizeof(*it));
    it->due = now_ns() + delay_ns;
    it->target = target;
    it->method = method;
    it->arg = arg;
    pthread_mutex_lock(&l->lock);
    qitem **at = &l->head;
    while (*at && (*at)->due <= it->due) at = &(*at)->next;
    it->next = *at;
    *at = it;
    pthread_cond_signal(&l->cond);
    pthread_mutex_unlock(&l->lock);
}

static looper *main_looper(void) {
    static looper *m;
    if (!m) {
        m = looper_new("main");
        looper_start(m);
    }
    return m;
}

static jobject looper_obj(looper *l) {
    jobj *o = new_obj("android/os/Looper");
    o->asset = l;
    return (jobject)o;
}
static looper *looper_of(jobject o) {
    jobj *j = as_obj(o);
    if (!j) return main_looper();
    if (strcmp(j->cls, "android/os/Handler") == 0 || strcmp(j->cls, "android/os/Looper") == 0 ||
        strcmp(j->cls, "android/os/HandlerThread") == 0)
        return j->asset ? (looper *)j->asset : main_looper();
    return main_looper();
}

static jvalue get_main_looper(jobject self, const jvalue *a) { (void)self; (void)a; return L(looper_obj(main_looper())); }
static jvalue ht_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *t = new_obj("android/os/HandlerThread");
    t->asset = looper_new(a[0].l ? java_text(a[0].l) : "HandlerThread");
    return L(t);
}
static jvalue thread_start(jobject self, const jvalue *a) {
    (void)a;
    jobj *t = as_obj(self);
    if (t && strcmp(t->cls, "android/os/HandlerThread") == 0) looper_start(t->asset);
    else droid_log("java: Thread.start() on a %s: no Java code to run", t ? t->cls : "FalsoJNI object");
    return V();
}
static jvalue ht_get_looper(jobject self, const jvalue *a) { (void)a; return L(looper_obj(looper_of(self))); }

static jvalue handler_new(jobject self, const jvalue *a, int has_looper, int has_cb) {
    (void)self;
    jobj *h = new_obj("android/os/Handler");
    h->asset = has_looper ? looper_of(a[0].l) : main_looper();
    h->ref = has_cb ? a[1].l : NULL;
    return L(h);
}
static jvalue handler_new0(jobject self, const jvalue *a) { return handler_new(self, a, 0, 0); }
static jvalue handler_new1(jobject self, const jvalue *a) { return handler_new(self, a, 1, 0); }
static jvalue handler_new2(jobject self, const jvalue *a) { return handler_new(self, a, 1, 1); }

static jvalue handler_post(jobject self, const jvalue *a) { looper_post(looper_of(self), a[0].l, NULL, 0); return Z(JNI_TRUE); }
static jvalue handler_post_delayed(jobject self, const jvalue *a) {
    looper_post(looper_of(self), a[0].l, NULL, a[1].j * 1000000LL);
    return Z(JNI_TRUE);
}

static jobject message_new(jobject handler, jint what, jint arg1, jint arg2, jobject obj) {
    jobj *m = new_obj("android/os/Message");
    m->ref = handler;
    m->field[0].i = what;
    m->field[1].i = arg1;
    m->field[2].i = arg2;
    m->field[3].l = obj;
    return (jobject)m;
}
static jvalue obtain1(jobject self, const jvalue *a) { return L(message_new(self, a[0].i, 0, 0, NULL)); }
static jvalue obtain2(jobject self, const jvalue *a) { return L(message_new(self, a[0].i, 0, 0, a[1].l)); }
static jvalue obtain3(jobject self, const jvalue *a) { return L(message_new(self, a[0].i, a[1].i, a[2].i, NULL)); }
static jvalue obtain4(jobject self, const jvalue *a) { return L(message_new(self, a[0].i, a[1].i, a[2].i, a[3].l)); }
static jvalue send_to_target(jobject self, const jvalue *a) {
    (void)a;
    jobj *m = as_obj(self);
    if (m) looper_post(looper_of(m->ref), NULL, self, 0);
    return V();
}
static jvalue send_message(jobject self, const jvalue *a) {
    jobj *m = as_obj(a[0].l);
    if (m) {
        m->ref = self;
        looper_post(looper_of(self), NULL, a[0].l, 0);
    }
    return Z(JNI_TRUE);
}
static jvalue send_message_delayed(jobject self, const jvalue *a) {
    jobj *m = as_obj(a[0].l);
    if (m) {
        m->ref = self;
        looper_post(looper_of(self), NULL, a[0].l, a[1].j * 1000000LL);
    }
    return Z(JNI_TRUE);
}
static jvalue send_empty(jobject self, const jvalue *a) {
    looper_post(looper_of(self), NULL, message_new(self, a[0].i, 0, 0, NULL), 0);
    return Z(JNI_TRUE);
}
static jvalue send_empty_delayed(jobject self, const jvalue *a) {
    looper_post(looper_of(self), NULL, message_new(self, a[0].i, 0, 0, NULL), a[1].j * 1000000LL);
    return Z(JNI_TRUE);
}
static jvalue run_on_ui(jobject self, const jvalue *a) {
    (void)self;
    looper_post(main_looper(), a[0].l, NULL, 0);
    return V();
}

// android.view.Choreographer: postFrameCallback(cb) -> cb.doFrame(nanos) at the next tick
static jvalue nano_time(jobject self, const jvalue *a) { (void)self; (void)a; return J(now_ns()); }
// android.os.SystemClock: uptime and elapsed time in ms (Geometry Dash asks uptimeMillis per touch)
static jvalue uptime_ms(jobject self, const jvalue *a) { (void)self; (void)a; return J(now_ns() / 1000000); }

static jvalue choreo_get(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(new_obj("android/view/Choreographer"));
}
static jvalue choreo_post(jobject self, const jvalue *a) { (void)self; post_frame(a[0].l, 0); return V(); }
static jvalue choreo_post_delayed(jobject self, const jvalue *a) {
    (void)self;
    post_frame(a[0].l, a[1].j * 1000000LL);
    return V();
}

// new String(byte[], charset)
static jvalue string_from_bytes(jobject self, const jvalue *a) {
    (void)self;
    jsize n = a[0].l ? jni->GetArrayLength(&jni, (jarray)a[0].l) : 0;
    char *t = calloc((size_t)n + 1, 1);
    if (n) jni->GetByteArrayRegion(&jni, (jbyteArray)a[0].l, 0, n, (jbyte *)t);
    jobject s = jstr(t);
    free(t);
    return L(s);
}

// ---- the table -------------------------------------------------------------------------

#define IS "java/io/InputStream"
// ---- java.nio direct ByteBuffers: memory the native side reads and writes -----------
// (FalsoJNI has none.) The address is in asset, the capacity in len.

jobject java_new_direct_buffer(void *address, int64_t capacity) {
    jobj *b = new_obj("java/nio/DirectByteBuffer");
    b->asset = address;
    b->len = (size_t)capacity;
    return (jobject)b;
}
static int is_direct(jobj *b) { return b && strcmp(b->cls, "java/nio/DirectByteBuffer") == 0; }
void *java_direct_address(jobject o) { jobj *b = as_obj(o); return is_direct(b) ? b->asset : NULL; }
int64_t java_direct_capacity(jobject o) { jobj *b = as_obj(o); return is_direct(b) ? (int64_t)b->len : -1; }

// ---- org.fmod.FMODAudioDevice: FMOD's Java output (what fmod.jar does) ----------------
// A thread asks FMOD for blocks - fmodProcess(ByteBuffer), registered by libunity -
// and plays them: 16-bit stereo at FMOD's rate on a PS5 audio port of its own.

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *buf);

#define FMOD_GRAIN 256 // frames per PS5 audio output
static volatile int fmod_running;
static pthread_t fmod_thread_id;

static void *fmod_thread(void *arg) {
    jobject self = (jobject)arg;
    typedef jint (*info_fn)(void *env, jobject self, jint what);
    typedef jint (*process_fn)(void *env, jobject self, jobject buffer);
    info_fn info = (info_fn)jni_find_native("fmodGetInfo");
    process_fn process = (process_fn)jni_find_native("fmodProcess");
    if (!info || !process) {
        droid_log("java: FMOD audio: natives fmodGetInfo/fmodProcess missing");
        return NULL;
    }
    int got[6];
    for (int i = 0; i < 6; i++) got[i] = info(arm_jni_env(), self, i);
    int rate = got[0] > 0 ? got[0] : 48000, block = got[1] > 0 && got[1] <= 8192 ? got[1] : 1024;
    droid_log("java: FMOD audio: info %d %d %d %d %d %d -> %d Hz, blocks of %d frames", got[0], got[1], got[2],
              got[3], got[4], got[5], rate, block);
    sceAudioOutInit();
    int port = sceAudioOutOpen(0xff, 0, 0, FMOD_GRAIN, 48000, 1); // 16-bit stereo, 48 kHz (the PS5's)
    droid_log("java: FMOD audio: PS5 audio port %#x", port);
    if (port < 0) return NULL;
    // The block FMOD fills: 16-bit stereo is block * 4 bytes; room for 8 channels of
    // floats besides, so a bigger write never runs past it. A marker shows what it wrote.
    size_t room = (size_t)block * 8 * 4 + 4096;
    unsigned char *raw = malloc(room);
    jobject buffer = java_new_direct_buffer(raw, (int64_t)block * 4);
    int is_float = -1; // found from the first blocks' size
    float *fifo = malloc(sizeof(float) * 2 * (8192 + 4));
    int have = 0, blocks = 0;
    double pos = 0, step = (double)rate / 48000; // FMOD's frames per PS5 frame
    int16_t out[FMOD_GRAIN * 2];
    while (fmod_running) {
        while (have < (int)(pos + step * FMOD_GRAIN) + 2) { // enough FMOD frames for a grain
            memset(raw, 0xA5, room);
            int r = process(arm_jni_env(), self, buffer);
            size_t wrote = room;
            while (wrote > 0 && raw[wrote - 1] == 0xA5) wrote--;
            if (blocks++ < 4)
                droid_log("java: FMOD audio: fmodProcess -> %d, %zu bytes written (block %d frames)", r, wrote, block);
            if (is_float < 0 && wrote > (size_t)block * 4 + 64) is_float = 1;
            else if (is_float < 0 && blocks >= 8) is_float = 0;
            if (have + block > 8192) have = 0; // never: the port drains it
            for (int i = 0; i < block * 2; i++)
                fifo[have * 2 + i] = is_float == 1 ? ((float *)raw)[i] : ((int16_t *)raw)[i] / 32768.0f;
            have += block;
        }
        for (int f = 0; f < FMOD_GRAIN; f++, pos += step) { // linear resampling to 48 kHz
            int i = (int)pos;
            float t = (float)(pos - i);
            for (int c = 0; c < 2; c++) {
                float v = fifo[i * 2 + c] * (1 - t) + fifo[(i + 1) * 2 + c] * t;
                v = v > 1 ? 1 : v < -1 ? -1 : v;
                out[f * 2 + c] = (int16_t)(v * 32767);
            }
        }
        int used = (int)pos;
        pos -= used;
        have -= used;
        memmove(fifo, fifo + used * 2, sizeof(float) * 2 * (size_t)have);
        droid_audio_mute_if_paused(out, FMOD_GRAIN); // (the Quick menu open: silent)
        sceAudioOutOutput(port, out); // blocks for one grain: the clock
        arm_sig_point();              // the game's GC pause, between blocks
    }
    droid_log("java: FMOD audio stopped");
    return NULL;
}

// ---- com.halfbrick.mortar.MortarAudioMixerOut: Mortar's own mixer, written out -------------
// Mortar mixes its sound itself and hands Java blocks of 16-bit stereo (WriteData(short[])) for
// an AudioTrack, whose write blocks while it plays - the audio thread's clock. Stubbed, write
// returned at once: no sound, and the audio thread spun (160 000 JNI calls in 5 s, a core).
// Here: a PS5 audio port at 48 kHz (the rate GetNativeSampleRate tells it), the blocking output.
#define MORTAR_GRAIN 256
static struct { int port, rate, have; int16_t buf[MORTAR_GRAIN * 2]; double pos; int16_t last[2]; } mortar_out;
static jvalue mortar_rate(jobject self, const jvalue *a) { (void)self; (void)a; return I(48000); }
static jvalue mortar_create(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L((jobject)new_obj("com/halfbrick/mortar/MortarAudioMixerOut"));
}
static jvalue mortar_init(jobject self, const jvalue *a) {
    (void)self;
    mortar_out.rate = a[0].i > 0 ? a[0].i : 48000;
    if (mortar_out.port <= 0) {
        sceAudioOutInit();
        mortar_out.port = sceAudioOutOpen(0xff, 0, 0, MORTAR_GRAIN, 48000, 1); // 16-bit stereo
    }
    droid_log("java: Mortar audio: %d Hz in, PS5 audio port %#x", mortar_out.rate, mortar_out.port);
    return I(mortar_out.rate);
}
static jvalue mortar_write(jobject self, const jvalue *a) {
    (void)self;
    if (mortar_out.port <= 0 || !a[0].l) return V();
    jsize n = jni->GetArrayLength(&jni, a[0].l) / 2; // frames
    static int16_t *in;
    static jsize in_cap;
    if (n > in_cap) {
        in = realloc(in, (size_t)n * 4);
        in_cap = n;
    }
    jni->GetShortArrayRegion(&jni, a[0].l, 0, n * 2, in);
    const double step = (double)mortar_out.rate / 48000;
    for (double p = mortar_out.pos; p < n; p += step) { // (nearest frame: 48 kHz in is a copy)
        int i = (int)p;
        mortar_out.buf[mortar_out.have * 2] = in[i * 2];
        mortar_out.buf[mortar_out.have * 2 + 1] = in[i * 2 + 1];
        if (++mortar_out.have == MORTAR_GRAIN) {
            droid_audio_mute_if_paused(mortar_out.buf, MORTAR_GRAIN); // (the Quick menu open: silent)
            sceAudioOutOutput(mortar_out.port, mortar_out.buf); // blocks for one grain: the clock
            mortar_out.have = 0;
        }
        mortar_out.pos = p + step - n; // carried into the next block
    }
    return V();
}

// Mortar's HttpClient (Java we don't have): the game sends a request, then reads the object's
// IsFinished every frame (Jetpack Joyride Classic's loading waited on it forever). Here a request
// is over at once, failed as on a phone with no network: IsFinished true, nothing else.
static jvalue mortar_http_new(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L((jobject)new_obj("com/halfbrick/mortar/HttpClient"));
}

static jvalue mortar_http_request(jobject self, const jvalue *a) { // (url, method, headers, body, timeout, id)
    jobj *j = as_obj(self);
    static int told;
    if (told++ < 8) droid_log("java: mortar HttpClient %s %s: no network (finished, failed)", cstr(a[1].l) ? cstr(a[1].l) : "?",
                              cstr(a[0].l) ? cstr(a[0].l) : "?");
    if (j) {
        j->field[1].j = a[4].j;                       // RequestPointer: the game's request, given back
        j->field[2].l = jni->NewByteArray(&jni, 0); // Result: no bytes
        j->field[0].z = JNI_TRUE;                     // IsFinished (last: the game reads the rest then)
    }
    return V();
}

static jvalue fmod_new(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_log("java: new org.fmod.FMODAudioDevice");
    return L((jobject)new_obj("org/fmod/FMODAudioDevice"));
}
static jvalue fmod_start(jobject self, const jvalue *a) {
    (void)a;
    if (!fmod_running) {
        fmod_running = 1;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 2 << 20);
        pthread_create(&fmod_thread_id, &at, fmod_thread, self);
        droid_log("java: FMOD audio started");
    }
    return V();
}
static jvalue fmod_stop(jobject self, const jvalue *a) { (void)self; (void)a; fmod_running = 0; return V(); }
static jvalue fmod_is_running(jobject self, const jvalue *a) { (void)self; (void)a; return Z(fmod_running); }
// the microphone: none (FMOD needs the methods to be there)
static jvalue fmod_no_record(jobject self, const jvalue *a) { (void)self; (void)a; return I(-1); }
static jvalue fmod_stop_record(jobject self, const jvalue *a) { (void)self; (void)a; return V(); }

// ---- input: android.view.MotionEvent / KeyEvent / InputDevice -----------------------
// Unity reads an event through these getters (nativeInjectEvent(event, display)).
// Two devices: 1 = the touchscreen (DualSense touchpad and the game's touch map),
// 2 = the DualSense as a gamepad.

#define ME "android/view/MotionEvent"
#define KE "android/view/KeyEvent"
#define ID "android/view/InputDevice"
#define MR "android/view/InputDevice$MotionRange"
#define MAX_POINTERS 8

typedef struct {
    int action, source, device, count, tool, buttons;
    int ids[MAX_POINTERS];
    float x[MAX_POINTERS], y[MAX_POINTERS];
    float axes[48];
    int64_t time, down_time; // ns
} motion_data;

typedef struct {
    int action, keycode, source, device, repeat, meta;
    int64_t time, down_time;
} key_data;

static motion_data *motion_of(jobject o) {
    jobj *j = as_obj(o);
    return j && strcmp(j->cls, ME) == 0 ? (motion_data *)j->buf : NULL;
}
static key_data *key_of(jobject o) {
    jobj *j = as_obj(o);
    return j && strcmp(j->cls, KE) == 0 ? (key_data *)j->buf : NULL;
}

jobject java_motion_event(int action, int source, int device, int count, const int *ids, const float *x,
                          const float *y, const float *axes, int64_t time_ns, int64_t down_ns) {
    jobj *o = new_obj(ME);
    motion_data *m = calloc(1, sizeof(*m));
    m->action = action;
    m->source = source;
    m->device = device;
    m->count = count > MAX_POINTERS ? MAX_POINTERS : count;
    m->tool = source == 0x1002 ? 1 : 0; // TOOL_TYPE_FINGER for the touchscreen
    for (int i = 0; i < m->count; i++) {
        m->ids[i] = ids ? ids[i] : i;
        m->x[i] = x ? x[i] : 0;
        m->y[i] = y ? y[i] : 0;
    }
    if (axes) memcpy(m->axes, axes, sizeof(m->axes));
    m->time = time_ns;
    m->down_time = down_ns;
    o->buf = (char *)m;
    return (jobject)o;
}

// An event we made, gone (its copy from obtain() too, when that one is passed here)
void java_free_event(jobject o) {
    jobj *j = as_obj(o);
    if (!j || (strcmp(j->cls, ME) != 0 && strcmp(j->cls, KE) != 0)) return;
    free(j->buf);
    j->buf = NULL;
    j->magic = 0; // a late use shows as "not our object", not as freed memory
    free(j);
}

jobject java_key_event(int action, int keycode, int source, int device, int repeat, int64_t time_ns,
                       int64_t down_ns) {
    jobj *o = new_obj(KE);
    key_data *k = calloc(1, sizeof(*k));
    k->action = action;
    k->keycode = keycode;
    k->source = source;
    k->device = device;
    k->repeat = repeat;
    k->time = time_ns;
    k->down_time = down_ns;
    o->buf = (char *)k;
    return (jobject)o;
}

static jvalue F(float f) { jvalue v; v.j = 0; v.f = f; return v; }

// cocos2d-x: the screen's refresh (RobTop's activity; unanswered 0, the director's frame time became
// 1/0 and Geometry Dash's smooth fix fed inf into its physics: a level froze a second in), and the
// frame time the engine asks for (cocos_host.c paces to it)
double cocos_anim_interval = 1.0 / 60;
static jvalue cc_refresh_rate(jobject self, const jvalue *a) { (void)self; (void)a; return F(60.0f); }
static jvalue cc_set_interval(jobject self, const jvalue *a) {
    (void)self;
    if (a[0].d > 0.001 && a[0].d < 1) cocos_anim_interval = a[0].d;
    droid_log("java: cocos2d-x frame time %.4f s asked", a[0].d);
    return V();
}
static int ptr_index(const motion_data *m, int i) { return i >= 0 && i < m->count ? i : 0; }

// pointer i's axis (AXIS_X 0, AXIS_Y 1, pressure 2, size 3, touch major 4); joystick axes
static float motion_axis(const motion_data *m, int axis, int i) {
    i = ptr_index(m, i);
    if (m->source == 0x1002) {
        switch (axis) {
        case 0: return m->x[i];
        case 1: return m->y[i];
        case 2: return 1.0f;
        case 3: return 0.05f;
        case 4: case 5: return 20.0f;
        default: return 0;
        }
    }
    return axis >= 0 && axis < 48 ? m->axes[axis] : 0;
}

static jvalue me_action(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return I(m ? m->action : 0); }
static jvalue me_action_masked(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return I(m ? m->action & 0xff : 0); }
static jvalue me_action_index(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return I(m ? (m->action >> 8) & 0xff : 0); }
static jvalue me_count(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return I(m ? m->count : 0); }
static jvalue me_pointer_id(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return I(m ? m->ids[ptr_index(m, a[0].i)] : 0); }
static jvalue me_find_pointer(jobject s, const jvalue *a) {
    motion_data *m = motion_of(s);
    for (int i = 0; m && i < m->count; i++)
        if (m->ids[i] == a[0].i) return I(i);
    return I(-1);
}
static jvalue me_x0(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return F(m ? motion_axis(m, 0, 0) : 0); }
static jvalue me_y0(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return F(m ? motion_axis(m, 1, 0) : 0); }
static jvalue me_x(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, 0, a[0].i) : 0); }
static jvalue me_y(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, 1, a[0].i) : 0); }
static jvalue me_axis0(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, a[0].i, 0) : 0); }
static jvalue me_axis(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, a[0].i, a[1].i) : 0); }
static jvalue me_pressure0(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return F(m ? motion_axis(m, 2, 0) : 0); }
static jvalue me_pressure(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, 2, a[0].i) : 0); }
static jvalue me_size(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, 3, a[0].i) : 0); }
static jvalue me_touch_major(jobject s, const jvalue *a) { motion_data *m = motion_of(s); return F(m ? motion_axis(m, 4, a[0].i) : 0); }
static jvalue me_orientation(jobject s, const jvalue *a) { (void)s; (void)a; return F(0); }
static jvalue me_tool(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return I(m ? m->tool : 0); }
static jvalue me_buttons(jobject s, const jvalue *a) { (void)a; motion_data *m = motion_of(s); return I(m ? m->buttons : 0); }
static jvalue me_obtain(jobject s, const jvalue *a) { // MotionEvent.obtain(other): a copy
    (void)s;
    motion_data *m = motion_of(a[0].l);
    if (!m) return L(NULL);
    jobject c = java_motion_event(m->action, m->source, m->device, m->count, m->ids, m->x, m->y, m->axes,
                                  m->time, m->down_time);
    return L(c);
}
static jvalue ie_zero(jobject s, const jvalue *a) { (void)s; (void)a; return I(0); }
static jvalue ie_void(jobject s, const jvalue *a) { (void)s; (void)a; return V(); }

// InputEvent's own: either kind
static jvalue ie_source(jobject s, const jvalue *a) {
    (void)a;
    motion_data *m = motion_of(s);
    key_data *k = key_of(s);
    return I(m ? m->source : k ? k->source : 0);
}
static jvalue ie_device_id(jobject s, const jvalue *a) {
    (void)a;
    motion_data *m = motion_of(s);
    key_data *k = key_of(s);
    return I(m ? m->device : k ? k->device : 0);
}
static jvalue ie_time(jobject s, const jvalue *a) { // ms, like Android
    (void)a;
    motion_data *m = motion_of(s);
    key_data *k = key_of(s);
    return J((m ? m->time : k ? k->time : 0) / 1000000);
}
static jvalue ie_time_nanos(jobject s, const jvalue *a) {
    (void)a;
    motion_data *m = motion_of(s);
    key_data *k = key_of(s);
    return J(m ? m->time : k ? k->time : 0);
}
static jvalue ie_down_time(jobject s, const jvalue *a) {
    (void)a;
    motion_data *m = motion_of(s);
    key_data *k = key_of(s);
    return J((m ? m->down_time : k ? k->down_time : 0) / 1000000);
}
static jvalue ie_from_source(jobject s, const jvalue *a) {
    int src = ie_source(s, a).i;
    return Z((src & a[0].i) == a[0].i);
}
static jobject input_device(int id);
static jvalue ie_device(jobject s, const jvalue *a) { return L(input_device(ie_device_id(s, a).i)); }

static jvalue ke_action(jobject s, const jvalue *a) { (void)a; key_data *k = key_of(s); return I(k ? k->action : 0); }
static jvalue ke_keycode(jobject s, const jvalue *a) { (void)a; key_data *k = key_of(s); return I(k ? k->keycode : 0); }
static jvalue ke_repeat(jobject s, const jvalue *a) { (void)a; key_data *k = key_of(s); return I(k ? k->repeat : 0); }
static jvalue ke_meta(jobject s, const jvalue *a) { (void)a; key_data *k = key_of(s); return I(k ? k->meta : 0); }
static jvalue ke_unicode(jobject s, const jvalue *a) {
    (void)a;
    key_data *k = key_of(s);
    if (!k) return I(0);
    if (k->keycode == 62) return I(' ');           // SPACE
    if (k->keycode == 66) return I('\n');          // ENTER
    return I(0);
}
static jvalue ke_char_map(jobject s, const jvalue *a) { (void)s; (void)a; return L((jobject)new_obj("android/view/KeyCharacterMap")); }

// InputDevice: 1 touchscreen, 2 DualSense
static jobject devices[3];
static jobject input_device(int id) {
    if (id < 1 || id > 2) return NULL;
    if (!devices[id]) {
        jobj *d = new_obj(ID);
        d->mark = id;
        devices[id] = (jobject)d;
    }
    return devices[id];
}
static int device_id(jobject s) { jobj *j = as_obj(s); return j ? (int)j->mark : 0; }
static jvalue id_ids(jobject s, const jvalue *a) {
    (void)s; (void)a;
    jintArray arr = jni->NewIntArray(&jni, 2);
    jint ids[2] = { 1, 2 };
    jni->SetIntArrayRegion(&jni, arr, 0, 2, ids);
    return L((jobject)arr);
}
static jvalue id_get(jobject s, const jvalue *a) { (void)s; return L(input_device(a[0].i)); }
static jvalue id_id(jobject s, const jvalue *a) { (void)a; return I(device_id(s)); }
static jvalue id_name(jobject s, const jvalue *a) {
    (void)a;
    return L(jstr(device_id(s) == 2 ? "DualSense Wireless Controller" : "PS5 touchscreen"));
}
static jvalue id_descriptor(jobject s, const jvalue *a) {
    (void)a;
    return L(jstr(device_id(s) == 2 ? "ps5-dualsense-0" : "ps5-touchscreen-0"));
}
static jvalue id_vendor(jobject s, const jvalue *a) { (void)a; return I(device_id(s) == 2 ? 0x054c : 0); }
static jvalue id_product(jobject s, const jvalue *a) { (void)a; return I(device_id(s) == 2 ? 0x0ce6 : 0); }
static jvalue id_sources(jobject s, const jvalue *a) {
    (void)a;
    return I(device_id(s) == 2 ? (0x401 | 0x1000010) : 0x1002); // GAMEPAD|JOYSTICK : TOUCHSCREEN
}
static jvalue id_supports(jobject s, const jvalue *a) { return Z((id_sources(s, a).i & a[0].i) == a[0].i); }
static jvalue id_keyboard(jobject s, const jvalue *a) { (void)a; return I(device_id(s) == 2 ? 1 : 0); } // NON_ALPHABETIC
static jvalue id_false(jobject s, const jvalue *a) { (void)s; (void)a; return Z(0); }
static jvalue id_true(jobject s, const jvalue *a) { (void)s; (void)a; return Z(1); }
static jvalue id_controller(jobject s, const jvalue *a) { (void)a; return I(device_id(s) == 2 ? 1 : 0); }
static jvalue id_range(jobject s, const jvalue *a) {
    int dev = device_id(s), axis = a[0].i;
    float lo, hi;
    if (dev == 1) {
        if (axis == 0) { lo = 0; hi = 1919; }
        else if (axis == 1) { lo = 0; hi = 1079; }
        else if (axis == 2 || axis == 3) { lo = 0; hi = 1; }
        else return L(NULL);
    } else {
        if (axis == 0 || axis == 1 || axis == 11 || axis == 14 || axis == 15 || axis == 16) { lo = -1; hi = 1; }
        else if (axis == 17 || axis == 18 || axis == 22 || axis == 23) { lo = 0; hi = 1; }
        else return L(NULL);
    }
    jobj *r = new_obj(MR);
    r->mark = axis;
    r->field[0].f = lo;
    r->field[1].f = hi;
    r->field[2].i = dev == 1 ? 0x1002 : 0x1000010;
    return L((jobject)r);
}
static jvalue mr_axis(jobject s, const jvalue *a) { (void)a; jobj *j = as_obj(s); return I(j ? (int)j->mark : 0); }
static jvalue mr_min(jobject s, const jvalue *a) { (void)a; jobj *j = as_obj(s); return F(j ? j->field[0].f : 0); }
static jvalue mr_max(jobject s, const jvalue *a) { (void)a; jobj *j = as_obj(s); return F(j ? j->field[1].f : 0); }
static jvalue mr_range(jobject s, const jvalue *a) { (void)a; jobj *j = as_obj(s); return F(j ? j->field[1].f - j->field[0].f : 0); }
static jvalue mr_flat(jobject s, const jvalue *a) { (void)a; jobj *j = as_obj(s); return F(j && j->field[0].f < 0 ? 0.05f : 0); }
static jvalue mr_zero_f(jobject s, const jvalue *a) { (void)s; (void)a; return F(0); }
static jvalue mr_source(jobject s, const jvalue *a) { (void)a; jobj *j = as_obj(s); return I(j ? j->field[2].i : 0); }


// ---- the plugin library, part 1: what every game's plugins pass around ---------------------
// Java's lists and maps, boxed numbers, dates, exceptions; Android's Bundle, Intent extras and
// Uri. Real working ones (a plugin keeps values in them and reads them back). A list keeps its
// objects in buf (len of them, room for cap); a map, Bundle or Intent keeps key/value pairs.
#define TAG_LIST 0x4c495354
#define TAG_MAP 0x4d415020
#define TAG_VIEW 0x56494557
typedef struct { jobject k, v; } kv_pair;

static int same_obj(jobject a, jobject b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (java_is_string(a) && java_is_string(b)) return !strcmp(java_text(a), java_text(b));
    jobj *x = as_obj(a), *y = as_obj(b);
    return x && y && x->mark != TAG_LIST && x->mark != TAG_MAP && x->buf && y->buf && x->cls && y->cls &&
           !strcmp(x->cls, y->cls) && !strcmp(x->buf, y->buf); // boxed numbers
}

static jobj *list_of(jobject o) { jobj *j = as_obj(o); return j && j->mark == TAG_LIST ? j : NULL; }
static jobj *map_of(jobject o) { jobj *j = as_obj(o); return j && j->mark == TAG_MAP ? j : NULL; }
static jobj *new_list(const char *cls) { jobj *j = new_obj(cls); j->mark = TAG_LIST; return j; }
static jobj *new_map(const char *cls) { jobj *j = new_obj(cls); j->mark = TAG_MAP; return j; }

static int grow(jobj *j, size_t each) {
    if (j->len < j->cap) return 1;
    size_t c = j->cap ? j->cap * 2 : 8;
    char *b = realloc(j->buf, c * each);
    if (!b) return 0;
    j->buf = b;
    j->cap = c;
    return 1;
}
static void list_push(jobj *l, jobject v) {
    if (grow(l, sizeof(jobject))) ((jobject *)l->buf)[l->len++] = v;
}
static kv_pair *map_find(jobj *m, jobject k) {
    for (size_t i = 0; m && i < m->len; i++)
        if (same_obj(((kv_pair *)m->buf)[i].k, k)) return &((kv_pair *)m->buf)[i];
    return NULL;
}
static jobject map_set(jobj *m, jobject k, jobject v) {
    if (!m) return NULL;
    kv_pair *e = map_find(m, k);
    if (e) {
        jobject old = e->v;
        e->v = v;
        return old;
    }
    if (grow(m, sizeof(kv_pair))) ((kv_pair *)m->buf)[m->len++] = (kv_pair){ k, v };
    return NULL;
}
static jobject map_take(jobj *m, jobject k) {
    kv_pair *e = map_find(m, k);
    if (!e) return NULL;
    jobject old = e->v;
    size_t i = (size_t)(e - (kv_pair *)m->buf);
    memmove(e, e + 1, (m->len - i - 1) * sizeof(kv_pair));
    m->len--;
    return old;
}

// lists: ArrayList, LinkedList, List, Collection
static jvalue l_new(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_list("java/util/ArrayList")); }
static jvalue l_add(jobject self, const jvalue *a) { jobj *l = list_of(self); if (l) list_push(l, a[0].l); return Z(l != NULL); }
static jvalue l_add_at(jobject self, const jvalue *a) {
    jobj *l = list_of(self);
    if (!l || a[0].i < 0 || (size_t)a[0].i > l->len || !grow(l, sizeof(jobject))) return V();
    jobject *v = (jobject *)l->buf;
    memmove(v + a[0].i + 1, v + a[0].i, (l->len - (size_t)a[0].i) * sizeof(jobject));
    v[a[0].i] = a[1].l;
    l->len++;
    return V();
}
static jvalue l_get(jobject self, const jvalue *a) {
    jobj *l = list_of(self);
    return L(l && a[0].i >= 0 && (size_t)a[0].i < l->len ? ((jobject *)l->buf)[a[0].i] : NULL);
}
static jvalue l_set(jobject self, const jvalue *a) {
    jobj *l = list_of(self);
    if (!l || a[0].i < 0 || (size_t)a[0].i >= l->len) return L(NULL);
    jobject old = ((jobject *)l->buf)[a[0].i];
    ((jobject *)l->buf)[a[0].i] = a[1].l;
    return L(old);
}
static jvalue l_remove_at(jobject self, const jvalue *a) {
    jobj *l = list_of(self);
    if (!l || a[0].i < 0 || (size_t)a[0].i >= l->len) return L(NULL);
    jobject *v = (jobject *)l->buf, old = v[a[0].i];
    memmove(v + a[0].i, v + a[0].i + 1, (l->len - (size_t)a[0].i - 1) * sizeof(jobject));
    l->len--;
    return L(old);
}
static jvalue l_size(jobject self, const jvalue *a) {
    (void)a;
    jobj *l = list_of(self), *m = map_of(self);
    return I(l ? (jint)l->len : m ? (jint)m->len : 0);
}
static jvalue l_is_empty(jobject self, const jvalue *a) { return Z(l_size(self, a).i == 0); }
static jvalue l_clear(jobject self, const jvalue *a) {
    (void)a;
    jobj *j = as_obj(self);
    if (j && (j->mark == TAG_LIST || j->mark == TAG_MAP)) j->len = 0;
    return V();
}
static jvalue l_contains(jobject self, const jvalue *a) {
    jobj *l = list_of(self);
    for (size_t i = 0; l && i < l->len; i++)
        if (same_obj(((jobject *)l->buf)[i], a[0].l)) return Z(JNI_TRUE);
    return Z(JNI_FALSE);
}
static jvalue l_to_array(jobject self, const jvalue *a) {
    (void)a;
    jobj *l = list_of(self);
    jobjectArray arr = jni->NewObjectArray(&jni, l ? (jsize)l->len : 0, NULL, NULL);
    for (size_t i = 0; l && i < l->len; i++) jni->SetObjectArrayElement(&jni, arr, (jsize)i, ((jobject *)l->buf)[i]);
    return L(arr);
}
// a view of a list or map to iterate: mode 0 the list, 1 the keys, 2 the values, 3 the entries
static jobject view(jobject of, int mode, const char *cls) {
    jobj *v = new_obj(cls);
    v->mark = TAG_VIEW;
    v->ref = of;
    v->field[0].i = mode;
    return (jobject)v;
}
static jvalue l_iterator(jobject self, const jvalue *a) {
    (void)a;
    jobj *s = as_obj(self);
    jobject of = s && s->mark == TAG_VIEW ? s->ref : self;
    int mode = s && s->mark == TAG_VIEW ? s->field[0].i : 0;
    jobj *it = new_obj("java/util/Iterator");
    it->ref = of;
    it->field[0].i = mode;
    it->field[1].i = 1; // ours (not a SharedPreferences store's)
    return L((jobject)it);
}
// the n-th thing a view gives (NULL past the end)
static int view_count(jobj *it) {
    jobj *l = list_of(it->ref), *m = map_of(it->ref);
    return l ? (int)l->len : m ? (int)m->len : 0;
}
static jobject view_item(jobj *it, int n) {
    jobj *l = list_of(it->ref), *m = map_of(it->ref);
    if (l) return ((jobject *)l->buf)[n];
    if (!m) return NULL;
    kv_pair *e = &((kv_pair *)m->buf)[n];
    if (it->field[0].i == 1) return e->k;
    if (it->field[0].i == 2) return e->v;
    jobj *en = new_obj("java/util/Map$Entry");
    en->field[0].l = e->k;
    en->field[1].l = e->v;
    return (jobject)en;
}

// maps: HashMap, Map, LinkedHashMap, TreeMap (and Bundle / Intent extras share them)
static jvalue m_new(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_map("java/util/HashMap")); }
static jvalue m_put(jobject self, const jvalue *a) { return L(map_set(map_of(self), a[0].l, a[1].l)); }
static jvalue m_get(jobject self, const jvalue *a) { kv_pair *e = map_find(map_of(self), a[0].l); return L(e ? e->v : NULL); }
static jvalue m_contains(jobject self, const jvalue *a) { return Z(map_find(map_of(self), a[0].l) != NULL); }
static jvalue m_remove(jobject self, const jvalue *a) { return L(map_take(map_of(self), a[0].l)); }
static jvalue m_key_set(jobject self, const jvalue *a) { (void)a; return L(view(self, 1, "java/util/Set")); }
static jvalue m_values(jobject self, const jvalue *a) { (void)a; return L(view(self, 2, "java/util/Collection")); }
static jvalue m_entries(jobject self, const jvalue *a) { (void)a; return L(view(self, 3, "java/util/Set")); }

// boxed numbers' values, and new Byte / Short / Character
static jvalue box_b(jobject self, const jvalue *a) { (void)self; return L(box('B', a[0])); }
static jvalue box_s(jobject self, const jvalue *a) { (void)self; return L(box('S', a[0])); }
static jvalue box_c(jobject self, const jvalue *a) { (void)self; return L(box('C', a[0])); }
static jvalue unbox_b(jobject self, const jvalue *a) { (void)a; jvalue v; v.j = 0; v.b = (jbyte)atoi(box_text(self)); return v; }
static jvalue unbox_s(jobject self, const jvalue *a) { (void)a; jvalue v; v.j = 0; v.s = (jshort)atoi(box_text(self)); return v; }
static jvalue unbox_c(jobject self, const jvalue *a) { (void)a; jvalue v; v.j = 0; v.c = (jchar)atoi(box_text(self)); return v; }
static jvalue unbox_d(jobject self, const jvalue *a) { (void)a; jvalue v; v.d = strtod(box_text(self), NULL); return v; }
static jvalue box_to_string(jobject self, const jvalue *a) { (void)a; return L(jstr(box_text(self))); }

// java.util.Date (milliseconds in mark), System.identityHashCode
static jvalue date_now(jobject self, const jvalue *a) {
    (void)self; (void)a;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    jobj *d = new_obj("java/util/Date");
    d->mark = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    return L((jobject)d);
}
static jvalue date_at(jobject self, const jvalue *a) { (void)self; jobj *d = new_obj("java/util/Date"); d->mark = a[0].j; return L((jobject)d); }
static jvalue date_time(jobject self, const jvalue *a) { (void)a; jobj *d = as_obj(self); return J(d ? d->mark : 0); }
static jvalue identity_hash(jobject self, const jvalue *a) { (void)self; return I((jint)(((uintptr_t)a[0].l >> 4) & 0x7fffffff)); }

// exceptions: their message (the plugins log them, Firebase records them)
static jvalue exc_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *e = new_obj("java/lang/Exception");
    const char *msg = a && a[0].l ? cstr(a[0].l) : NULL;
    e->buf = strdup(msg ? msg : "");
    return L((jobject)e);
}
static jvalue exc_message(jobject self, const jvalue *a) { (void)a; jobj *e = as_obj(self); return L(e && e->buf ? jstr(e->buf) : NULL); }

// Bundle: typed puts and gets over a map (values boxed)
static jvalue bundle_new(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_map("android/os/Bundle")); }
static jvalue bundle_copy(jobject self, const jvalue *a) {
    (void)self;
    jobj *b = new_map("android/os/Bundle"), *from = map_of(a[0].l);
    for (size_t i = 0; from && i < from->len; i++) map_set(b, ((kv_pair *)from->buf)[i].k, ((kv_pair *)from->buf)[i].v);
    return L((jobject)b);
}
static jvalue b_put_obj(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, a[1].l); return V(); }
static jvalue b_put_i(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('I', a[1])); return V(); }
static jvalue b_put_j(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('J', a[1])); return V(); }
static jvalue b_put_z(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('Z', a[1])); return V(); }
static jvalue b_put_f(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('F', a[1])); return V(); }
static jvalue b_put_d(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('D', a[1])); return V(); }
static jvalue b_put_all(jobject self, const jvalue *a) {
    jobj *to = map_of(self), *from = map_of(a[0].l);
    for (size_t i = 0; to && from && i < from->len; i++) map_set(to, ((kv_pair *)from->buf)[i].k, ((kv_pair *)from->buf)[i].v);
    return V();
}
static const char *b_text(jobject self, jobject key) { kv_pair *e = map_find(map_of(self), key); return e && e->v ? box_text(e->v) : NULL; }
static jvalue b_get_obj(jobject self, const jvalue *a) { kv_pair *e = map_find(map_of(self), a[0].l); return L(e ? e->v : NULL); }
static jvalue b_get_str(jobject self, const jvalue *a) {
    kv_pair *e = map_find(map_of(self), a[0].l);
    return L(e && e->v && java_is_string(e->v) ? e->v : NULL);
}
static jvalue b_get_str_or(jobject self, const jvalue *a) { jvalue r = b_get_str(self, a); return r.l ? r : L(a[1].l); }
static jvalue b_get_i(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); return I(t ? atoi(t) : 0); }
static jvalue b_get_i_or(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); return I(t ? atoi(t) : a[1].i); }
static jvalue b_get_j(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); return J(t ? strtoll(t, NULL, 10) : 0); }
static jvalue b_get_j_or(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); return J(t ? strtoll(t, NULL, 10) : a[1].j); }
static jvalue b_get_z(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); return Z(t && t[0] == '1'); }
static jvalue b_get_z_or(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); return Z(t ? t[0] == '1' : a[1].z); }
static jvalue b_get_f(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); jvalue v; v.j = 0; v.f = t ? strtof(t, NULL) : 0; return v; }
static jvalue b_get_d(jobject self, const jvalue *a) { const char *t = b_text(self, a[0].l); jvalue v; v.d = t ? strtod(t, NULL) : 0; return v; }

// Intent: its action (field 0), data Uri (field 1), flags (field 2) and extras (a map)
static jvalue intent_new(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_map("android/content/Intent")); }
static jvalue intent_new_action(jobject self, const jvalue *a) {
    jobj *i = (jobj *)intent_new(self, a).l;
    i->field[0].l = a[0].l;
    return L((jobject)i);
}
static jvalue intent_self(jobject self, const jvalue *a) { (void)a; return L(self); }
static jvalue intent_set_action(jobject self, const jvalue *a) { jobj *i = map_of(self); if (i) i->field[0].l = a[0].l; return L(self); }
static jvalue intent_action(jobject self, const jvalue *a) { (void)a; jobj *i = map_of(self); return L(i ? i->field[0].l : NULL); }
static jvalue intent_set_data(jobject self, const jvalue *a) { jobj *i = map_of(self); if (i) i->field[1].l = a[0].l; return L(self); }
static jvalue intent_data(jobject self, const jvalue *a) { (void)a; jobj *i = map_of(self); return L(i ? i->field[1].l : NULL); }
static jvalue intent_flags(jobject self, const jvalue *a) { jobj *i = map_of(self); if (i) i->field[2].i |= a[0].i; return L(self); }
static jvalue intent_get_flags(jobject self, const jvalue *a) { (void)a; jobj *i = map_of(self); return I(i ? i->field[2].i : 0); }
static jvalue i_put_obj(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, a[1].l); return L(self); }
static jvalue i_put_i(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('I', a[1])); return L(self); }
static jvalue i_put_j(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('J', a[1])); return L(self); }
static jvalue i_put_z(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('Z', a[1])); return L(self); }
static jvalue i_put_f(jobject self, const jvalue *a) { map_set(map_of(self), a[0].l, box('F', a[1])); return L(self); }
static jvalue i_extras(jobject self, const jvalue *a) {
    (void)a;
    jobj *i = map_of(self);
    if (!i || !i->len) return L(NULL); // no extras: null, as Android answers
    jvalue from = L(self);
    return bundle_copy(NULL, &from);
}

// Uri: its text, and the parts a plugin asks for (scheme://host/path?query)
static jvalue uri_parse(jobject self, const jvalue *a) {
    (void)self;
    const char *t = a[0].l ? cstr(a[0].l) : NULL;
    if (!t) return L(NULL);
    jobj *u = new_obj("android/net/Uri");
    u->buf = strdup(t);
    return L((jobject)u);
}
static const char *uri_text(jobject self) { jobj *u = as_obj(self); return u && u->buf ? u->buf : ""; }
static jvalue uri_to_string(jobject self, const jvalue *a) { (void)a; return L(jstr(uri_text(self))); }
static jvalue uri_scheme(jobject self, const jvalue *a) {
    (void)a;
    const char *t = uri_text(self), *c = strstr(t, "://");
    if (!c) c = strchr(t, ':');
    if (!c) return L(NULL);
    char s[64];
    snprintf(s, sizeof(s), "%.*s", (int)(c - t), t);
    return L(jstr(s));
}
static jvalue uri_host(jobject self, const jvalue *a) {
    (void)a;
    const char *t = uri_text(self), *c = strstr(t, "://");
    if (!c) return L(NULL);
    c += 3;
    char h[256];
    snprintf(h, sizeof(h), "%.*s", (int)strcspn(c, "/?#:"), c);
    return L(jstr(h));
}
static jvalue uri_path(jobject self, const jvalue *a) {
    (void)a;
    const char *t = uri_text(self), *c = strstr(t, "://");
    c = c ? strchr(c + 3, '/') : t;
    if (!c) return L(jstr(""));
    char path[512];
    snprintf(path, sizeof(path), "%.*s", (int)strcspn(c, "?#"), c);
    return L(jstr(path));
}
static jvalue uri_query_param(jobject self, const jvalue *a) {
    const char *t = uri_text(self), *q = strchr(t, '?'), *k = a[0].l ? cstr(a[0].l) : NULL;
    if (!q || !k) return L(NULL);
    size_t kl = strlen(k);
    for (const char *p = q + 1; *p;) {
        size_t n = strcspn(p, "&#");
        if (n > kl && !strncmp(p, k, kl) && p[kl] == '=') {
            char v[512];
            snprintf(v, sizeof(v), "%.*s", (int)(n - kl - 1), p + kl + 1);
            return L(jstr(v));
        }
        p += n;
        if (*p != '&') break;
        p++;
    }
    return L(NULL);
}


// ---- the plugin library, part 2: Firebase's C++ glue, Unity's asset packs -----------------
#define GMS_TASK_CLASS "com/google/android/gms/tasks/Task"
// Firebase's C++ SDK carries its own Java helpers (unpacked at run time as app_resources_lib.jar)
// and waits on them: CppThreadDispatcher runs a C++ function on a thread for it, and a
// JniResultCallback reports a Java Task's result. Ours run the function, and report every
// Task as "not available" (there's no Google service here), so Firebase's waits end.
typedef struct {
    int kind; // 0: nativeFunction(fn, data); 1: nativeOnResult (a Task failed); 2: (it succeeded)
    jobject self;
    jlong fn, data;
    jobject result; // kind 2: the Task's result
} fb_job;

static void *fb_job_run(void *arg) {
    fb_job *j = arg;
    usleep(2000); // after the call that made it has returned
    if (j->kind == 0) {
        typedef void (*native_fn)(void *env, jclass cls, jlong fn, jlong data);
        native_fn f = (native_fn)jni_find_native("nativeFunction");
        if (f) f(arm_jni_env(), find_class("com/google/firebase/app/internal/cpp/CppThreadDispatcherContext"), j->fn, j->data);
    } else {
        typedef void (*result_fn)(void *env, jobject self, jobject result, jboolean ok, jboolean cancelled,
                                  jobject status, jlong fn, jlong data);
        result_fn f = (result_fn)jni_find_native("nativeOnResult");
        if (f && j->kind == 2) f(arm_jni_env(), j->self, j->result, JNI_TRUE, JNI_FALSE, NULL, j->fn, j->data);
        else if (f) f(arm_jni_env(), j->self, NULL, JNI_FALSE, JNI_FALSE, jstr("Google Play services are not available"), j->fn, j->data);
    }
    free(j);
    return NULL;
}
static void fb_start(int kind, jobject self, jlong fn, jlong data, jobject result) {
    fb_job *j = malloc(sizeof(*j));
    if (!j) return;
    *j = (fb_job){ kind, self, fn, data, result };
    pthread_t t;
    if (pthread_create(&t, NULL, fb_job_run, j) == 0) pthread_detach(t);
    else free(j);
}

// Firebase Auth's listeners (JniAuthStateListener / JniIdTokenListener, C++ data in their long):
// on a phone FirebaseAuth calls a listener once as soon as it's added, with the state now
// (nobody signed in here) - the C++ side waits for that first call. Jetpack Joyride Classic's
// Halfbrick+ check (its title menu behind "processing"); other games as before (none called).
static int fb_auth_listeners_on(void) {
    return java_package && !strncmp(java_package, "com.halfbrick.jetpack", 21);
}
static jvalue fb_listener_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *l = new_obj("com/google/firebase/auth/internal/cpp/JniAuthStateListener");
    l->field[0].j = a[0].j;
    return L((jobject)l);
}
static jvalue fb_token_listener_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *l = new_obj("com/google/firebase/auth/internal/cpp/JniIdTokenListener");
    l->field[0].j = a[0].j;
    return L((jobject)l);
}
typedef struct { jobject listener; jlong data; const char *native; } fb_listen;
static void *fb_listen_run(void *arg) {
    fb_listen *l = arg;
    usleep(2000); // after addAuthStateListener has returned
    typedef void (*changed_fn)(void *env, jobject self, jlong data);
    changed_fn f = (changed_fn)jni_find_native(l->native);
    if (f) f(arm_jni_env(), l->listener, l->data);
    droid_log("java: Firebase Auth: %s(nobody signed in) %s", l->native, f ? "called" : "not registered");
    free(l);
    return NULL;
}
static void fb_listen_start(jobject listener, const char *native) {
    jobj *o = as_obj(listener);
    if (!fb_auth_listeners_on() || !o) return;
    fb_listen *l = malloc(sizeof(*l));
    if (!l) return;
    *l = (fb_listen){ listener, o->field[0].j, native };
    pthread_t t;
    if (pthread_create(&t, NULL, fb_listen_run, l) == 0) pthread_detach(t);
    else free(l);
}
static jvalue fb_add_auth_listener(jobject self, const jvalue *a) {
    (void)self;
    fb_listen_start(a[0].l, "nativeOnAuthStateChanged");
    return V();
}
static jvalue fb_add_token_listener(jobject self, const jvalue *a) {
    (void)self;
    fb_listen_start(a[0].l, "nativeOnIdTokenChanged");
    return V();
}

static jvalue fb_ctx_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *c = new_obj("com/google/firebase/app/internal/cpp/CppThreadDispatcherContext");
    c->field[0].j = a[0].j; // the function
    c->field[1].j = a[1].j; // its data
    c->field[2].j = a[2].j; // the cancel function
    return L((jobject)c);
}
static void fb_dispatch(jobject ctx) {
    jobj *c = as_obj(ctx);
    if (!c || c->field[3].i++) return; // once
    static int told;
    if (told++ < 8) droid_log("java: Firebase: runs its C++ function %#llx", (unsigned long long)c->field[0].j);
    fb_start(0, NULL, c->field[0].j, c->field[1].j, NULL);
}
static jvalue fb_run_main(jobject self, const jvalue *a) { (void)self; fb_dispatch(a[1].l); return V(); }
static jvalue fb_run_background(jobject self, const jvalue *a) { (void)self; fb_dispatch(a[0].l); return V(); }

static jvalue fb_result_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *r = new_obj("com/google/firebase/app/internal/cpp/JniResultCallback");
    jobj *task = as_obj(a[0].l);
    if (task && task->field[0].i == 1) { // one of ours that finishes well (Remote Config below)
        fb_start(2, (jobject)r, a[1].j, a[2].j, task->field[1].l);
        return L((jobject)r);
    }
    static int told;
    if (told++ < 8) droid_log("java: Firebase: a Task's result asked for: \"not available\"");
    fb_start(1, (jobject)r, a[1].j, a[2].j, NULL);
    return L((jobject)r);
}

// Firebase Remote Config (com.google.firebase.remoteconfig): the game's settings from Google's
// servers. None here: as a phone offline - its own defaults (setDefaultsAsync), the fetch
// fails, the other Tasks finish well at once. With no
// answer the C++ side never got an instance: Fruit Ninja waited on "remote config fetch pending".
#define FRC "com/google/firebase/remoteconfig/"
static jobj *rc_self(void) {
    static jobj *rc;
    if (!rc) {
        rc = new_obj(FRC "FirebaseRemoteConfig");
        rc->field[0].l = (jobject)new_map("java/util/HashMap"); // the defaults
        droid_log("java: Firebase Remote Config: the game's own defaults (no server here)");
    }
    return rc;
}
static jobject rc_task(jobject result) {
    jobj *t = new_obj(GMS_TASK_CLASS);
    t->field[0].i = 1; // finishes well
    t->field[1].l = result;
    return (jobject)t;
}
// FirebaseAnalytics.getAppInstanceId(): on a phone a Task that finishes well even offline (the id
// is made on the device). Ours failed like every Task: Temple Run 2's FirebaseInit then never called
// the game back (task.Exception set: return) - its loading picture forever. Imangi's games: an id
// made once and kept (prefs/FirebaseAnalytics.txt); every other game as before (the Task fails).
static jvalue fa_app_instance_id(jobject self, const jvalue *a) {
    (void)self; (void)a;
    if (!java_package || strncmp(java_package, "com.imangi.", 11) != 0) return L((jobject)new_obj(GMS_TASK_CLASS));
    prefs_store *st = prefs_open("FirebaseAnalytics");
    char id[40];
    pthread_mutex_lock(&prefs_lock);
    const pref *p = prefs_find(st, "app_instance_id");
    if (p) {
        snprintf(id, sizeof(id), "%s", p->value);
    } else {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        uint64_t x = (uint64_t)t.tv_sec * 2654435761u ^ (uint64_t)t.tv_nsec ^ 0x5bd1e9955bd1e995ull;
        for (int i = 0; i < 32; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            id[i] = "0123456789abcdef"[x & 15];
        }
        id[32] = 0;
        prefs_put_raw(st, 's', "app_instance_id", id);
        prefs_save(st);
    }
    pthread_mutex_unlock(&prefs_lock);
    droid_log("java: FirebaseAnalytics.getAppInstanceId: %s (finished well)", id);
    return L(rc_task(jstr(id)));
}
static jvalue rc_instance(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)rc_self()); }
static jvalue rc_task_void(jobject self, const jvalue *a) { (void)self; (void)a; return L(rc_task(NULL)); }
// fetch: fails as a phone offline (the game takes its offline path); a "fetched" empty config
// read every key as "" - Fruit Ninja's bool.Parse("") threw, a black screen
static jvalue rc_task_fail(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_obj(GMS_TASK_CLASS)); }
// the games firebase_rc.c fetches for: fetch finishes well once its fetch is in (waits up to 10 s);
// activate / fetchAndActivate say whether the values changed (a Boolean)
static jvalue rc_fetch(jobject self, const jvalue *a) {
    if (!frc_enabled()) return rc_task_fail(self, a);
    return frc_wait(10000) ? L(rc_task(NULL)) : rc_task_fail(self, a);
}
static jvalue rc_bool_task(int z) {
    jvalue b;
    b.j = 0;
    b.z = z ? JNI_TRUE : JNI_FALSE;
    return L(rc_task(box('Z', b)));
}
static jvalue rc_fetch_activate(jobject self, const jvalue *a) {
    if (!frc_enabled()) return rc_task_fail(self, a);
    if (!frc_wait(10000)) return rc_task_fail(self, a);
    frc_activate();
    return rc_bool_task(1);
}
static jvalue rc_defaults_res(jobject self, const jvalue *a) { // setDefaultsAsync(R.xml.<defaults>): not read yet
    (void)self;
    droid_log("java: Remote Config: defaults from resource %#x (not read yet)", a[0].i);
    return L(rc_task(NULL));
}
static jvalue rc_task_false(jobject self, const jvalue *a) {
    (void)self; (void)a;
    if (frc_enabled()) return rc_bool_task(frc_activate());
    jvalue no;
    no.j = 0;
    no.z = JNI_FALSE;
    return L(rc_task(box('Z', no)));
}
static jvalue rc_info(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_obj(FRC "FirebaseRemoteConfigInfo")); }
static jvalue rc_task_info(jobject self, const jvalue *a) { return L(rc_task(rc_info(self, a).l)); }
static jvalue rc_set_defaults(jobject self, const jvalue *a) {
    (void)self;
    jobj *m = map_of(a[0].l), *d = map_of(rc_self()->field[0].l);
    for (size_t i = 0; m && i < m->len; i++) map_set(d, ((kv_pair *)m->buf)[i].k, ((kv_pair *)m->buf)[i].v);
    droid_log("java: Firebase Remote Config: %zu default(s)", d ? d->len : 0);
    return L(rc_task(NULL));
}
// a value: Google's (firebase_rc.c, the games it fetches for: source REMOTE 2), else the game's
// default (DEFAULT 1), else none (STATIC 0)
static jobject rc_value_of(jobject key) {
    jobj *v = new_obj(FRC "FirebaseRemoteConfigValue");
    const char *remote = frc_value(java_text(key));
    if (remote) {
        v->field[0].l = jstr(remote);
        v->field[1].i = 2;
        return (jobject)v;
    }
    kv_pair *e = map_find(map_of(rc_self()->field[0].l), key);
    v->field[0].l = e ? e->v : NULL;
    v->field[1].i = e != NULL;
    return (jobject)v;
}
static const char *rc_text(jobject self) { // a value's text: a String, or a box ("1" for true)
    jobj *v = as_obj(self);
    jobject d = v ? v->field[0].l : NULL;
    if (!d) return "";
    jobj *b = as_obj(d);
    if (b && b->cls && !strcmp(b->cls, "java/lang/Boolean")) // Android's text: "true" / "false" (not 1 / 0:
        return box_text(d)[0] == '1' ? "true" : "false"; // C#'s bool.Parse failed on it, Fruit Ninja)
    return b ? box_text(d) : java_text(d);
}
static jvalue rc_get_value(jobject self, const jvalue *a) {
    (void)self;
    jobject v = rc_value_of(a[0].l);
    static int told;
    if (told++ < 80) droid_log("java: Remote Config: %s = \"%s\"", java_text(a[0].l), rc_text(v));
    return L(v);
}
static jvalue rc_v_string(jobject self, const jvalue *a) { (void)a; return L(jstr(rc_text(self))); }
static jvalue rc_v_bool(jobject self, const jvalue *a) {
    (void)a;
    const char *t = rc_text(self);
    return Z(!strcmp(t, "1") || !strcasecmp(t, "true") || !strcasecmp(t, "yes") || !strcasecmp(t, "on"));
}
static jvalue rc_v_long(jobject self, const jvalue *a) { (void)a; return J(strtoll(rc_text(self), NULL, 10)); }
static jvalue rc_v_double(jobject self, const jvalue *a) {
    (void)a;
    jvalue v;
    v.d = strtod(rc_text(self), NULL);
    return v;
}
static jvalue rc_v_bytes(jobject self, const jvalue *a) {
    (void)a;
    const char *t = rc_text(self);
    jsize n = (jsize)strlen(t);
    jbyteArray b = jni->NewByteArray(&jni, n);
    if (b && n) jni->SetByteArrayRegion(&jni, b, 0, n, (const jbyte *)t);
    return L(b);
}
static jvalue rc_v_source(jobject self, const jvalue *a) { // VALUE_SOURCE_REMOTE 2, DEFAULT 1, STATIC 0
    (void)a;
    jobj *v = as_obj(self);
    return I(v ? v->field[1].i : 0);
}
static jvalue rc_get_string(jobject self, const jvalue *a) { return rc_v_string(rc_value_of(a[0].l), NULL); (void)self; }
static jvalue rc_get_bool(jobject self, const jvalue *a) { return rc_v_bool(rc_value_of(a[0].l), NULL); (void)self; }
static jvalue rc_get_long(jobject self, const jvalue *a) { return rc_v_long(rc_value_of(a[0].l), NULL); (void)self; }
static jvalue rc_get_double(jobject self, const jvalue *a) { return rc_v_double(rc_value_of(a[0].l), NULL); (void)self; }
static jvalue rc_get_all(jobject self, const jvalue *a) {
    (void)self; (void)a;
    jobj *d = map_of(rc_self()->field[0].l), *all = new_map("java/util/HashMap");
    for (size_t i = 0; d && i < d->len; i++) {
        jobject k = ((kv_pair *)d->buf)[i].k;
        map_set(all, k, rc_value_of(k));
    }
    for (int i = 0; i < frc_count(); i++) { // (Google's keys the defaults don't have)
        jobject k = jstr(frc_key(i));
        if (!map_find(all, k)) map_set(all, k, rc_value_of(k));
    }
    return L((jobject)all);
}
static jvalue rc_keys(jobject self, const jvalue *a) { // getKeysByPrefix: the defaults' keys
    (void)self;
    const char *prefix = a[0].l ? java_text(a[0].l) : "";
    jobj *d = map_of(rc_self()->field[0].l), *keys = new_map("java/util/HashMap");
    for (size_t i = 0; d && i < d->len; i++) {
        jobject k = ((kv_pair *)d->buf)[i].k;
        if (!strncmp(java_text(k), prefix, strlen(prefix))) map_set(keys, k, k);
    }
    for (int i = 0; i < frc_count(); i++)
        if (!strncmp(frc_key(i), prefix, strlen(prefix))) {
            jobject k = jstr(frc_key(i));
            if (!map_find(keys, k)) map_set(keys, k, k);
        }
    jvalue none;
    none.j = 0;
    return m_key_set((jobject)keys, &none);
}
static jvalue rc_fetch_status(jobject self, const jvalue *a) { (void)self; (void)a; return I(-1); } // SUCCESS
static jvalue rc_fetch_time(jobject self, const jvalue *a) { (void)self; (void)a; return J(0); }
static jvalue rc_settings(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_obj(FRC "FirebaseRemoteConfigSettings")); }
static jvalue rc_timeout(jobject self, const jvalue *a) { (void)self; (void)a; return J(60); }
static jvalue rc_interval(jobject self, const jvalue *a) { (void)self; (void)a; return J(43200); }
static jvalue rc_builder(jobject self, const jvalue *a) { (void)a; return L(self); }
static jvalue rc_registration(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L((jobject)new_obj(FRC "ConfigUpdateListenerRegistration"));
}


// ---- the plugin library, part 3: device-info helpers (Nekki's SystemInfo) -----------------
// Shadow Fight 2 asks its own Java helper about the device (Build fields, storage paths,
// installed apps, GL strings). The answers are what Android would say on this device: a
// PlayStation 5 by Sony, no other apps installed, its paths. (Null answers: a C#
// NullReferenceException right after its server check, and the game stopped there.)
static jvalue si_text(const char *t) { return L(jstr(t)); }
static jvalue si_board(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("ps5"); }
static jvalue si_bootloader(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("unknown"); }
static jvalue si_brand(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("PlayStation"); }
static jvalue si_device(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("ps5"); }
static jvalue si_fingerprint(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("sony/ps5/ps5:11/RP1A/1:user/release-keys"); }
static jvalue si_model(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("PlayStation 5"); }
static jvalue si_maker(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("Sony"); }
static jvalue si_data_path(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("/data/"); }
static jvalue si_ext_path(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("/storage/emulated/0/"); }
static jvalue si_gl_vendor(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("Sony"); }
static jvalue si_gl_renderer(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("PlayStation 5 GPU"); }
static jvalue si_gl_version(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("OpenGL ES 3.2"); }
static jvalue si_gl_sl(jobject s, const jvalue *a) { (void)s; (void)a; return si_text("OpenGL ES GLSL ES 3.20"); }
static jvalue si_prop_default(jobject s, const jvalue *a) { (void)s; return L(a[1].l); } // getProp(name, default)


// ---- the plugin library, part 4: Google's Tasks, and small device answers ----------------
// A Task (Play Games, Billing, Firebase's Java side, Play Core...) is a result to come; the
// plugin adds listeners and waits for them. No Google services here: every Task finishes at
// once as failed ("not available"), its listeners called on the main thread like Android's.
#define GMS_TASK "com/google/android/gms/tasks/Task"
static jobject task_error(void) {
    static jobject e;
    if (!e) {
        jvalue m = L(jstr("Google Play services are not available on this device"));
        e = exc_new(NULL, &m).l;
    }
    return e;
}
static jvalue task_on_success(jobject self, const jvalue *a) { (void)a; return L(self); } // never succeeds
static jvalue task_on_failure(jobject self, const jvalue *a) {
    looper_post_call(main_looper(), a[0].l, "onFailure", task_error(), 1000000);
    return L(self);
}
static jvalue task_on_failure2(jobject self, const jvalue *a) { // (Activity / Executor, listener)
    looper_post_call(main_looper(), a[1].l, "onFailure", task_error(), 1000000);
    return L(self);
}
// Google Play Games (the plugin's HelperFragment.fetchToken): a PendingResult whose callback
// hears at once "sign-in required" (4), so the game goes on signed out (Vector waited on
// "connecting": null was a NullReferenceException and the callback never came)
static jvalue pgs_fetch_token(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_log("java: Play Games sign-in: not on this console (sign-in required)");
    return L((jobject)new_obj("com/google/android/gms/common/api/PendingResult"));
}
static jvalue pgs_set_callback(jobject self, const jvalue *a) {
    (void)self;
    looper_post_call(main_looper(), a[0].l, "onResult", (jobject)new_obj("com/google/games/bridge/TokenResult"), 1000000);
    return V();
}
// Play Games v2 (PlayGames.getGamesSignInClient(activity)): a client whose isAuthenticated /
// signIn / requestServerSideAccess are Tasks that fail at once (signed out). Null was Angry
// Birds 2's NullReferenceException in Rovio's BeaconAuthentication ("Oops! Something went wrong!")
static jvalue pgs_client(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_log("java: Play Games v2 sign-in client: signed out (no Play services)");
    return L((jobject)new_obj("com/google/android/gms/games/GamesSignInClient"));
}
static jvalue pgs_task(jobject self, const jvalue *a) { (void)self; (void)a; return L((jobject)new_obj(GMS_TASK)); }
static jvalue pgs_status(jobject self, const jvalue *a) { (void)self; (void)a; return I(4); } // SIGN_IN_REQUIRED
static jvalue pgs_null_text(jobject self, const jvalue *a) { (void)self; (void)a; return L(NULL); }

// UMP's getPrivacyOptionsRequirementStatus: an enum, NOT_REQUIRED (1); null was a
// NullReferenceException that stopped Vector's ads step
static jvalue ump_privacy(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L((jobject)new_obj("com/google/android/ump/ConsentInformation$PrivacyOptionsRequirementStatus"));
}
static jvalue ump_privacy_ordinal(jobject self, const jvalue *a) { (void)self; (void)a; return I(1); }
static jvalue ump_privacy_name(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("NOT_REQUIRED")); }

// Billing: no store here, said at once ("billing unavailable", 3) - a game waits for the
// answer (Vector: "loading..." for ever on Nekki's BillingHelper.Connect)
static jvalue nekki_billing_connect(jobject self, const jvalue *a) { // IConnectingCallback.onResult(int, Boolean)
    (void)self;
    jvalue code, no;
    code.j = 0;
    code.i = 3;
    no.j = 0;
    no.z = JNI_FALSE;
    jobject args[2] = { box('I', code), box('Z', no) };
    droid_log("java: Nekki billing connect: no store (3)");
    if (a[0].l) proxy_call(a[0].l, "onResult", 2, args);
    return V();
}
// Beacon's DeviceInfo.getAsyncIdentifiers(advertising, AsyncIdsListener): Java looks the ids up
// on a thread of its own and calls onComplete(adId, trackingEnabled, userAgent, appSetId); with
// no answer Angry Birds 2 stayed on its loading screen. Ad tracking off, as a phone that opted out.
static jvalue beacon_async_ids(jobject self, const jvalue *a) {
    (void)self;
    jvalue no;
    no.j = 0;
    no.z = JNI_FALSE;
    jobject args[4] = { jstr("00000000-0000-0000-0000-000000000000"), box('Z', no),
                        jstr("Mozilla/5.0 (Linux; Android 11; PlayStation 5) AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Version/4.0 Chrome/120.0.0.0 Mobile Safari/537.36"),
                        jstr("") };
    droid_log("java: Beacon async ids: answered (ad tracking off)");
    if (a[1].l) proxy_call(a[1].l, "onComplete", 4, args);
    return V();
}
// Facebook's Unity SDK: FB.Init(params) ends in FB.OnInitialized -> UnitySendMessage(
// "UnityFacebookSDKPlugin", "OnInitComplete", {key_hash}) (read with Apktool); the C# side waits
// for it (Angry Birds 2: "Starting up..." for ever). Initialized, nobody logged in.
static jvalue fb_init(jobject self, const jvalue *a) {
    (void)self; (void)a;
    droid_log("java: Facebook SDK init: done (no login)");
    unity_send_message("UnityFacebookSDKPlugin", "OnInitComplete", "{\"key_hash\":\"\"}");
    return V();
}
static jvalue fb_sdk_version(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("18.0.0")); }
static jvalue beacon_locale(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en_US")); }
static jvalue beacon_empty(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("")); }

static jvalue billing_start(jobject self, const jvalue *a) { // BillingClientStateListener.onBillingSetupFinished
    (void)self;
    droid_log("java: BillingClient.startConnection: no store (3)");
    looper_post_call(main_looper(), a[0].l, "onBillingSetupFinished",
                     (jobject)new_obj("com/android/billingclient/api/BillingResult"), 1000000);
    return V();
}
static jvalue billing_code(jobject self, const jvalue *a) { (void)self; (void)a; return I(3); } // BILLING_UNAVAILABLE
static jvalue billing_message(jobject self, const jvalue *a) {
    (void)self; (void)a;
    return L(jstr("Billing is not available on this device"));
}

static jvalue task_on_complete(jobject self, const jvalue *a) {
    looper_post_call(main_looper(), a[0].l, "onComplete", self, 1000000);
    return L(self);
}
static jvalue task_on_complete2(jobject self, const jvalue *a) {
    looper_post_call(main_looper(), a[1].l, "onComplete", self, 1000000);
    return L(self);
}
static jvalue task_exception(jobject self, const jvalue *a) { (void)self; (void)a; return L(task_error()); }

static jvalue dev_chipset(jobject s, const jvalue *a) { (void)s; (void)a; return L(jstr("AMD Oberon")); }
static jvalue dev_maker(jobject s, const jvalue *a) { (void)s; (void)a; return L(jstr("Sony")); }
static jvalue disk_free_mb(jobject s, const jvalue *a) { (void)s; (void)a; return I(32 * 1024); } // 32 GB free


// ---- the plugin library: Nekki's IdHelper (Vector, Shadow Fight 3 / 4, Shades) -------------
// GetFullID is the phone's Android ID (Android 8+: Settings.Secure android_id). Answered null,
// Vector's C# threw "Unsupported platform" and stayed on its background.
static jvalue nekki_model(jobject s, const jvalue *a) { (void)s; (void)a; return L(jstr("Sony PlayStation 5")); }
static jvalue nekki_serial(jobject s, const jvalue *a) { (void)s; (void)a; return L(jstr("unknown")); }
static jvalue nekki_mac(jobject s, const jvalue *a) { (void)s; (void)a; return L(jstr("unavailable")); }


// ---- the plugin library: Nekki's activity and secure store (Shadow Fight 3) ---------------
// GetLibraryFolder: the game's native libraries' folder (null: C#'s Path.Combine(null, ...)
// threw, the game showed "unrecoverable error, restart" again and again).
static jvalue nekki_lib_folder(jobject s, const jvalue *a) {
    (void)s; (void)a;
    extern const char *java_lib_dir; // jni_impl.c
    return L(jstr(java_lib_dir));
}
// KeyChainSecureHelper: Android's keystore-encrypted values, by key and alias. Ours: kept as
// they are in the game's folder (keychain.txt, hex), so what the game writes it reads back.
#define KC_MAX 256
static struct { char *key; char *value; } kc[KC_MAX];
static int kc_count = -1;
static pthread_mutex_t kc_lock = PTHREAD_MUTEX_INITIALIZER;
static void kc_hex(const char *in, char *out) {
    static const char d[] = "0123456789abcdef";
    for (; *in; in++) *out++ = d[(unsigned char)*in >> 4], *out++ = d[(unsigned char)*in & 15];
    *out = 0;
}
static char *kc_unhex(const char *in) {
    size_t n = strlen(in) / 2;
    char *o = malloc(n + 1);
    for (size_t i = 0; o && i < n; i++) {
        unsigned v = 0;
        sscanf(in + i * 2, "%2x", &v);
        o[i] = (char)v;
    }
    if (o) o[n] = 0;
    return o;
}
static void kc_load(void) {
    if (kc_count >= 0) return;
    kc_count = 0;
    char path[300];
    FILE *f = fopen(data_path(path, sizeof(path), "keychain.txt"), "r");
    if (!f) return;
    static char line[65536];
    while (fgets(line, sizeof(line), f) && kc_count < KC_MAX) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        tab[1 + strcspn(tab + 1, "\r\n")] = 0;
        kc[kc_count].key = kc_unhex(line);
        kc[kc_count].value = kc_unhex(tab + 1);
        kc_count++;
    }
    fclose(f);
}
static void kc_save(void) {
    char path[300];
    FILE *f = fopen(data_path(path, sizeof(path), "keychain.txt"), "w");
    if (!f) return;
    for (int i = 0; i < kc_count; i++) {
        char *a = malloc(strlen(kc[i].key) * 2 + 1), *b = malloc(strlen(kc[i].value) * 2 + 1);
        if (a && b) {
            kc_hex(kc[i].key, a);
            kc_hex(kc[i].value, b);
            fprintf(f, "%s\t%s\n", a, b);
        }
        free(a);
        free(b);
    }
    fclose(f);
}
static char *kc_name(const jvalue *a) { // its key: both names the game gives
    const char *x = cstr(a[0].l), *y = cstr(a[1].l);
    size_t n = strlen(x ? x : "") + strlen(y ? y : "") + 2;
    char *k = malloc(n);
    if (k) snprintf(k, n, "%s|%s", x ? x : "", y ? y : "");
    return k;
}
static jvalue kc_read(jobject s, const jvalue *a) { // (String, String) -> String or null
    (void)s;
    char *k = kc_name(a);
    jobject r = NULL;
    pthread_mutex_lock(&kc_lock);
    kc_load();
    for (int i = 0; k && i < kc_count && !r; i++)
        if (!strcmp(kc[i].key, k)) r = jstr(kc[i].value);
    pthread_mutex_unlock(&kc_lock);
    free(k);
    return L(r);
}
static jvalue kc_write(jobject s, const jvalue *a) { // (String, String, String value)
    (void)s;
    char *k = kc_name(a);
    const char *v = cstr(a[2].l);
    if (!k) return L(NULL);
    pthread_mutex_lock(&kc_lock);
    kc_load();
    int i = 0;
    while (i < kc_count && strcmp(kc[i].key, k)) i++;
    if (i == kc_count && kc_count < KC_MAX) kc[kc_count++].key = strdup(k);
    if (i < kc_count) {
        free(kc[i].value);
        kc[i].value = strdup(v ? v : "");
        kc_save();
    }
    pthread_mutex_unlock(&kc_lock);
    free(k);
    return L(NULL);
}


// ---- the on-screen keyboard (UnityPlayer.showSoftInput): for now, a name typed for you -----
// A game asks for text (a player name...): Android's keyboard would show, then report the
// text (nativeSetInputString) and close (nativeSoftInputClosed). Ours answers at once with
// the text the game gave, or "PS5Player" and 4 digits - Shadow Fight 3's name field
// waited for a keyboard that never came. (Next: the PS5's own keyboard, sceImeDialog.)
static void *soft_input_run(void *arg) {
    char *text = arg;
    usleep(300000); // after the game has opened its field
    typedef void (*set_fn)(void *env, jobject self, jobject s);
    typedef void (*closed_fn)(void *env, jobject self);
    set_fn set = (set_fn)jni_find_native("nativeSetInputString");
    closed_fn closed = (closed_fn)jni_find_native("nativeSoftInputClosed");
    jobject player = find_class("com/unity3d/player/UnityPlayer");
    if (set) set(arm_jni_env(), player, jstr(text));
    droid_log("keyboard: \"%s\" given to the game%s", text, set ? "" : " (no nativeSetInputString)");
    if (closed) closed(arm_jni_env(), player);
    free(text);
    return NULL;
}
static jvalue soft_input(jobject self, const jvalue *a) { // (String text, int type, ... String placeholder, int limit, ...)
    (void)self;
    const char *given = cstr(a[0].l);
    char name[32];
    if (given && given[0]) snprintf(name, sizeof(name), "%.31s", given);
    else snprintf(name, sizeof(name), "PS5Player%04d", (int)(now_ns() / 1000 % 10000));
    int limit = a[7].i;
    if (limit > 0 && limit < (int)sizeof(name)) name[limit] = 0;
    pthread_t t;
    char *copy = strdup(name);
    if (copy && pthread_create(&t, NULL, soft_input_run, copy) == 0) pthread_detach(t);
    else free(copy);
    return L(NULL);
}

// King's engine (further down, by per_game)
static jvalue king_anr_new(jobject self, const jvalue *a);
static jvalue king_anr_active(jobject self, const jvalue *a);
static jvalue king_anr_schedule(jobject self, const jvalue *a);
static jvalue king_cache_dir(jobject self, const jvalue *a);
static jvalue king_home_dir(jobject self, const jvalue *a);
static jvalue king_shared_dir(jobject self, const jvalue *a);
static jvalue king_language(jobject self, const jvalue *a);
static jvalue king_country(jobject self, const jvalue *a);
static jvalue king_version(jobject self, const jvalue *a);
static jvalue king_res_scale(jobject self, const jvalue *a);

static const jmeth methods[] = {
    // the on-screen keyboard
    { "com/unity3d/player/UnityPlayer", "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V", soft_input },
    { "com/unity3d/player/UnityPlayer", "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V", soft_input },
    // the plugin library: Nekki's activity, its secure store
    { "com/unity3d/player/UnityPlayerActivity", "GetLibraryFolder", "()Ljava/lang/String;", nekki_lib_folder },
    { "com/nekki/keychainsecure/KeyChainSecureHelper", "readDecryptedString",
      "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", kc_read },
    { "com/nekki/keychainsecure/KeyChainSecureHelper", "writeEncryptedString",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", kc_write },
    // the plugin library: Nekki's IdHelper
#define NID "com/nekki/unityplugins/IdHelperUnityApi"
    { NID, "GetFullID", "(Ljava/lang/String;)Ljava/lang/String;", device_id_answer },
    { NID, "GetAndroidID", "()Ljava/lang/String;", device_id_answer },
    { NID, "GetDeviceModel", "()Ljava/lang/String;", nekki_model },
    { NID, "GetBuildSerial", "()Ljava/lang/String;", nekki_serial },
    { NID, "GetMacAdress", "()Ljava/lang/String;", nekki_mac },
    // the plugin library, part 4 (Google's Tasks, small device answers)
#define TSK GMS_TASK
#define TL(x) "Lcom/google/android/gms/tasks/" x ";)Lcom/google/android/gms/tasks/Task;"
    { TSK, "addOnSuccessListener", "(" TL("OnSuccessListener"), task_on_success },
    { TSK, "addOnSuccessListener", "(Landroid/app/Activity;" TL("OnSuccessListener"), task_on_success },
    { TSK, "addOnSuccessListener", "(Ljava/util/concurrent/Executor;" TL("OnSuccessListener"), task_on_success },
    { TSK, "addOnFailureListener", "(" TL("OnFailureListener"), task_on_failure },
    { TSK, "addOnFailureListener", "(Landroid/app/Activity;" TL("OnFailureListener"), task_on_failure2 },
    { TSK, "addOnFailureListener", "(Ljava/util/concurrent/Executor;" TL("OnFailureListener"), task_on_failure2 },
    { TSK, "addOnCompleteListener", "(" TL("OnCompleteListener"), task_on_complete },
    { TSK, "addOnCompleteListener", "(Landroid/app/Activity;" TL("OnCompleteListener"), task_on_complete2 },
    { TSK, "addOnCompleteListener", "(Ljava/util/concurrent/Executor;" TL("OnCompleteListener"), task_on_complete2 },
    { TSK, "addOnCanceledListener", "(" TL("OnCanceledListener"), task_on_success },
#define PGC "com/google/android/gms/games/GamesSignInClient"
    { "com/google/android/gms/games/PlayGames", "getGamesSignInClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/GamesSignInClient;", pgs_client },
    { "com/google/android/gms/games/PlayGames", "getGamesSignInClient", "(Lcom.unity3d.player.UnityPlayerActivity;)Ljava/lang/Object;", pgs_client },
    { PGC, "isAuthenticated", "()Lcom/google/android/gms/tasks/Task;", pgs_task },
    { PGC, "isAuthenticated", "()Ljava/lang/Object;", pgs_task },
    { PGC, "signIn", "()Lcom/google/android/gms/tasks/Task;", pgs_task },
    { PGC, "signIn", "()Ljava/lang/Object;", pgs_task },
    { PGC, "requestServerSideAccess", "(Ljava/lang/String;Z)Lcom/google/android/gms/tasks/Task;", pgs_task },
    { TSK, "isComplete", "()Z", answer_true }, { TSK, "isSuccessful", "()Z", answer_false },
    { TSK, "isCanceled", "()Z", answer_false }, { TSK, "getResult", "()Ljava/lang/Object;", answer_null },
    { TSK, "getException", "()Ljava/lang/Exception;", task_exception },
    { "com/sybo/analytics/ChipsetUtils", "GetChipsetName", "()Ljava/lang/String;", dev_chipset },
    { "com/sybo/nativeutils/NativeUtilsPlugin", "GetManufacturer", "()Ljava/lang/String;", dev_maker },
    { "com/dikra/diskutils/DiskUtils", "availableSpace", "(Z)I", disk_free_mb },
    { "com/dikra/diskutils/DiskUtils", "totalSpace", "(Z)I", disk_free_mb },
    // the plugin library, part 3: Nekki's SystemInfo (device info, Shadow Fight 2)
#define NSI "com/nekki/utils/system/SystemInfo"
    { NSI, "BBd", "()Ljava/lang/String;", si_board }, { NSI, "BBl", "()Ljava/lang/String;", si_bootloader },
    { NSI, "BBr", "()Ljava/lang/String;", si_brand }, { NSI, "BDv", "()Ljava/lang/String;", si_device },
    { NSI, "BFp", "()Ljava/lang/String;", si_fingerprint }, { NSI, "BHd", "()Ljava/lang/String;", si_device },
    { NSI, "BHs", "()Ljava/lang/String;", si_device }, { NSI, "BMd", "()Ljava/lang/String;", si_model },
    { NSI, "BMn", "()Ljava/lang/String;", si_maker }, { NSI, "BPr", "()Ljava/lang/String;", si_device },
    { NSI, "GetIPN", "()Ljava/lang/String;", play_store },
    { NSI, "dtP", "()Ljava/lang/String;", si_data_path }, { NSI, "extP", "()Ljava/lang/String;", si_ext_path },
    { NSI, "checkBPackageName", "(Ljava/lang/String;)Ljava/lang/String;", answer_empty_string },
    { NSI, "CheckLIForPack", "(Ljava/lang/String;)Z", answer_false },
    { NSI, "getDrDt", "(Ljava/lang/String;)Ljava/lang/String;", answer_null }, // a file that isn't there
    { NSI, "getGl", "()Ljava/lang/String;", si_gl_vendor }, { NSI, "getGlV", "()Ljava/lang/String;", si_gl_renderer },
    { NSI, "getGlVer", "()Ljava/lang/String;", si_gl_version }, { NSI, "getGlSVer", "()Ljava/lang/String;", si_gl_sl },
    { NSI, "getProp", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", si_prop_default },
    // the plugin library, part 2 (Firebase's C++ glue, Unity's asset packs, Google services)
#define FBC "com/google/firebase/app/internal/cpp/"
    { FBC "CppThreadDispatcherContext", "<init>", "(JJJ)V", fb_ctx_new },
    { FBC "CppThreadDispatcherContext", "acquireExecuteCancelLock", "()Z", answer_true },
    { FBC "CppThreadDispatcherContext", "releaseExecuteCancelLock", "()V", answer_void },
    { FBC "CppThreadDispatcherContext", "cancel", "()V", answer_void },
    { FBC "CppThreadDispatcher", "runOnMainThread",
      "(Landroid/app/Activity;Lcom/google/firebase/app/internal/cpp/CppThreadDispatcherContext;)V", fb_run_main },
    { FBC "CppThreadDispatcher", "runOnBackgroundThread",
      "(Lcom/google/firebase/app/internal/cpp/CppThreadDispatcherContext;)V", fb_run_background },
    { FBC "JniResultCallback", "<init>", "(Lcom/google/android/gms/tasks/Task;JJ)V", fb_result_new },
    { FBC "JniResultCallback", "cancel", "()V", answer_void },
    // Mortar's sound out (Fruit Ninja 2.x, Jetpack Joyride)
#define MAO "com/halfbrick/mortar/MortarAudioMixerOut"
    { MAO, "GetNativeSampleRate", "()I", mortar_rate },
    { MAO, "Create", "()L" MAO ";", mortar_create },
    { MAO, "Init", "(I)I", mortar_init },
    { MAO, "WriteData", "([S)V", mortar_write },
    { "com/halfbrick/mortar/HttpClient", "<init>", "()V", mortar_http_new },
    { "com/halfbrick/mortar/HttpClient", "HttpRequest", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BJI)V",
      mortar_http_request },
    // Mortar's consent form (Google UMP): none to show, done (Jetpack Joyride Classic waits on it)
    { "com/halfbrick/mortar/ConsentManagementPlatform", "Done", "()Z", answer_true },
    { "com/halfbrick/mortar/HBSupport", "RequestHBID", "()V", mortar_request_hbid },
    { "com/google/firebase/auth/FirebaseAuth", "getCurrentUser", "()Lcom/google/firebase/auth/FirebaseUser;", fb_current_user },
    { "com/google/firebase/auth/internal/cpp/JniAuthStateListener", "<init>", "(J)V", fb_listener_new },
    { "com/google/firebase/auth/internal/cpp/JniIdTokenListener", "<init>", "(J)V", fb_token_listener_new },
    { "com/google/firebase/auth/FirebaseAuth", "addAuthStateListener", "(Lcom/google/firebase/auth/FirebaseAuth$AuthStateListener;)V",
      fb_add_auth_listener },
    { "com/google/firebase/auth/FirebaseAuth", "addIdTokenListener", "(Lcom/google/firebase/auth/FirebaseAuth$IdTokenListener;)V",
      fb_add_token_listener },
    // the notification permission (Android 13's prompt), asked every frame until it's true: handled
    { "com/halfbrick/mortar/NotificationPermission", "RequestNotificationPermission", "()Z", answer_true },
    { "com/halfbrick/mortar/GooglePlaySignIn", "SignIn", "(Landroid/app/Activity;)V", mortar_sign_in },
    { "com/halfbrick/mortar/GooglePlaySnapshotManager", "Open", "(Landroid/content/Context;Ljava/lang/String;Z)I",
      mortar_snapshot_open },
    { "com/halfbrick/mortar/GooglePlayAchievementsManager", "Load", "(Landroid/app/Activity;Z)I", mortar_achievements_load },
    { "com/halfbrick/mortar/GooglePlaySignIn", "SignInManually", "(Landroid/app/Activity;)V", mortar_sign_in_manually },
    { "com/halfbrick/mortar/KeyStore", "GetValue", "(Ljava/lang/String;)Ljava/lang/String;", mortar_ks_get },
    { "com/halfbrick/mortar/KeyStore", "SetValue", "(Ljava/lang/String;Ljava/lang/String;)Z", mortar_ks_set },
    // Firebase Remote Config: the game's own defaults, every Task finished well
    { FRC "FirebaseRemoteConfig", "getInstance", "()L" FRC "FirebaseRemoteConfig;", rc_instance },
    { FRC "FirebaseRemoteConfig", "getInstance", "(Lcom/google/firebase/FirebaseApp;)L" FRC "FirebaseRemoteConfig;", rc_instance },
#define IFS "com/imangi/imangiutilities/ImangiFileSystem"
    { IFS, "GetInternalPersistantDataPath", "()Ljava/lang/String;", ifs_data_path },
    { IFS, "GetExternalPersistantDataPath", "()Ljava/lang/String;", ifs_data_path },
    { IFS, "GetInternalFullPath", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", ifs_internal_full },
    { IFS, "GetExternalFullPath", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", ifs_external_full },
    { IFS, "SetFileProviderName", "(Ljava/lang/String;)V", ifs_set_provider },
    { IFS, "GetFileProviderName", "()Ljava/lang/String;", ifs_get_provider },
    { "com/rovio/beacon/billing/GooglePlayBillingProvider", "<init>",
      "(Lcom/rovio/beacon/billing/GooglePlayBillingListener;)V", rovio_billing_new },
    { ST_CLASS, "<init>", "(I)V", st_new },
    { ST_CLASS, "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V", st_listener },
    { ST_CLASS, "setOnFrameAvailableListener",
      "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;Landroid/os/Handler;)V", st_listener },
    { ST_CLASS, "updateTexImage", "()V", st_update },
    { ST_CLASS, "getTransformMatrix", "([F)V", st_matrix },
    { ST_CLASS, "getTimestamp", "()J", st_timestamp },
    { ST_CLASS, "release", "()V", st_nothing },
    { "android/view/Surface", "<init>", "(Landroid/graphics/SurfaceTexture;)V", surface_new },
    { "com/unity3d/player/UnityPlayer", "showVideoPlayer", "(Ljava/lang/String;IIIZII)Z", unity_show_video },
    { "com/sega/gamelib/SLGlobal", "GetAndroidID", "()Ljava/lang/String;", sega_android_id },
    { "com/sega/gamelib/SLGlobal", "GetAdvertisingID", "()Ljava/lang/String;", sega_ad_id },
    { "com/sega/gamelib/SLGlobal", "GetInternalStoragePath", "()Ljava/lang/String;", sega_storage },
    { "com/sega/gamelib/SLGlobal", "GetExternalStoragePath", "()Ljava/lang/String;", sega_storage },
    { "com/sega/gamelib/SLGlobal", "GetCurrentLocale", "()I", sega_locale },
    { "com/sega/gamelib/SLGlobal", "IsConnected", "()Z", sega_connected },
    { "hardlight/hlcore/HLUnityCore", "Unity_GetISO2CountryCode", "()Ljava/lang/String;", hl_country },
    { "hardlight/hlcore/HLUnityCore", "Unity_GetPixelsFromNativeUnitDistance", "(F)I", hl_dp_to_px },
#define HL_CMP "com/hardlight/hlcompliance/ConsentManagementPlatformGoogle"
    { HL_CMP, "Unity_CMPInitialise", "(Ljava/lang/String;Z)V", hl_cmp_init },
    { HL_CMP, "Unity_CMPRequestConsent", "(ZZ)V", hl_cmp_request },
    { HL_CMP, "Unity_CMPGetConsentStatus", "()I", hl_cmp_status },
    { HL_CMP, "Unity_CMPIsConsentFlowComplete", "()Z", hl_cmp_complete },
    { "hardlight/hlcore/HLUnityCore", "Unity_GetLanguageCode", "()Ljava/lang/String;", hl_language },
#define DIMPS "jp/co/dimps/trad/common/TRADRuntimePermissionController"
    { DIMPS, "ShowPermissionsDialog", "([Ljava/lang/String;Ljava/lang/String;)V", dimps_permissions },
    { DIMPS, "ShowPermissionsDialog", "(Ljava.lang.Object;Ljava/lang/String;)V", dimps_permissions }, // (from its C#: dots)
    { DIMPS, "ShowPermissionDialogFor11OrGreater", "(Ljava.lang.String;Ljava/lang/String;)V", dimps_permission_11 },
    { DIMPS, "ShowPermissionDialogFor11OrGreater", "(Ljava/lang/String;Ljava/lang/String;)V", dimps_permission_11 },
    { DIMPS, "GetPermissionResult", "([Ljava/lang/String;)I", dimps_zero },
    { "jp/co/dimps/safearea/SafeArea", "GetSafeInsetTop", "()I", dimps_zero },
    { "jp/co/dimps/safearea/SafeArea", "GetSafeInsetBottom", "()I", dimps_zero },
    { "com/unity3d/player/UnityPlayerActivity", "checkSelfPermission", "(Ljava/lang/String;)I", dimps_zero },
    { "com/unity3d/player/UnityPlayerActivity", "getView", "()Ljava/lang/Object;", unity_player_view },
#define GREE "net/gree/unitywebview/CWebViewPlugin"
    { GREE, "Init", "(Ljava/lang/String;ZZILjava/lang/String;I)V", gree_void },
    { GREE, "SetURLPattern", "(Ljava/lang/String;Ljava/lang/Object;Ljava/lang/Object;)Z", gree_true },
    { GREE, "LoadURL", "(Ljava/lang/String;)V", gree_load_url },
    { GREE, "EvaluateJS", "(Ljava/lang/String;)V", gree_eval_js },
    { GREE, "GetMessage", "()Ljava/lang/String;", gree_get_message },
    { GREE, "SetVisibility", "(Z)V", gree_void },
    { GREE, "SetMargins", "(IIII)V", gree_void },
    { GREE, "Destroy", "()V", gree_destroy },
#define NETEASE "com/netease/tools/Utility"
    { NETEASE, "GetFreeDiskSpace", "()J", uno_free_mb },
    { NETEASE, "GetFreeDiskSpace_Internal", "()J", uno_free_mb },
    { NETEASE, "LoadUUIDFromLocal", "()Ljava/lang/String;", uno_uuid_load },
    { NETEASE, "SaveUUIDToLocal", "(Ljava/lang/String;)V", uno_uuid_save },
    { NETEASE, "LoadAppOpenRecord", "()Ljava/lang/String;", uno_opened_load },
    { NETEASE, "SaveAppOpenRecord", "(Ljava/lang/String;)V", uno_opened_save },
    { NETEASE, "GetNativeCountryCode", "()Ljava/lang/String;", uno_country },
#define BOOM "com/sega/sonicboomandroid/plugin/"
    { BOOM "HLUtils", "GetCurrentLocale", "()I", boom_locale },
    { BOOM "HLUtils", "GetCurrentLocaleString", "()Ljava/lang/String;", boom_locale_text },
    { BOOM "HLUtils", "GetCurrentLanguageCode", "()Ljava/lang/String;", hl_language },
    { BOOM "HLUtils", "GetISOLocale_TwoDigit", "()Ljava/lang/String;", boom_country },
    { BOOM "BoomDeviceIdentifier", "GetBoomDeviceIdentifier", "()Ljava/lang/String;", boom_id_get },
    { BOOM "BoomDeviceIdentifier", "SetBoomDeviceIdentifier", "(Ljava/lang/String;)Z", boom_id_set },
    { "com/sega/gamelib/social/SLSocialInterface", "Authenticate", "()V", sega_gp_auth },
#define KC "com/king/core/"
    { KC "AppNotRespondingTaskProcessor", "<init>", "(J)V", king_anr_new },
    { KC "AppNotRespondingTaskProcessor", "setActive", "(Z)V", king_anr_active },
    { KC "AppNotRespondingTaskProcessor", "scheduleTaskOnUiThread", "()V", king_anr_schedule },
    { KC "FileSystem", "getCacheDirectory", "()Ljava/lang/String;", king_cache_dir },
    { KC "FileSystem", "getHomeDirectory", "()Ljava/lang/String;", king_home_dir },
    { KC "FileSystem", "getSharedDirectory", "()Ljava/lang/String;", king_shared_dir },
    { "com/king/abm/internal/DefaultFileSystem", "getDefaultAppDirectory", "(Landroid/app/Activity;)Ljava/lang/String;",
      king_home_dir }, // (null: its tracking files written to "/")
    { KC "DeviceLocale", "getLanguageCode", "()Ljava/lang/String;", king_language },
    { KC "DeviceLocale", "getCountryCode", "()Ljava/lang/String;", king_country },
    { KC "AndroidCallstackUtils", "getPackageVersionName", "(Landroid/content/Context;)Ljava/lang/String;", king_version },
    { "com/king/candycrushsaga/CandyCrushSagaActivity", "getDisplayResolutionScale", "()F", king_res_scale },
    { "com/sega/gamelib/social/SLSocialInterface", "IsSignedIntoGooglePlay", "()Z", sega_gp_no },
    { "com/sega/gamelib/social/SLSocialInterface", "IsSigningIntoGooglePlay", "()Z", sega_gp_no },
    { "com/sega/gamelib/identifiers/DashServerIdentifier", "GetDashServerIdentifier", "(Ljava/lang/String;)Ljava/lang/String;", dash_id_get },
    { "com/sega/gamelib/identifiers/DashServerIdentifier", "SetDashServerIdentifier", "(Ljava/lang/String;Ljava/lang/String;)Z", dash_id_set },
    { "com/rovio/beacon/billing/GooglePlayBillingProvider", "initialize", "([Ljava/lang/String;[Ljava/lang/String;)V",
      rovio_billing_init },
#define IB_ALERT_SIG "(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;IILjava/lang/String;)V"
#define IB_HTTP_SIG "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI)[B"
    { "android/app/NativeActivity", "showAlert", IB_ALERT_SIG, ib_show_alert },
    { "com/ib3port/game/GameActivity", "showAlert", IB_ALERT_SIG, ib_show_alert },
    { "com/ib2port/game/GameActivity", "showAlert", IB_ALERT_SIG, ib_show_alert },
    { "com/ib1port/game/GameActivity", "showAlert", IB_ALERT_SIG, ib_show_alert },
    { "android/app/NativeActivity", "httpRequest", IB_HTTP_SIG, ib_http },
    { "com/ib3port/game/GameActivity", "httpRequest", IB_HTTP_SIG, ib_http },
    { "com/ib2port/game/GameActivity", "httpRequest", IB_HTTP_SIG, ib_http },
    { "com/ib1port/game/GameActivity", "httpRequest", IB_HTTP_SIG, ib_http },
#define P31 "com/prime31/EtceteraPlugin"
    { P31, "instance", "()L" P31 ";", p31_instance },
    { P31, "instance", "()Ljava/lang/Object;", p31_instance },
    { P31, "showAlert", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", p31_show_alert },
    { P31, "showAlertPrompt", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
      p31_show_prompt },
    { P31, "showToast", "(Ljava/lang/String;Z)V", p31_nothing },
    { P31, "showProgressDialog", "(Ljava/lang/String;Ljava/lang/String;)V", p31_nothing },
    { P31, "hideProgressDialog", "()V", p31_nothing },
    { P31, "resetAskForReview", "()V", p31_nothing },
    { P31, "askForReview", "(IIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", p31_nothing },
    { P31, "askForReviewNow", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", p31_nothing },
    { "com/imangi/imangiutilities/ImangiXLSHelper", "Parse", "(Ljava/lang/String;)V", xls_parse },
    { "com/imangi/imangiutilities/ImangiXLSHelper", "ParseList", "(Ljava/lang/String;)V", xls_parse_list },
    { "com/google/firebase/analytics/FirebaseAnalytics", "getAppInstanceId", "()L" GMS_TASK_CLASS ";", fa_app_instance_id },
    { FRC "FirebaseRemoteConfig", "ensureInitialized", "()L" GMS_TASK_CLASS ";", rc_task_info },
    { FRC "FirebaseRemoteConfig", "activate", "()L" GMS_TASK_CLASS ";", rc_task_false },
    { FRC "FirebaseRemoteConfig", "fetchAndActivate", "()L" GMS_TASK_CLASS ";", rc_fetch_activate },
    { FRC "FirebaseRemoteConfig", "fetch", "()L" GMS_TASK_CLASS ";", rc_fetch },
    { FRC "FirebaseRemoteConfig", "fetch", "(J)L" GMS_TASK_CLASS ";", rc_fetch },
    { FRC "FirebaseRemoteConfig", "setDefaultsAsync", "(I)L" GMS_TASK_CLASS ";", rc_defaults_res },
    { FRC "FirebaseRemoteConfig", "setDefaultsAsync", "(Ljava/util/Map;)L" GMS_TASK_CLASS ";", rc_set_defaults },
    { FRC "FirebaseRemoteConfig", "setConfigSettingsAsync", "(L" FRC "FirebaseRemoteConfigSettings;)L" GMS_TASK_CLASS ";", rc_task_void },
    { FRC "FirebaseRemoteConfig", "reset", "()L" GMS_TASK_CLASS ";", rc_task_void },
    { FRC "FirebaseRemoteConfig", "getValue", "(Ljava/lang/String;)L" FRC "FirebaseRemoteConfigValue;", rc_get_value },
    { FRC "FirebaseRemoteConfig", "getString", "(Ljava/lang/String;)Ljava/lang/String;", rc_get_string },
    { FRC "FirebaseRemoteConfig", "getBoolean", "(Ljava/lang/String;)Z", rc_get_bool },
    { FRC "FirebaseRemoteConfig", "getLong", "(Ljava/lang/String;)J", rc_get_long },
    { FRC "FirebaseRemoteConfig", "getDouble", "(Ljava/lang/String;)D", rc_get_double },
    { FRC "FirebaseRemoteConfig", "getAll", "()Ljava/util/Map;", rc_get_all },
    { FRC "FirebaseRemoteConfig", "getKeysByPrefix", "(Ljava/lang/String;)Ljava/util/Set;", rc_keys },
    { FRC "FirebaseRemoteConfig", "getInfo", "()L" FRC "FirebaseRemoteConfigInfo;", rc_info },
    { FRC "FirebaseRemoteConfig", "addOnConfigUpdateListener",
      "(L" FRC "ConfigUpdateListener;)L" FRC "ConfigUpdateListenerRegistration;", rc_registration },
    { FRC "ConfigUpdateListenerRegistration", "remove", "()V", answer_void },
    { FRC "internal/cpp/JniConfigUpdateListener", "<init>", "(J)V", rc_registration },
    { FRC "FirebaseRemoteConfigValue", "asString", "()Ljava/lang/String;", rc_v_string },
    { FRC "FirebaseRemoteConfigValue", "asBoolean", "()Z", rc_v_bool },
    { FRC "FirebaseRemoteConfigValue", "asLong", "()J", rc_v_long },
    { FRC "FirebaseRemoteConfigValue", "asDouble", "()D", rc_v_double },
    { FRC "FirebaseRemoteConfigValue", "asByteArray", "()[B", rc_v_bytes },
    { FRC "FirebaseRemoteConfigValue", "getSource", "()I", rc_v_source },
    { FRC "FirebaseRemoteConfigInfo", "getLastFetchStatus", "()I", rc_fetch_status },
    { FRC "FirebaseRemoteConfigInfo", "getFetchTimeMillis", "()J", rc_fetch_time },
    { FRC "FirebaseRemoteConfigInfo", "getConfigSettings", "()L" FRC "FirebaseRemoteConfigSettings;", rc_settings },
    { FRC "FirebaseRemoteConfigSettings", "getFetchTimeoutInSeconds", "()J", rc_timeout },
    { FRC "FirebaseRemoteConfigSettings", "getMinimumFetchIntervalInSeconds", "()J", rc_interval },
    { FRC "FirebaseRemoteConfigSettings$Builder", "<init>", "()V", rc_settings },
    { FRC "FirebaseRemoteConfigSettings$Builder", "setFetchTimeoutInSeconds", "(J)L" FRC "FirebaseRemoteConfigSettings$Builder;", rc_builder },
    { FRC "FirebaseRemoteConfigSettings$Builder", "setMinimumFetchIntervalInSeconds", "(J)L" FRC "FirebaseRemoteConfigSettings$Builder;", rc_builder },
    { FRC "FirebaseRemoteConfigSettings$Builder", "build", "()L" FRC "FirebaseRemoteConfigSettings;", rc_settings },
    { FBC "GoogleApiAvailabilityHelper", "makeGooglePlayServicesAvailable", "(Landroid/app/Activity;)Z", answer_false },
    { FBC "GoogleApiAvailabilityHelper", "stopCallbacks", "()V", answer_void },
    { FBC "Log", "shutdown", "()V", answer_void },
    // Unity's Play Asset Delivery: no Play Core here; install-time packs are split APKs it reads
    { "com/unity3d/player/PlayAssetDeliveryUnityWrapper", "playCoreApiMissing", "()Z", answer_true },
    // the plugin library, part 1 (lists, maps, boxes, dates, exceptions, Bundle, Intent, Uri)
#define LST "java/util/List"
#define MAP "java/util/Map"
    { "java/util/ArrayList", "<init>", "()V", l_new }, { "java/util/ArrayList", "<init>", "(I)V", l_new },
    { "java/util/LinkedList", "<init>", "()V", l_new },
    { LST, "add", "(Ljava/lang/Object;)Z", l_add }, { LST, "add", "(ILjava/lang/Object;)V", l_add_at },
    { LST, "get", "(I)Ljava/lang/Object;", l_get }, { LST, "set", "(ILjava/lang/Object;)Ljava/lang/Object;", l_set },
    { LST, "remove", "(I)Ljava/lang/Object;", l_remove_at }, { LST, "size", "()I", l_size },
    { LST, "isEmpty", "()Z", l_is_empty }, { LST, "clear", "()V", l_clear },
    { LST, "contains", "(Ljava/lang/Object;)Z", l_contains }, { LST, "toArray", "()[Ljava/lang/Object;", l_to_array },
    { LST, "iterator", "()Ljava/util/Iterator;", l_iterator },
    { "java/util/Collection", "iterator", "()Ljava/util/Iterator;", l_iterator },
    { "java/util/Collection", "size", "()I", l_size },
    { "java/util/HashMap", "<init>", "()V", m_new }, { "java/util/HashMap", "<init>", "(I)V", m_new },
    { "java/util/LinkedHashMap", "<init>", "()V", m_new }, { "java/util/TreeMap", "<init>", "()V", m_new },
    { MAP, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", m_put },
    { MAP, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", m_get },
    { MAP, "containsKey", "(Ljava/lang/Object;)Z", m_contains },
    { MAP, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;", m_remove },
    { MAP, "size", "()I", l_size }, { MAP, "isEmpty", "()Z", l_is_empty }, { MAP, "clear", "()V", l_clear },
    { MAP, "keySet", "()Ljava/util/Set;", m_key_set }, { MAP, "values", "()Ljava/util/Collection;", m_values },
    { "java/lang/Byte", "<init>", "(B)V", box_b }, { "java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;", box_b },
    { "java/lang/Short", "<init>", "(S)V", box_s }, { "java/lang/Short", "valueOf", "(S)Ljava/lang/Short;", box_s },
    { "java/lang/Character", "<init>", "(C)V", box_c }, { "java/lang/Character", "valueOf", "(C)Ljava/lang/Character;", box_c },
    { "java/lang/Byte", "byteValue", "()B", unbox_b }, { "java/lang/Short", "shortValue", "()S", unbox_s },
    { "java/lang/Character", "charValue", "()C", unbox_c }, { "java/lang/Double", "doubleValue", "()D", unbox_d },
    { "java/lang/Number", "intValue", "()I", box_int }, { "java/lang/Number", "longValue", "()J", box_long },
    { "java/lang/Number", "doubleValue", "()D", unbox_d },
    { "java/lang/Integer", "toString", "()Ljava/lang/String;", box_to_string },
    { "java/lang/Long", "toString", "()Ljava/lang/String;", box_to_string },
    { "java/lang/Double", "toString", "()Ljava/lang/String;", box_to_string },
    { "java/lang/Float", "toString", "()Ljava/lang/String;", box_to_string },
    { "java/util/Date", "<init>", "()V", date_now }, { "java/util/Date", "<init>", "(J)V", date_at },
    { "java/util/Date", "getTime", "()J", date_time },
    { "java/lang/System", "identityHashCode", "(Ljava/lang/Object;)I", identity_hash },
    { "java/lang/Exception", "<init>", "(Ljava/lang/String;)V", exc_new },
    { "java/lang/Throwable", "<init>", "(Ljava/lang/String;)V", exc_new },
    { "java/lang/RuntimeException", "<init>", "(Ljava/lang/String;)V", exc_new },
    { "java/lang/Throwable", "getMessage", "()Ljava/lang/String;", exc_message },
    { "java/lang/Throwable", "getLocalizedMessage", "()Ljava/lang/String;", exc_message },
    { "java/lang/Throwable", "setStackTrace", "([Ljava/lang/StackTraceElement;)V", answer_void },
#define BND "android/os/Bundle"
    { BND, "<init>", "()V", bundle_new }, { BND, "<init>", "(Landroid/os/Bundle;)V", bundle_copy },
    { BND, "putString", "(Ljava/lang/String;Ljava/lang/String;)V", b_put_obj },
    { BND, "putInt", "(Ljava/lang/String;I)V", b_put_i }, { BND, "putLong", "(Ljava/lang/String;J)V", b_put_j },
    { BND, "putBoolean", "(Ljava/lang/String;Z)V", b_put_z }, { BND, "putFloat", "(Ljava/lang/String;F)V", b_put_f },
    { BND, "putDouble", "(Ljava/lang/String;D)V", b_put_d },
    { BND, "putBundle", "(Ljava/lang/String;Landroid/os/Bundle;)V", b_put_obj },
    { BND, "putParcelable", "(Ljava/lang/String;Landroid/os/Parcelable;)V", b_put_obj },
    { BND, "putParcelableArrayList", "(Ljava/lang/String;Ljava/util/ArrayList;)V", b_put_obj },
    { BND, "putStringArray", "(Ljava/lang/String;[Ljava/lang/String;)V", b_put_obj },
    { BND, "putSerializable", "(Ljava/lang/String;Ljava/io/Serializable;)V", b_put_obj },
    { BND, "putAll", "(Landroid/os/Bundle;)V", b_put_all },
    { BND, "getString", "(Ljava/lang/String;)Ljava/lang/String;", b_get_str },
    { BND, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", b_get_str_or },
    { BND, "getInt", "(Ljava/lang/String;)I", b_get_i }, { BND, "getInt", "(Ljava/lang/String;I)I", b_get_i_or },
    { BND, "getLong", "(Ljava/lang/String;)J", b_get_j }, { BND, "getLong", "(Ljava/lang/String;J)J", b_get_j_or },
    { BND, "getBoolean", "(Ljava/lang/String;)Z", b_get_z }, { BND, "getBoolean", "(Ljava/lang/String;Z)Z", b_get_z_or },
    { BND, "getFloat", "(Ljava/lang/String;)F", b_get_f }, { BND, "getDouble", "(Ljava/lang/String;)D", b_get_d },
    { BND, "getBundle", "(Ljava/lang/String;)Landroid/os/Bundle;", b_get_obj },
    { BND, "get", "(Ljava/lang/String;)Ljava/lang/Object;", b_get_obj },
    { BND, "containsKey", "(Ljava/lang/String;)Z", m_contains }, { BND, "remove", "(Ljava/lang/String;)V", m_remove },
    { BND, "keySet", "()Ljava/util/Set;", m_key_set }, { BND, "size", "()I", l_size },
    { BND, "isEmpty", "()Z", l_is_empty }, { BND, "clear", "()V", l_clear },
#define INT "android/content/Intent"
    { INT, "<init>", "()V", intent_new }, { INT, "<init>", "(Ljava/lang/String;)V", intent_new_action },
    { INT, "<init>", "(Landroid/content/Context;Ljava/lang/Class;)V", intent_new },
    { INT, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V", intent_new_action },
    { INT, "setAction", "(Ljava/lang/String;)Landroid/content/Intent;", intent_set_action },
    { INT, "getAction", "()Ljava/lang/String;", intent_action },
    { INT, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;", intent_set_data },
    { INT, "getData", "()Landroid/net/Uri;", intent_data },
    { INT, "setFlags", "(I)Landroid/content/Intent;", intent_flags }, { INT, "addFlags", "(I)Landroid/content/Intent;", intent_flags },
    { INT, "getFlags", "()I", intent_get_flags },
    { INT, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;", intent_self },
    { INT, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", intent_self },
    { INT, "setClass", "(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;", intent_self },
    { INT, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", i_put_obj },
    { INT, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;", i_put_i },
    { INT, "putExtra", "(Ljava/lang/String;J)Landroid/content/Intent;", i_put_j },
    { INT, "putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;", i_put_z },
    { INT, "putExtra", "(Ljava/lang/String;F)Landroid/content/Intent;", i_put_f },
    { INT, "putExtra", "(Ljava/lang/String;Landroid/os/Bundle;)Landroid/content/Intent;", i_put_obj },
    { INT, "putExtra", "(Ljava/lang/String;Landroid/os/Parcelable;)Landroid/content/Intent;", i_put_obj },
    { INT, "putExtras", "(Landroid/os/Bundle;)Landroid/content/Intent;", intent_self },
    { INT, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;", b_get_str },
    { INT, "getIntExtra", "(Ljava/lang/String;I)I", b_get_i_or },
    { INT, "getLongExtra", "(Ljava/lang/String;J)J", b_get_j_or },
    { INT, "getBooleanExtra", "(Ljava/lang/String;Z)Z", b_get_z_or },
    { INT, "hasExtra", "(Ljava/lang/String;)Z", m_contains },
    { INT, "getExtras", "()Landroid/os/Bundle;", i_extras },
    { "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", uri_parse },
    { "android/net/Uri", "toString", "()Ljava/lang/String;", uri_to_string },
    { "android/net/Uri", "getScheme", "()Ljava/lang/String;", uri_scheme },
    { "android/net/Uri", "getHost", "()Ljava/lang/String;", uri_host },
    { "android/net/Uri", "getPath", "()Ljava/lang/String;", uri_path },
    { "android/net/Uri", "getQueryParameter", "(Ljava/lang/String;)Ljava/lang/String;", uri_query_param },
    { "java/lang/Class", "isArray", "()Z", answer_false },
    { "org/fmod/FMODAudioDevice", "<init>", "()V", fmod_new },
    { "org/fmod/FMODAudioDevice", "start", "()V", fmod_start },
    { "org/fmod/FMODAudioDevice", "stop", "()V", fmod_stop },
    { "org/fmod/FMODAudioDevice", "close", "()V", fmod_stop },
    { "org/fmod/FMODAudioDevice", "isRunning", "()Z", fmod_is_running },
    { "org/fmod/FMODAudioDevice", "startAudioRecord", "(III)I", fmod_no_record },
    { "org/fmod/FMODAudioDevice", "stopAudioRecord", "()V", fmod_stop_record },
    { ME, "getAction", "()I", me_action },
    { ME, "getActionMasked", "()I", me_action_masked },
    { ME, "getActionIndex", "()I", me_action_index },
    { ME, "getPointerCount", "()I", me_count },
    { ME, "getPointerId", "(I)I", me_pointer_id },
    { ME, "findPointerIndex", "(I)I", me_find_pointer },
    { ME, "getX", "()F", me_x0 },
    { ME, "getY", "()F", me_y0 },
    { ME, "getRawX", "()F", me_x0 },
    { ME, "getRawY", "()F", me_y0 },
    { ME, "getX", "(I)F", me_x },
    { ME, "getY", "(I)F", me_y },
    { ME, "getRawX", "(I)F", me_x },
    { ME, "getRawY", "(I)F", me_y },
    { ME, "getAxisValue", "(I)F", me_axis0 },
    { ME, "getAxisValue", "(II)F", me_axis },
    { ME, "getPressure", "()F", me_pressure0 },
    { ME, "getPressure", "(I)F", me_pressure },
    { ME, "getSize", "(I)F", me_size },
    { ME, "getTouchMajor", "(I)F", me_touch_major },
    { ME, "getTouchMinor", "(I)F", me_touch_major },
    { ME, "getOrientation", "(I)F", me_orientation },
    { ME, "getToolType", "(I)I", me_tool },
    { ME, "getButtonState", "()I", me_buttons },
    { ME, "getHistorySize", "()I", ie_zero },
    { ME, "getFlags", "()I", ie_zero },
    { ME, "getEdgeFlags", "()I", ie_zero },
    { ME, "getMetaState", "()I", ie_zero },
    { ME, "getDownTime", "()J", ie_down_time },
    { ME, "obtain", "(L" ME ";)L" ME ";", me_obtain },
    { ME, "recycle", "()V", ie_void },
    { KE, "getAction", "()I", ke_action },
    { KE, "getKeyCode", "()I", ke_keycode },
    { KE, "getRepeatCount", "()I", ke_repeat },
    { KE, "getMetaState", "()I", ke_meta },
    { KE, "getUnicodeChar", "()I", ke_unicode },
    { KE, "getUnicodeChar", "(I)I", ke_unicode },
    { KE, "getScanCode", "()I", ie_zero },
    { KE, "getFlags", "()I", ie_zero },
    { KE, "getDownTime", "()J", ie_down_time },
    { KE, "getKeyCharacterMap", "()Landroid/view/KeyCharacterMap;", ke_char_map },
    { "android/view/KeyCharacterMap", "get", "(II)I", ie_zero },
    { "android/view/InputEvent", "getSource", "()I", ie_source },
    { "android/view/InputEvent", "getDeviceId", "()I", ie_device_id },
    { "android/view/InputEvent", "getEventTime", "()J", ie_time },
    { "android/view/InputEvent", "getEventTimeNanos", "()J", ie_time_nanos },
    { "android/view/InputEvent", "isFromSource", "(I)Z", ie_from_source },
    { "android/view/InputEvent", "getDevice", "()L" ID ";", ie_device },
    { ME, "getSource", "()I", ie_source },
    { ME, "getDeviceId", "()I", ie_device_id },
    { ME, "getEventTime", "()J", ie_time },
    { ME, "isFromSource", "(I)Z", ie_from_source },
    { ME, "getDevice", "()L" ID ";", ie_device },
    { KE, "getSource", "()I", ie_source },
    { KE, "getDeviceId", "()I", ie_device_id },
    { KE, "getEventTime", "()J", ie_time },
    { KE, "isFromSource", "(I)Z", ie_from_source },
    { KE, "getDevice", "()L" ID ";", ie_device },
    { ID, "getDeviceIds", "()[I", id_ids },
    { ID, "getDevice", "(I)L" ID ";", id_get },
    { ID, "getId", "()I", id_id },
    { ID, "getName", "()Ljava/lang/String;", id_name },
    { ID, "getDescriptor", "()Ljava/lang/String;", id_descriptor },
    { ID, "getVendorId", "()I", id_vendor },
    { ID, "getProductId", "()I", id_product },
    { ID, "getSources", "()I", id_sources },
    { ID, "supportsSource", "(I)Z", id_supports },
    { ID, "getKeyboardType", "()I", id_keyboard },
    { ID, "isVirtual", "()Z", id_false },
    { ID, "isExternal", "()Z", id_true },
    { ID, "isEnabled", "()Z", id_true },
    { ID, "getControllerNumber", "()I", id_controller },
    { ID, "getMotionRange", "(I)L" MR ";", id_range },
    { ID, "getMotionRange", "(II)L" MR ";", id_range },
    { MR, "getAxis", "()I", mr_axis },
    { MR, "getMin", "()F", mr_min },
    { MR, "getMax", "()F", mr_max },
    { MR, "getRange", "()F", mr_range },
    { MR, "getFlat", "()F", mr_flat },
    { MR, "getFuzz", "()F", mr_zero_f },
    { MR, "getResolution", "()F", mr_zero_f },
    { MR, "getSource", "()I", mr_source },
    { "java/lang/ClassLoader", "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", cl_find_library },
    { NULL, "getClass", "()Ljava/lang/Class;", get_class },
    // C#'s AndroidJavaObject asks with an Object answer (Call<AndroidJavaObject>("getClass")):
    // Unity builds a call's signature from each argument's getClass().getName()
    { NULL, "getClass", "()Ljava/lang/Object;", get_class },
    { "android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", storage_mounted },
    { "com/epicgames/ue4/GameActivity", "getPackageName", "()Ljava/lang/String;", ctx_package_name },
    { "com/epicgames/ue4/GameActivity", "getPackageManager", "()Landroid/content/pm/PackageManager;", pm_object },
    { "com/epicgames/ue4/GameActivity", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", ai_object },
    { "com/epicgames/ue4/PackageManager", "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", pi_object },
    { "com/epicgames/ue4/PackageManager", "getApplicationInfo", "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", ai_object },
    { "com/epicgames/ue4/GameActivity", "getVersionCode", "()I", ctx_version_code },
    { "com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataString", "(Ljava/lang/String;)Ljava/lang/String;", ue_meta_string },
    { "com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataInt", "(Ljava/lang/String;)I", ue_meta_int },
    { "com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataBoolean", "(Ljava/lang/String;)Z", ue_meta_bool },
    { "com/epicgames/ue4/GameActivity", "AndroidThunkJava_HasMetaDataKey", "(Ljava/lang/String;)Z", ue_meta_has },
    // (UE: 0 = no connection; Kuro's launcher then gave up on its remote config) 2 = Ethernet
    { "com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetNetworkConnectionType", "()I", network_lan },
    { "android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", env_ext_storage },
    { "com/epicgames/ue4/GameActivity", "getFilesDir", "()Ljava/io/File;", ctx_dir_files },
    { "com/epicgames/ue4/GameActivity", "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", ctx_dir_files },
    { "com/epicgames/ue4/GameActivity", "getCacheDir", "()Ljava/io/File;", ctx_dir_cache },
    { "com/epicgames/ue4/GameActivity", "getExternalCacheDir", "()Ljava/io/File;", ctx_dir_cache },
    { "com/epicgames/ue4/GameActivity", "getObbDir", "()Ljava/io/File;", ctx_dir_obb },
    { "com/epicgames/ue4/GameActivity", "getDataDir", "()Ljava/io/File;", ctx_dir_data },
    { "com/epicgames/ue4/GameActivity", "getPackageCodePath", "()Ljava/lang/String;", ctx_apk_path },
    { "com/epicgames/ue4/GameActivity", "getPackageResourcePath", "()Ljava/lang/String;", ctx_apk_path },
    { "com/unity3d/player/UnityPlayer", "initializeGoogleAr", "()Z", answer_false },
    { "android/os/Process", "setThreadPriority", "(II)V", answer_void },
    { "android/content/Intent", "getExtras", "()Landroid/os/Bundle;", answer_null },
    { "android/content/Context", "getObbDirs", "()[Ljava/io/File;", obb_dirs },
    { "android/hardware/display/DisplayManager", "getDisplay", "(I)Landroid/view/Display;", display_get },
    { "android/hardware/display/DisplayManager", "registerDisplayListener",
      "(Landroid/hardware/display/DisplayManager$DisplayListener;Landroid/os/Handler;)V", answer_void },
    { "android/view/Display", "getDisplayId", "()I", answer_zero },
    { "android/view/Display", "getRotation", "()I", answer_zero },
    { "android/view/Display", "getWidth", "()I", display_width },
    { "android/view/Display", "getHeight", "()I", display_height },
    { "android/view/Display", "getRealMetrics", "(Landroid/util/DisplayMetrics;)V", answer_void },
    { "android/view/Display", "getMetrics", "(Landroid/util/DisplayMetrics;)V", answer_void },
    { "android/view/Display", "getRefreshRate", "()F", display_refresh },
    { "android/view/Display", "isWideColorGamut", "()Z", answer_false },
    { "android/view/Display", "isHdr", "()Z", answer_false },
    { "android/view/Display", "getAppVsyncOffsetNanos", "()J", answer_zero_long },
    { "android/view/Display", "getPresentationDeadlineNanos", "()J", answer_zero_long },
    { "android/view/Display", "getSupportedModes", "()[Landroid/view/Display$Mode;", answer_null },
    { "java/lang/String", "<init>", "([BLjava/lang/String;)V", string_from_bytes },
    { "java/lang/String", "<init>", "([B)V", string_from_bytes },
    { "android/net/Uri", "encode", "(Ljava/lang/String;)Ljava/lang/String;", uri_encode },
    { "android/app/Activity", "setRequestedOrientation", "(I)V", act_set_orientation },
    { "android/app/Activity", "runOnUiThread", "(Ljava/lang/Runnable;)V", run_on_ui },
    { "com/unity3d/player/ReflectionHelper", "newProxyInstance",
      "(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;", new_proxy },
    { "com/unity3d/player/ReflectionHelper", "newProxyInstance",
      "(Lcom/unity3d/player/UnityPlayer;J[Ljava/lang/Class;)Ljava/lang/Object;", new_proxy },
    { "bitter/jnibridge/JNIBridge", "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;", new_bridge_proxy },
    { "java/lang/reflect/Method", "getName", "()Ljava/lang/String;", method_get_name },
    { "com/unity3d/player/ReflectionHelper", "getMethodID",
      "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;", rh_method },
    { "com/unity3d/player/ReflectionHelper", "getConstructorID",
      "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;", rh_constructor },
    { "com/unity3d/player/ReflectionHelper", "getFieldID",
      "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;", rh_field },
    { "com/unity3d/player/ReflectionHelper", "getFieldSignature", "(Ljava/lang/reflect/Field;)Ljava/lang/String;",
      rh_field_signature },
    { "com/unity3d/player/ReflectionHelper", "createInvocationError", "(JZ)Ljava/lang/Object;", answer_null },
    { "java/lang/reflect/Method", "getDeclaringClass", "()Ljava/lang/Class;", declaring_class },
    { "java/lang/reflect/Field", "getDeclaringClass", "()Ljava/lang/Class;", declaring_class },
    { "java/lang/reflect/Constructor", "getDeclaringClass", "()Ljava/lang/Class;", declaring_class },
    { "java/lang/reflect/Field", "getName", "()Ljava/lang/String;", method_get_name },
    { "java/lang/reflect/Method", "equals", "(Ljava/lang/Object;)Z", method_equals },
    { "java/lang/reflect/Method", "hashCode", "()I", method_hash },
    { "java/lang/Boolean", "<init>", "(Z)V", box_z },
    { "java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", box_z },
    { "java/lang/Integer", "<init>", "(I)V", box_i },
    { "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", box_i },
    { "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", integer_parse },
    { "java/lang/Long", "<init>", "(J)V", box_j },
    { "java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", box_j },
    { "java/lang/Float", "<init>", "(F)V", box_f },
    { "java/lang/Float", "valueOf", "(F)Ljava/lang/Float;", box_f },
    { "java/lang/Double", "<init>", "(D)V", box_d },
    { "java/lang/Double", "valueOf", "(D)Ljava/lang/Double;", box_d },
    { "android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", get_main_looper },
    { "android/os/Looper", "myLooper", "()Landroid/os/Looper;", get_main_looper },
    { "android/os/Looper", "quit", "()V", answer_void },
    { "android/os/Looper", "quitSafely", "()V", answer_void },
    { "android/os/HandlerThread", "<init>", "(Ljava/lang/String;)V", ht_new },
    { "android/os/HandlerThread", "<init>", "(Ljava/lang/String;I)V", ht_new },
    { "android/os/HandlerThread", "getLooper", "()Landroid/os/Looper;", ht_get_looper },
    { "android/os/HandlerThread", "quit", "()Z", answer_true },
    { "android/os/HandlerThread", "quitSafely", "()Z", answer_true },
    { "java/lang/Thread", "start", "()V", thread_start },
    { "android/os/HandlerThread", "start", "()V", thread_start },
    { "android/os/Handler", "<init>", "()V", handler_new0 },
    { "android/os/Handler", "<init>", "(Landroid/os/Looper;)V", handler_new1 },
    { "android/os/Handler", "<init>", "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", handler_new2 },
    { "android/os/Handler", "post", "(Ljava/lang/Runnable;)Z", handler_post },
    { "android/os/Handler", "postAtFrontOfQueue", "(Ljava/lang/Runnable;)Z", handler_post },
    { "android/os/Handler", "postDelayed", "(Ljava/lang/Runnable;J)Z", handler_post_delayed },
    { "android/os/Handler", "obtainMessage", "(I)Landroid/os/Message;", obtain1 },
    { "android/os/Handler", "obtainMessage", "(ILjava/lang/Object;)Landroid/os/Message;", obtain2 },
    { "android/os/Handler", "obtainMessage", "(III)Landroid/os/Message;", obtain3 },
    { "android/os/Handler", "obtainMessage", "(IIILjava/lang/Object;)Landroid/os/Message;", obtain4 },
    { "android/os/Handler", "sendMessage", "(Landroid/os/Message;)Z", send_message },
    { "android/os/Handler", "sendMessageDelayed", "(Landroid/os/Message;J)Z", send_message_delayed },
    { "android/os/Handler", "sendEmptyMessage", "(I)Z", send_empty },
    { "android/os/Handler", "sendEmptyMessageDelayed", "(IJ)Z", send_empty_delayed },
    { "android/os/Handler", "removeMessages", "(I)V", answer_void },
    { "android/os/Handler", "removeCallbacks", "(Ljava/lang/Runnable;)V", answer_void },
    { "android/os/Handler", "removeCallbacksAndMessages", "(Ljava/lang/Object;)V", answer_void },
    { "android/os/Handler", "getLooper", "()Landroid/os/Looper;", ht_get_looper },
    { "android/os/Message", "sendToTarget", "()V", send_to_target },
    { "android/view/Choreographer", "getInstance", "()Landroid/view/Choreographer;", choreo_get },
    { "android/view/Choreographer", "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", choreo_post },
    { "android/view/Choreographer", "postFrameCallbackDelayed",
      "(Landroid/view/Choreographer$FrameCallback;J)V", choreo_post_delayed },
    { "android/view/Choreographer", "removeFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", answer_void },
    { "java/lang/System", "nanoTime", "()J", nano_time },
    { "android/os/SystemClock", "uptimeMillis", "()J", uptime_ms },
    { "com/customRobTop/BaseRobTopActivity", "getDeviceRefreshRate", "()F", cc_refresh_rate },
    // FMOD's Java side (org.fmod.FMOD, set up by FMOD.init(context) on a phone): initialized, and
    // AAudio there - its output then is AAudio (ours), not Java's AudioTrack (Geometry Dash: silent)
    { "com/rovio/androidpermissionsplugin/AndroidPermissions", "instance", "()Ljava/lang/Object;", rovio_permissions },
    { "com/rovio/beacon/DeviceInfo", "getDataPath", "()Ljava/lang/String;", beacon_data_path },
    { "com/rovio/beacon/DeviceInfo", "getAsyncIdentifiers", "(ZLcom/rovio/beacon/DeviceInfo$AsyncIdsListener;)V", beacon_async_ids },
    { "com/rovio/beacon/DeviceInfo", "getAsyncIdentifiers", "(ZLcom.rovio.beacon.DeviceInfo$AsyncIdsListener;)V", beacon_async_ids },
    { "com/rovio/beacon/DeviceInfo", "getMetaInstallReferrer", "()Ljava/lang/String;", beacon_empty },
    { "com/rovio/beacon/Localization", "deviceLocale", "()Ljava/lang/String;", beacon_locale },
    { "com/facebook/unity/FB", "Init", "(Ljava/lang/String;)V", fb_init },
    { "com/facebook/unity/FB", "GetSdkVersion", "()Ljava/lang/String;", fb_sdk_version },
    { "com/facebook/unity/FB", "GetUserID", "()Ljava/lang/String;", beacon_empty },
    { "com/rovio/beacon/DeviceInfo", "getPlatformIdentifiers", "()Ljava/lang/String;", beacon_platform_ids },
    { "org/fmod/FMOD", "checkInit", "()Z", yes },
    { "org/fmod/FMOD", "supportsAAudio", "()Z", yes },
    { "org/fmod/FMOD", "supportsLowLatency", "()Z", yes },
    { "org/cocos2dx/lib/Cocos2dxRenderer", "setAnimationInterval", "(D)V", cc_set_interval },
    { "android/os/SystemClock", "elapsedRealtime", "()J", uptime_ms },
    { "android/media/AudioManager", "getDevices", "(I)[Landroid/media/AudioDeviceInfo;", answer_null },
    { "android/media/AudioManager", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", audio_property },
    { "android/media/AudioManager", "getStreamVolume", "(I)I", answer_zero },
    { "android/content/Context", "checkCallingOrSelfPermission", "(Ljava/lang/String;)I", answer_zero },
    { "java/util/Locale", "toLanguageTag", "()Ljava/lang/String;", language_tag },
    { "java/lang/System", "load", "(Ljava/lang/String;)V", sys_load },
    { "android/net/Uri", "decode", "(Ljava/lang/String;)Ljava/lang/String;", uri_decode },
    // PreciseLocale (a common Unity plugin): FalsoJNI's by-name getLanguage said "1" (no such culture)
#define PL "com/kokosoft/preciselocale/PreciseLocale"
    { PL, "getLanguage", "()Ljava/lang/String;", locale_language },
    { PL, "getLanguageID", "()Ljava/lang/String;", locale_id },
    { PL, "getRegion", "()Ljava/lang/String;", locale_region },
    { PL, "getCurrencyCode", "()Ljava/lang/String;", locale_currency },
    { PL, "getCurrencySymbol", "()Ljava/lang/String;", locale_currency_symbol },
    { "java/util/Locale", "getLanguage", "()Ljava/lang/String;", locale_language },
    { "java/util/Locale", "getCountry", "()Ljava/lang/String;", locale_region },
    { "com/nekki/utils/DeviceUniqueID", "GetID", "()Ljava/lang/String;", device_id_answer },
    { "com/nekki/DeviceUniqueID", "GetID", "()Ljava/lang/String;", nekki_v2_device_id },
    { "com/nekki/utils/security/SecurityManager", "GetSignature", "()Ljava/lang/String;", apk_signature_answer },
    { "com/google/android/ump/ConsentInformation", "requestConsentInfoUpdate",
      "(Lcom.unity3d.player.UnityPlayerActivity;Lcom.google.android.ump.ConsentRequestParameters;"
      "Lcom.google.android.ump.ConsentInformation$OnConsentInfoUpdateSuccessListener;"
      "Lcom.google.android.ump.ConsentInformation$OnConsentInfoUpdateFailureListener;)V", ump_update },
    { "com/google/unity/ump/UnityConsentForm", "<init>",
      "(Lcom.unity3d.player.UnityPlayerActivity;Lcom.google.unity.ump.UnityConsentFormCallback;)V", ump_form_new },
    { "com/google/unity/ump/UnityConsentForm", "<init>",
      "(Landroid/app/Activity;Lcom/google/unity/ump/UnityConsentFormCallback;)V", ump_form_new },
    { "com/google/unity/ump/UnityConsentForm", "loadAndShowConsentFormIfRequired", "()V", ump_form_show },
    { "com/google/unity/ads/nextgen/UnityMobileAds", "initialize",
      "(Lcom.unity3d.player.UnityPlayerActivity;Lcom.google.android.libraries.ads.mobile.sdk.initialization.OnAdapterInitializationCompleteListener;)V",
      gma_init },
    { "com/google/android/libraries/ads/mobile/sdk/initialization/InitializationStatus", "getAdapterStatusMap", "()Ljava/util/Map;",
      gma_status_map },
    { "com/google/unity/ump/UnityConsentForm", "showPrivacyOptionsForm", "()V", ump_form_show },
#define UMP_PO "com/google/android/ump/ConsentInformation$PrivacyOptionsRequirementStatus"
    { "com/google/android/ump/ConsentInformation", "getPrivacyOptionsRequirementStatus", "()Ljava/lang/Object;", ump_privacy },
    { "com/google/android/ump/ConsentInformation", "getPrivacyOptionsRequirementStatus", "()L" UMP_PO ";", ump_privacy },
    { UMP_PO, "ordinal", "()I", ump_privacy_ordinal },
    { UMP_PO, "name", "()Ljava/lang/String;", ump_privacy_name },
    { UMP_PO, "toString", "()Ljava/lang/String;", ump_privacy_name },
    { "com/nekki/billing/BillingHelper", "Connect", "(Lcom.nekki.billing.IConnectingCallback;)V", nekki_billing_connect },
    { "com/nekki/billing/BillingHelper", "Connect", "(Lcom/nekki/billing/IConnectingCallback;)V", nekki_billing_connect },
    { "com/nekki/billing/BillingHelper", "IsConnected", "()Z", answer_false },
    { "com/android/billingclient/api/BillingClient", "startConnection",
      "(Lcom.android.billingclient.api.BillingClientStateListener;)V", billing_start },
    { "com/android/billingclient/api/BillingClient", "startConnection",
      "(Lcom/android/billingclient/api/BillingClientStateListener;)V", billing_start },
    { "com/android/billingclient/api/BillingResult", "getResponseCode", "()I", billing_code },
    { "com/android/billingclient/api/BillingResult", "getDebugMessage", "()Ljava/lang/String;", billing_message },
#define PGS_HF "com/google/games/bridge/HelperFragment"
#define PGS_TR "com/google/games/bridge/TokenResult"
#define PGS_PR "com/google/android/gms/common/api/PendingResult"
    { PGS_HF, "fetchToken",
      "(Lcom.unity3d.player.UnityPlayerActivity;ZZZZLjava/lang/String;Z[Ljava/lang/String;ZLjava/lang/Object;)Ljava/lang/Object;",
      pgs_fetch_token },
    { PGS_HF, "fetchToken",
      "(Landroid/app/Activity;ZZZZLjava/lang/String;Z[Ljava/lang/String;ZLjava/lang/String;)Lcom/google/android/gms/common/api/PendingResult;",
      pgs_fetch_token },
    { PGS_PR, "setResultCallback", "(Lcom.google.android.gms.common.api.ResultCallback;)V", pgs_set_callback },
    { PGS_PR, "setResultCallback", "(Lcom/google/android/gms/common/api/ResultCallback;)V", pgs_set_callback },
    { PGS_PR, "setResultCallback", "(Ljava/lang/Object;)V", pgs_set_callback },
    { PGS_TR, "getStatusCode", "()I", pgs_status },
    { PGS_TR, "getAuthCode", "()Ljava/lang/String;", pgs_null_text },
    { PGS_TR, "getEmail", "()Ljava/lang/String;", pgs_null_text },
    { PGS_TR, "getIdToken", "()Ljava/lang/String;", pgs_null_text },
    { "com/nekki/utils/security/SecurityManager", "GetInstallerId", "()Ljava/lang/String;", play_store },
    { "com/unity3d/player/UnityPlayer", "getNetworkConnectivity", "()I", network_lan },
    { "com/stansassets/mnp/NativePopupsManager", "ShowMessage",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V", mnp_show_message },
#define UMP "com/google/android/ump/ConsentInformation"
    { "com/google/android/ump/UserMessagingPlatform", "getConsentInformation",
      "(Lcom.unity3d.player.UnityPlayerActivity;)Ljava/lang/Object;", ump_info },
    { "com/google/android/ump/UserMessagingPlatform", "getConsentInformation",
      "(Landroid/content/Context;)Lcom/google/android/ump/ConsentInformation;", ump_info },
    { UMP, "getConsentStatus", "()I", ump_status },
    { UMP, "canRequestAds", "()Z", answer_true },
    { UMP, "isConsentFormAvailable", "()Z", answer_false },
    { UMP, "reset", "()V", answer_void },
    { UMP, "requestConsentInfoUpdate",
      "(Landroid/app/Activity;Lcom/google/android/ump/ConsentRequestParameters;Lcom/google/android/ump/ConsentInformation$OnConsentInfoUpdateSuccessListener;Lcom/google/android/ump/ConsentInformation$OnConsentInfoUpdateFailureListener;)V",
      ump_update },
    { "android/provider/Settings$Secure", "getString",
      "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", secure_string },
    { "javax/net/ssl/TrustManagerFactory", "getDefaultAlgorithm", "()Ljava/lang/String;", tmf_algorithm },
    { "javax/net/ssl/TrustManagerFactory", "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;",
      tmf_instance },
    { "javax/net/ssl/TrustManagerFactory", "init", "(Ljava/security/KeyStore;)V", answer_void },
    { "javax/net/ssl/TrustManagerFactory", "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;", tmf_trust_managers },
    { NULL, "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;", tm_issuers },
    { NULL, "getEncoded", "()[B", cert_encoded },
    { "java/io/File", "<init>", "(Ljava/lang/String;)V", file_new },
    { "java/io/File", "<init>", "(Ljava/io/File;Ljava/lang/String;)V", file_new_child },
    { "java/io/File", "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", file_new_child },
    { "java/io/File", "getAbsolutePath", "()Ljava/lang/String;", file_path },
    { "java/io/File", "getPath", "()Ljava/lang/String;", file_path },
    { "java/io/File", "getCanonicalPath", "()Ljava/lang/String;", file_path },
    { "java/io/File", "toString", "()Ljava/lang/String;", file_path },
    { "java/io/File", "exists", "()Z", file_exists },
    { "java/io/File", "isDirectory", "()Z", file_is_dir },
    { "java/io/File", "length", "()J", file_length },
    { "java/io/File", "delete", "()Z", file_delete },
    { "java/io/File", "mkdirs", "()Z", file_mkdirs },
    { "java/io/File", "mkdir", "()Z", file_mkdir },
    { "java/io/File", "setWritable", "(Z)Z", answer_true },
    { "java/io/File", "setReadable", "(Z)Z", answer_true },
    { "java/io/File", "setReadOnly", "()Z", answer_true },
    { "java/io/FileOutputStream", "<init>", "(Ljava/io/File;)V", fos_new },
    { "java/io/FileOutputStream", "<init>", "(Ljava/lang/String;)V", fos_new },
    { "java/io/FileOutputStream", "<init>", "(Ljava/io/File;Z)V", fos_new_append },
    { "java/io/FileOutputStream", "<init>", "(Ljava/lang/String;Z)V", fos_new_append },
    { "java/io/FileOutputStream", "write", "([BII)V", fos_write_part },
    { "java/io/FileOutputStream", "write", "([B)V", fos_write_all },
    { "java/io/FileOutputStream", "write", "(I)V", fos_write_byte },
    { "java/io/FileOutputStream", "flush", "()V", answer_void },
    { "java/io/FileOutputStream", "close", "()V", fos_close },
    { "com/sybogames/chili/migration/KilooPlatformAndroidBridge", "getUserdataPath", "()Ljava/lang/String;",
      userdata_path },
    { "com/sybo/nativevibration/VibrationPlugin", "IsStandardVibrationSupported", "()Z", vib_yes },
    { "com/sybo/nativevibration/VibrationPlugin", "ArePredefinedHapticsSupported", "()Z", vib_yes },
    { "com/sybo/nativevibration/VibrationPlugin", "IsVibrationWithCustomWaveFormSupported", "()Z", vib_yes },
    { "com/sybo/nativevibration/VibrationPlugin", "VibrateWithWaveForm", "([J[II)V", vib_wave },
    { "com/sybo/nativevibration/VibrationPlugin", "Vibrate", "(I)V", vib_ms },
    { "com/sybo/nativevibration/VibrationPlugin", "VibrateWithPredefinedEffect", "(Ljava/lang/String;)V", vib_effect },
    { "android/os/Vibrator", "hasVibrator", "()Z", avib_has },
    { "android/os/Vibrator", "hasAmplitudeControl", "()Z", avib_has },
    { "android/os/Vibrator", "vibrate", "(J)V", avib_vibrate_ms },
    { "android/os/Vibrator", "vibrate", "(Landroid/os/VibrationEffect;)V", avib_vibrate },
    { "android/os/Vibrator", "vibrate", "(Landroid.os.VibrationEffect;)V", avib_vibrate },
    { "android/os/Vibrator", "vibrate", "(Landroid/os/VibrationEffect;Landroid/media/AudioAttributes;)V", avib_vibrate },
    { "android/os/Vibrator", "vibrate", "([JI)V", avib_vibrate },
    { "android/os/VibrationEffect", "createOneShot", "(JI)Landroid/os/VibrationEffect;", avib_one_shot },
    { "android/os/VibrationEffect", "createWaveform", "([JI)Landroid/os/VibrationEffect;", avib_wave },
    { "android/os/VibrationEffect", "createWaveform", "([J[II)Landroid/os/VibrationEffect;", avib_wave },
    // C#'s AndroidJavaObject calls these on "java/lang/Object" with Object signatures: any class
    { NULL, "getInstallSourceInfo", "(Ljava/lang/String;)Ljava/lang/Object;", install_source },
    { NULL, "getApkContentsSigners", "()[Landroid/content/pm/Signature;", signers },
    { NULL, "getApkContentsSigners", "()[Ljava/lang/Object;", signers },
    { NULL, "getSigningCertificateHistory", "()[Landroid/content/pm/Signature;", signers },
    { NULL, "getSigningCertificateHistory", "()[Ljava/lang/Object;", signers },
    { NULL, "hasMultipleSigners", "()Z", answer_false },
    { "android/content/pm/Signature", "toByteArray", "()[B", sig_bytes },
    { "android/content/pm/Signature", "toCharsString", "()Ljava/lang/String;", sig_chars },
    { "android/content/pm/Signature", "hashCode", "()I", sig_hash },
    { NULL, "hasSystemFeature", "(Ljava/lang/String;)Z", has_feature },
    { NULL, "hasSystemFeature", "(Ljava/lang/String;I)Z", has_feature },
    { NULL, "getInstallSourceInfo", "(Ljava/lang/String;)Landroid/content/pm/InstallSourceInfo;", install_source },
    { NULL, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", play_store },
    { NULL, "getInstallingPackageName", "()Ljava/lang/String;", play_store },
    { NULL, "getInitiatingPackageName", "()Ljava/lang/String;", play_store },
    { NULL, "getOriginatingPackageName", "()Ljava/lang/String;", play_store },
    { "java/lang/System", "loadLibrary", "(Ljava/lang/String;)V", sys_load_library },
    { "android/os/PowerManager", "isSustainedPerformanceModeSupported", "()Z", answer_false },
    { "android/hardware/input/InputManager", "getInputDeviceIds", "()[I", id_ids },
    { "android/hardware/input/InputManager", "getInputDevice", "(I)L" ID ";", id_get },
    { "android/hardware/input/InputManager", "registerInputDeviceListener",
      "(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V", answer_void },
    { "android/content/res/Resources", "getIdentifier", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", answer_zero },
    { "com/unity3d/player/UnityPlayer", "startOrientationListener", "(I)Z", answer_false },
    { "com/unity3d/player/UnityPlayer", "getLaunchURL", "()Ljava/lang/String;", answer_null },
    { "com/unity3d/player/UnityPlayer", "startActivityIndicator", "(I)V", answer_void },
    { "com/unity3d/player/UnityPlayer", "stopActivityIndicator", "()V", answer_void },
    { "com/unity3d/player/UnityPlayer", "hideSoftInput", "()V", answer_void },
    { NULL, "clearHFPStat", "()V", answer_void },
#define CC "com/google/androidgamesdk/ChoreographerCallback"
    { CC, "<init>", "(J)V", cc_new },
    { CC, "postFrameCallback", "()V", cc_post },
    { CC, "postFrameCallbackDelayed", "(J)V", cc_post_delayed },
    { CC, "terminate", "()V", answer_void },
#define SP "android/content/SharedPreferences"
#define ED "android/content/SharedPreferences$Editor"
    { "android/content/Context", "getSharedPreferences", "(Ljava/lang/String;I)L" SP ";", sp_get },
    { "android/app/Activity", "getPreferences", "(I)L" SP ";", sp_get_activity },
    { SP, "edit", "()L" ED ";", sp_edit },
    { SP, "getInt", "(Ljava/lang/String;I)I", sp_get_int },
    { SP, "getLong", "(Ljava/lang/String;J)J", sp_get_long },
    { SP, "getFloat", "(Ljava/lang/String;F)F", sp_get_float },
    { SP, "getBoolean", "(Ljava/lang/String;Z)Z", sp_get_bool },
    { SP, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", sp_get_string },
    // by reflection the default is an Object (Cut the Rope's SecureStorage): it went to FalsoJNI's
    // by-name answer, not the saved value; C# got null and the game's loading waited forever
    { SP, "getString", "(Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/String;", sp_get_string },
    { SP, "contains", "(Ljava/lang/String;)Z", sp_contains },
    { SP, "getAll", "()Ljava/util/Map;", sp_get_all },
    { ED, "putInt", "(Ljava/lang/String;I)L" ED ";", ed_put_int },
    { ED, "putLong", "(Ljava/lang/String;J)L" ED ";", ed_put_long },
    { ED, "putFloat", "(Ljava/lang/String;F)L" ED ";", ed_put_float },
    { ED, "putBoolean", "(Ljava/lang/String;Z)L" ED ";", ed_put_bool },
    { ED, "putString", "(Ljava/lang/String;Ljava/lang/String;)L" ED ";", ed_put_string },
    { ED, "remove", "(Ljava/lang/String;)L" ED ";", ed_remove },
    { ED, "clear", "()L" ED ";", ed_clear },
    { ED, "apply", "()V", ed_apply },
    { ED, "commit", "()Z", ed_commit },
    { "java/util/Map", "entrySet", "()Ljava/util/Set;", map_entry_set },
    { "java/util/Set", "iterator", "()Ljava/util/Iterator;", set_iterator },
    { "java/util/Iterator", "hasNext", "()Z", it_has_next },
    { "java/util/Iterator", "next", "()Ljava/lang/Object;", it_next },
    { "java/util/Map$Entry", "getKey", "()Ljava/lang/Object;", entry_key },
    { "java/util/Map$Entry", "getValue", "()Ljava/lang/Object;", entry_value },
    { "java/lang/Integer", "intValue", "()I", box_int },
    { "java/lang/Long", "longValue", "()J", box_long },
    { "java/lang/Float", "floatValue", "()F", box_float },
    { "java/lang/Boolean", "booleanValue", "()Z", box_bool },
    { "java/lang/String", "length", "()I", s_length },
    { "java/lang/String", "isEmpty", "()Z", s_is_empty },
    { "java/lang/String", "equals", "(Ljava/lang/Object;)Z", s_equals },
    { "java/lang/String", "equalsIgnoreCase", "(Ljava/lang/String;)Z", s_equals_ic },
    { "java/lang/String", "hashCode", "()I", s_hash },
    { "java/lang/String", "charAt", "(I)C", s_char_at },
    { "java/lang/String", "getBytes", "()[B", s_get_bytes },
    { "java/lang/String", "getBytes", "(Ljava/lang/String;)[B", s_get_bytes },
    { "java/lang/String", "getBytes", "(Ljava/nio/charset/Charset;)[B", s_get_bytes },
    { "java/lang/String", "substring", "(I)Ljava/lang/String;", s_substring1 },
    { "java/lang/String", "substring", "(II)Ljava/lang/String;", s_substring2 },
    { "java/lang/String", "indexOf", "(Ljava/lang/String;)I", s_index_of_str },
    { "java/lang/String", "indexOf", "(I)I", s_index_of_char },
    { "java/lang/String", "lastIndexOf", "(I)I", s_last_index_of_char },
    { "java/lang/String", "startsWith", "(Ljava/lang/String;)Z", s_starts_with },
    { "java/lang/String", "endsWith", "(Ljava/lang/String;)Z", s_ends_with },
    { "java/lang/String", "contains", "(Ljava/lang/CharSequence;)Z", s_contains },
    { "java/lang/String", "toLowerCase", "()Ljava/lang/String;", s_lower },
    { "java/lang/String", "toUpperCase", "()Ljava/lang/String;", s_upper },
    { "java/lang/String", "trim", "()Ljava/lang/String;", s_trim },
    { "java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;", s_concat },
    { "java/lang/String", "replace", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;", s_replace },
    { "java/lang/String", "valueOf", "(I)Ljava/lang/String;", s_value_of_int },
    { "java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", class_for_name },
    { "java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", class_for_name },
    // Genshin Impact's Unity (its ReflectionHelper): the same, declared to return Object
    { "java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Object;", class_for_name },
    { "java/lang/Class", "getName", "()Ljava/lang/String;", class_get_name },
    { "java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", class_for_name },
    { "java/lang/Object", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", object_load_class },
    { "java/lang/StringBuilder", "<init>", "()V", sb_new },
    { "java/lang/StringBuilder", "<init>", "(I)V", sb_new_cap },
    { "java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V", sb_new_str },
    { "java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", sb_append_str },
    { "java/lang/StringBuilder", "append", "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;", sb_append_str },
    { "java/lang/StringBuilder", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", sb_append_str },
    { "java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", sb_append_int },
    { "java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", sb_append_long },
    { "java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", sb_append_char },
    { "java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", sb_append_bool },
    { "java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;", sb_append_float },
    { "java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;", sb_append_double },
    { "java/lang/StringBuilder", "length", "()I", sb_length },
    { NULL, "toString", "()Ljava/lang/String;", obj_to_string }, // any class
    { "android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", am_open },
    { "android/content/res/AssetManager", "list", "(Ljava/lang/String;)[Ljava/lang/String;", am_list },
    { NULL, "open", "(Ljava/lang/String;)Ljava/lang/Object;", am_open }, // (Genshin's Unity, as list)
    { NULL, "open", "(Ljava/lang/String;I)Ljava/lang/Object;", am_open },
    // Genshin's Unity: on its asset object (whose class it sees as Object), declared to return Object
    { NULL, "list", "(Ljava/lang/String;)Ljava/lang/Object;", am_list },
    { "java/lang/reflect/Array", "getLength", "(Ljava/lang/Object;)I", array_get_length },
    { "java/lang/reflect/Array", "newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;", array_new },
    { "java/lang/reflect/Array", "set", "(Ljava.lang.Object;ILjava.lang.String;)V", array_set }, // (Unity's C#: dots)
    { "java/lang/reflect/Array", "set", "(Ljava/lang/Object;ILjava/lang/Object;)V", array_set },
    { "com/mihoyo/combosdk/ComboForUnity", "invoke", "(Ljava/lang/String;Ljava/lang/String;I)V", combo_invoke },
    { "com/gme/GME/QLog", "getDefaultDir", "()Ljava/lang/String;", gme_default_dir },
    { "com/mihoyo/combosdk/ComboForUnity", "setUnityVersion", "(Ljava/lang/String;)V", combo_set_version },
    { "com/mihoyo/combosdk/ComboForUnity", "invokeReturn", "(Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/String;",
      combo_invoke_return },
    { "com/mihoyo/combosdk/ComboForUnity", "invoke", "(Ljava/lang/String;Ljava/lang/Object;I)V", combo_invoke_obj },
    { "com/nvidia/devtech/NvUtil", "getInstance", "()Lcom/nvidia/devtech/NvUtil;", nv_instance },
    { "com/nvidia/devtech/NvUtil", "hasAppLocalValue", "(Ljava/lang/String;)Z", nv_has },
    { "com/nvidia/devtech/NvUtil", "getAppLocalValue", "(Ljava/lang/String;)Ljava/lang/String;", nv_get },
    { "com/nvidia/devtech/NvUtil", "setAppLocalValue", "(Ljava/lang/String;Ljava/lang/String;)V", nv_set },
#define GPS "com/rockstargames/oswrapper/GamePlatformServices"
    { GPS, "rockstarShowInitial", "()V", rs_initial },
    { GPS, "playlistOpen", "(Ljava/lang/String;)V", rs_playlist },
    { GPS, "rockstarShowGate", "(I)V", rs_gate },
    { GPS, "rockstarSignIn", "()V", rs_sign_in },
    { GPS, "rockstarSignOut", "()V", rs_sign_out },
    { GPS, "rockstarShowCloudDisabled", "()V", rs_cloud_disabled },
    { GPS, "rockstarAccountDeletion", "()V", rs_deletion },
    { GPS, "httpHead", "(ILjava/lang/String;)V", rs_http },
    { GPS, "httpGet", "(ILjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V", rs_http },
    { GPS, "httpPost", "(ILjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[B)V", rs_http },
    { "android/net/ConnectivityManager", "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;", net_active_info },
    { "android/net/ConnectivityManager", "getNetworkInfo", "(I)Landroid/net/NetworkInfo;", net_active_info },
    { "android/net/ConnectivityManager", "getActiveNetwork", "()Landroid/net/Network;", net_active_network },
    { "android/net/ConnectivityManager", "getNetworkCapabilities", "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;", net_capabilities },
    { "android/net/NetworkCapabilities", "hasCapability", "(I)Z", net_connected },
    { "android/net/NetworkCapabilities", "hasTransport", "(I)Z", net_connected },
    { "android/net/NetworkInfo", "isConnected", "()Z", net_connected },
    { "android/net/NetworkInfo", "isConnectedOrConnecting", "()Z", net_connected },
    { "android/net/NetworkInfo", "isAvailable", "()Z", net_connected },
    { "android/net/NetworkInfo", "getType", "()I", net_type_wifi },
    { "android/net/NetworkInfo", "getTypeName", "()Ljava/lang/String;", net_type_name },
    { "com/miHoYo/DeviceInfo/Device", "getStorageTotalSize", "()J", device_storage_total },
    { "com/miHoYo/DeviceInfo/Device", "getStorageAvailableSize", "()J", device_storage_free },
    { "java/lang/reflect/Array", "get", "(Ljava/lang/Object;I)Ljava/lang/Object;", array_get },
    { "java/lang/reflect/Array", "get", "(Ljava/lang/Object;I)Ljava/lang/String;", array_get }, // (Genshin's Unity)
    { "android/content/res/AssetManager", "open", "(Ljava/lang/String;I)Ljava/io/InputStream;", am_open },
    { IS, "read", "([B)I", in_read_all },
    { IS, "read", "([BII)I", in_read_part },
    { IS, "read", "()I", in_read_byte },
    { IS, "available", "()I", in_available },
    { IS, "skip", "(J)J", in_skip },
    { IS, "mark", "(I)V", in_mark },
    { IS, "reset", "()V", in_reset },
    { IS, "markSupported", "()Z", yes },
    { IS, "close", "()V", in_close },
    { "java/util/Scanner", "<init>", "(Ljava/io/InputStream;Ljava/lang/String;)V", sc_new },
    { "java/util/Scanner", "<init>", "(Ljava/io/InputStream;)V", sc_new },
    { "java/util/Scanner", "useDelimiter", "(Ljava/lang/String;)Ljava/util/Scanner;", sc_self },
    { "java/util/Scanner", "next", "()Ljava/lang/String;", sc_next },
    { "java/util/Scanner", "hasNext", "()Z", sc_has_next },
    { "java/util/Scanner", "close", "()V", sc_close },
};
#define N_METHODS (sizeof(methods) / sizeof(methods[0]))

// Subclasses we hand out: an AssetInputStream is an InputStream.
static int class_matches(const char *want, const char *have) {
    if (!want || !have) return 1;
    if (strcmp(want, have) == 0) return 1;
    if (strcmp(want, "android/view/InputEvent") == 0 && (strcmp(have, ME) == 0 || strcmp(have, KE) == 0)) return 1;
    if (strcmp(want, IS) == 0 && strstr(have, "InputStream")) return 1;
    // lists, maps and sets as their interfaces (java/util/List.size on an ArrayList)
    static const char *const lists[] = { "java/util/ArrayList", "java/util/LinkedList", "java/util/Vector", NULL };
    static const char *const maps[] = { "java/util/HashMap", "java/util/LinkedHashMap", "java/util/TreeMap",
                                        "java/util/Hashtable", "java/util/concurrent/ConcurrentHashMap", NULL };
    if (!strcmp(want, "java/util/List") || !strcmp(want, "java/util/Collection") || !strcmp(want, "java/lang/Iterable"))
        for (int i = 0; lists[i]; i++) if (!strcmp(have, lists[i])) return 1;
    if (!strcmp(want, "java/util/Map"))
        for (int i = 0; maps[i]; i++) if (!strcmp(have, maps[i])) return 1;
    if (!strcmp(want, "java/util/Collection") && (!strcmp(have, "java/util/Set") || !strcmp(have, "java/util/HashSet")))
        return 1;
    if (!strcmp(want, "java/lang/Number") && !strncmp(have, "java/lang/", 10)) return 1;
    if (!strcmp(want, "java/lang/Throwable") && strstr(have, "Exception")) return 1;
    // Unity's activity is an Activity (and a Context): runOnUiThread & co. on it
    if (!strcmp(have, "com/unity3d/player/UnityPlayerActivity") &&
        (!strcmp(want, "android/app/Activity") || !strcmp(want, "android/content/Context")))
        return 1;
    return 0;
}

// signatures with dots for slashes (Genshin Impact's Unity: "(Ljava.lang.Object;)I"): the same
static int sig_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (*a != *b && !((*a == '.' || *a == '/') && (*b == '.' || *b == '/'))) return 0;
    return *a == *b;
}

// ---- King's engine (Candy Crush Saga): its com.king.core Java ---------------------------------
// The "app not responding" watchdog: scheduleTaskOnUiThread asks the UI thread to call back
// onTaskRunOnUiThread(handle); never answered, the game took itself for hung (an ANR report a
// second). Ours: a small thread of ours as the UI thread, the callback a moment later.
static void *king_native(const char *name) {
    for (int i = 0; i < arm_module_count(); i++) {
        void *p = so_sym(arm_module(i), name);
        if (p) return arm_module(i)->arm64 ? arm_callback((uint64_t)(uintptr_t)p) : p;
    }
    return NULL;
}
static pthread_mutex_t king_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t king_ui_cond = PTHREAD_COND_INITIALIZER;
static jobj *king_ui_due[32];
static int king_ui_n;
static void *king_ui_thread(void *arg) {
    (void)arg;
    typedef void (*run_fn)(void *, void *, jlong);
    run_fn run = (run_fn)king_native("Java_com_king_core_AppNotRespondingTaskProcessor_onTaskRunOnUiThread");
    void *env = arm_jni_env();
    for (;;) {
        pthread_mutex_lock(&king_ui_lock);
        while (!king_ui_n) pthread_cond_wait(&king_ui_cond, &king_ui_lock);
        jobj *o = king_ui_due[--king_ui_n];
        pthread_mutex_unlock(&king_ui_lock);
        if (run && o && o->field[1].z) run(env, (jobject)o, o->field[0].j);
    }
    return NULL;
}
static jvalue king_anr_new(jobject self, const jvalue *a) {
    (void)self;
    jobj *o = new_obj("com/king/core/AppNotRespondingTaskProcessor");
    o->field[0].j = a[0].j; // its native handler
    o->field[1].z = 1;      // active
    return L((jobject)o);
}
static jvalue king_anr_active(jobject self, const jvalue *a) {
    jobj *o = as_obj(self);
    if (o) o->field[1].z = a[0].z;
    return V();
}
static jvalue king_anr_schedule(jobject self, const jvalue *a) {
    (void)a;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    static pthread_t t;
    void king_ui_start(void);
    pthread_once(&once, king_ui_start);
    (void)t;
    pthread_mutex_lock(&king_ui_lock);
    if (king_ui_n < 32) king_ui_due[king_ui_n++] = as_obj(self);
    pthread_cond_signal(&king_ui_cond);
    pthread_mutex_unlock(&king_ui_lock);
    return V();
}
void king_ui_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, king_ui_thread, NULL);
    droid_log("java: King's UI thread (its watchdog's callbacks) started");
}
static jvalue king_dir(const char *sub) { // a folder of the game's, with its "/"
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", ifs_dir(), sub);
    mkdir(p, 0777);
    strncat(p, "/", sizeof(p) - strlen(p) - 1);
    return L(jstr(p));
}
static jvalue king_cache_dir(jobject self, const jvalue *a) { (void)self; (void)a; return king_dir("cache"); }
static jvalue king_home_dir(jobject self, const jvalue *a) { (void)self; (void)a; return king_dir("files"); }
static jvalue king_shared_dir(jobject self, const jvalue *a) { (void)self; (void)a; return king_dir("shared"); }
static jvalue king_language(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("en")); }
static jvalue king_country(jobject self, const jvalue *a) { (void)self; (void)a; return L(jstr("US")); }
static jvalue king_version(jobject self, const jvalue *a) {
    (void)self; (void)a;
    extern const char *droid_version_name; // app_main.c
    return L(jstr(droid_version_name && droid_version_name[0] ? droid_version_name : "1.0"));
}
static jvalue king_res_scale(jobject self, const jvalue *a) { (void)self; (void)a; return F(1.0f); }

// Answers made for some games only: every other game never sees them and keeps what it had
// before they came (a fix for one game broke others: Shadow Fight 4, Subway Surfers)
static const struct { jvalue (*fn)(jobject, const jvalue *); const char *sig_has, *games; } per_game[] = {
    { ump_form_new, NULL, "com.imangi." },   { ump_form_show, NULL, "com.imangi." },
    { gma_init, NULL, "com.imangi." },       { gma_status_map, NULL, "com.imangi." },
    { p31_instance, NULL, "com.imangi." },   { p31_show_alert, NULL, "com.imangi." },
    { p31_show_prompt, NULL, "com.imangi." }, { p31_nothing, NULL, "com.imangi." },
    { fa_app_instance_id, NULL, "com.imangi." },
    { sp_get_string, "Ljava/lang/Object;)", "com.zeptolab." }, // getString(String, Object): Cut the Rope's
    { rovio_billing_new, NULL, "com.rovio.baba" }, { rovio_billing_init, NULL, "com.rovio.baba" },
    { avib_has, NULL, VIBRATOR_GAMES }, { avib_one_shot, NULL, VIBRATOR_GAMES }, { avib_wave, NULL, VIBRATOR_GAMES },
    { avib_vibrate, NULL, VIBRATOR_GAMES }, { avib_vibrate_ms, NULL, VIBRATOR_GAMES },
    { nekki_v2_device_id, NULL, "com.nekki.vector2" },
    { st_new, NULL, VIDEO_GAMES }, { st_listener, NULL, VIDEO_GAMES }, { st_update, NULL, VIDEO_GAMES },
    { st_matrix, NULL, VIDEO_GAMES }, { st_timestamp, NULL, VIDEO_GAMES }, { st_nothing, NULL, VIDEO_GAMES },
    { surface_new, NULL, VIDEO_GAMES }, { unity_show_video, NULL, "com.nekki.vector2" },
    { sega_android_id, NULL, "com.sega.sonicdash" }, { sega_ad_id, NULL, "com.sega.sonicdash" },
    { sega_storage, NULL, "com.sega.sonicdash" }, { sega_locale, NULL, "com.sega.sonicdash" },
    // Hardlight's own (HLUnityCore, its Google consent): both of its Sonic games
#define HARDLIGHT_GAMES "com.sega.sonicdash,com.sega.sonicboomandroid"
    { sega_connected, NULL, "com.sega.sonicdash" }, { hl_country, NULL, HARDLIGHT_GAMES },
    { hl_dp_to_px, NULL, HARDLIGHT_GAMES }, { hl_cmp_init, NULL, HARDLIGHT_GAMES },
    { hl_cmp_request, NULL, HARDLIGHT_GAMES }, { hl_cmp_status, NULL, HARDLIGHT_GAMES },
    { hl_cmp_complete, NULL, HARDLIGHT_GAMES }, { hl_language, NULL, HARDLIGHT_GAMES },
    { boom_locale, NULL, "com.sega.sonicboomandroid" }, { boom_locale_text, NULL, "com.sega.sonicboomandroid" },
    { boom_country, NULL, "com.sega.sonicboomandroid" }, { boom_id_get, NULL, "com.sega.sonicboomandroid" },
    { boom_id_set, NULL, "com.sega.sonicboomandroid" },
    { uno_free_mb, NULL, "com.matteljv.uno" }, { uno_uuid_load, NULL, "com.matteljv.uno" },
    { uno_uuid_save, NULL, "com.matteljv.uno" }, { uno_opened_load, NULL, "com.matteljv.uno" },
    { uno_opened_save, NULL, "com.matteljv.uno" }, { uno_country, NULL, "com.matteljv.uno" },
    { dash_id_get, NULL, "com.sega.sonicdash" }, { dash_id_set, NULL, "com.sega.sonicdash" },
    { sega_gp_auth, NULL, "com.sega.sonicdash" }, { sega_gp_no, NULL, "com.sega.sonicdash" },
#define KING "com.king.candycrushsaga"
    { king_anr_new, NULL, KING }, { king_anr_active, NULL, KING }, { king_anr_schedule, NULL, KING },
    { king_cache_dir, NULL, KING }, { king_home_dir, NULL, KING }, { king_shared_dir, NULL, KING },
    { king_language, NULL, KING }, { king_country, NULL, KING }, { king_version, NULL, KING },
    { king_res_scale, NULL, KING }, { object_load_class, NULL, KING },
};
static int not_for_this_game(const jmeth *m) {
    for (size_t i = 0; i < sizeof(per_game) / sizeof(per_game[0]); i++)
        if (m->fn == per_game[i].fn && (!per_game[i].sig_has || strstr(m->sig, per_game[i].sig_has)))
            return !droid_game_in(per_game[i].games);
    return 0;
}

jmethodID java_method(jclass cls, const char *name, const char *sig) {
    const char *cname = !placeholder(cls) ? (const char *)cls : NULL;
    if (cname && as_obj((jobject)cls)) cname = as_obj((jobject)cls)->cls;
    for (size_t pass = 0; pass < 2; pass++) // the class's own first, then by name + signature
        for (size_t i = 0; i < N_METHODS; i++) {
            const jmeth *m = &methods[i];
            if (strcmp(m->name, name) != 0 || !sig_eq(m->sig, sig)) continue;
            // Unreal's answer (/sdcard); every other game keeps FalsoJNI's, as before
            if (m->fn == env_ext_storage && !(droid_quirks & QUIRK_UNREAL)) continue;
            if (not_for_this_game(m)) continue;
            if (pass == 0 ? (cname && m->cls && class_matches(m->cls, cname)) : (!m->cls || !cname))
                return (jmethodID)m;
        }
    // Unity's ReflectionHelper from C#: the return type written as Object ("...)Ljava/lang/Object;")
    // for a method returning some class (Angry Birds 2: Task.addOnSuccessListener went to the empty
    // stub, its listener never ran). Only when nothing matched above: the same arguments, any
    // object result, the class's own entry.
    const char *ret = strrchr(sig, ')');
    if (cname && ret && !strcmp(ret, ")Ljava/lang/Object;")) {
        size_t args = (size_t)(ret - sig) + 1; // "(...)"
        for (size_t i = 0; i < N_METHODS; i++) {
            const jmeth *m = &methods[i];
            if (strcmp(m->name, name) != 0 || !m->cls || !class_matches(m->cls, cname) || not_for_this_game(m)) continue;
            const char *mret = strrchr(m->sig, ')');
            if (!mret || (size_t)(mret - m->sig) + 1 != args || (mret[1] != 'L' && mret[1] != '[')) continue;
            char head[256];
            if (args >= sizeof(head)) continue;
            memcpy(head, m->sig, args);
            head[args] = 0;
            char want[256];
            memcpy(want, sig, args);
            want[args] = 0;
            if (sig_eq(head, want)) return (jmethodID)m;
        }
    }
    return NULL;
}

// the APK scanner's list (tools/apk_scan.py): every Java method we answer, "class name sig"
void java_known_dump(FILE *f) {
    for (size_t i = 0; i < N_METHODS; i++)
        fprintf(f, "%s %s %s\n", methods[i].cls ? methods[i].cls : "*", methods[i].name, methods[i].sig);
}

int java_is_ours(jmethodID id) {
    return ((const jmeth *)id >= methods && (const jmeth *)id < methods + N_METHODS) || is_dynamic(id);
}

const char *java_sig(jmethodID id) {
    if (is_dynamic(id)) return ((const dyn_method *)id)->sig;
    return java_is_ours(id) ? ((const jmeth *)id)->sig : NULL;
}

// The Java calls in progress, a slot a thread: a game thread stuck in one of our answers (the
// thread report says "outside ARM code: in our C code", no calls) names it (java_busy_report)
#define BUSY_SLOTS 64
static struct { pthread_t th; const char *cls, *name, *sig; int64_t since; } busy[BUSY_SLOTS];
static pthread_mutex_t busy_lock = PTHREAD_MUTEX_INITIALIZER;
// each thread's own slot, taken once (no lock per call); only its outermost call counts (an
// answer calling back into Java stays the one shown)
static _Thread_local int busy_slot = -2, busy_depth;
static int busy_enter(const char *cls, const char *name, const char *sig) {
    if (busy_slot == -2) {
        busy_slot = -1;
        pthread_mutex_lock(&busy_lock);
        for (int i = 0; i < BUSY_SLOTS && busy_slot < 0; i++)
            if (!busy[i].th) busy[i].th = pthread_self(), busy_slot = i;
        pthread_mutex_unlock(&busy_lock);
    }
    if (busy_slot < 0 || busy_depth++) return -1;
    busy[busy_slot].cls = cls, busy[busy_slot].name = name, busy[busy_slot].sig = sig;
    __atomic_store_n(&busy[busy_slot].since, now_ns(), __ATOMIC_RELEASE);
    return busy_slot;
}
static void busy_leave(int slot) {
    if (busy_slot >= 0) busy_depth--;
    if (slot >= 0) __atomic_store_n(&busy[slot].since, 0, __ATOMIC_RELEASE);
}
void java_busy_report(void) { // (unity_host.c's thread report): the calls running for over 2 s
    int64_t t = now_ns();
    for (int i = 0; i < BUSY_SLOTS; i++) {
        const int64_t since = __atomic_load_n(&busy[i].since, __ATOMIC_ACQUIRE);
        if (since && t - since > 2000000000LL)
            droid_log("java: STILL IN %s.%s%s for %.1f s [pthread %p]", busy[i].cls ? busy[i].cls : "?", busy[i].name,
                      busy[i].sig ? busy[i].sig : "", (t - since) / 1e9, (void *)busy[i].th);
    }
}

jvalue java_call(jmethodID id, jobject self, const jvalue *args) {
    if (is_dynamic(id)) {
        const int slot = busy_enter("(dynamic)", "call", "");
        jvalue r = dyn_call((const dyn_method *)id, self, args);
        busy_leave(slot);
        return r;
    }
    static int traced;
    const jmeth *m = (const jmeth *)id;
    static int input_traced; // the input classes: what Unity asks of an event
    int input_cls = m->cls && (!strncmp(m->cls, "android/view/MotionEvent", 24) || !strncmp(m->cls, "android/view/KeyEvent", 21) ||
                               !strncmp(m->cls, "android/view/Input", 18));
    // a touch event's calls apart (the pad's events used the 120 up while a game loaded)
    static int touch_traced;
    const motion_data *tm = input_cls ? motion_of(self) : NULL;
    if (tm && tm->source == 0x1002 && touch_traced < 200) {
        touch_traced++;
        droid_log("java: touch call %s.%s%s (action %d)", m->cls, m->name, m->sig, tm->action);
    } else if (input_cls && input_traced < 120) {
        input_traced++;
        droid_log("java: input call %s.%s%s", m->cls, m->name, m->sig);
    } else if (traced < 300) {
        traced++;
        droid_log("java: call %s.%s%s self %p arg0 %#llx", m->cls ? m->cls : "*", m->name, m->sig, self,
                  (unsigned long long)(args ? args[0].j : 0));
    }
    const int slot = busy_enter(m->cls, m->name, m->sig);
    jvalue r = m->fn(self, args);
    busy_leave(slot);
    return r;
}

int java_is_object(jobject o) { return as_obj(o) != NULL; }

// Our objects: their class. A class: java/lang/Class. FalsoJNI's objects
// (strings naming what they stand for, e.g. "com/unity3d/player/UnityPlayerActivity"):
// that name, or java/lang/Object for "file:..." and other non-class strings.
jclass java_class_of(jobject o) {
    if (placeholder(o)) return find_class("java/lang/Object");
    jobj *j = as_obj(o);
    if (j) return find_class(j->cls);
    if (java_is_string(o)) return find_class("java/lang/String");
    if (is_class(o)) return find_class("java/lang/Class");
    const char *s = (const char *)o;
    size_t n = strnlen(s, 200);
    int looks_like_class = n > 2 && n < 200 && strchr(s, '/') && !strchr(s, ':') && !strchr(s, ' ');
    return find_class(looks_like_class ? s : "java/lang/Object");
}

jboolean java_instance_of(jobject o, jclass cls) {
    jobj *j = as_obj(o);
    if (!o) return JNI_FALSE;
    const char *want = !placeholder(cls) ? (const char *)cls : NULL;
    if (!j) return JNI_TRUE; // FalsoJNI's objects: Context / Activity checks pass
    if (!want || strcmp(want, "java/lang/Object") == 0) return JNI_TRUE;
    if (strcmp(j->cls, "java/lang/reflect/Proxy") == 0) return JNI_TRUE; // it implements what it was made for
    return class_matches(want, j->cls) ? JNI_TRUE : JNI_FALSE;
}

// Per-object fields of our classes (FalsoJNI keeps fields per name, for everyone).
static const struct {
    const char *cls, *name;
    int slot;
} our_fields[] = {
    { "android/os/Message", "what", 0 },
    { "android/os/Message", "arg1", 1 },
    { "android/os/Message", "arg2", 2 },
    { "android/os/Message", "obj", 3 },
    { "com/epicgames/ue4/GameActivity", "VersionCode", 4 },
    { "com/epicgames/ue4/PackageInfo", "versionCode", 5 },
    { "com/epicgames/ue4/PackageInfo", "versionName", 6 },
    { "com/epicgames/ue4/PackageInfo", "packageName", 7 },
    { "com/epicgames/ue4/ApplicationInfo", "packageName", 8 },
    { "com/halfbrick/mortar/HttpClient", "IsFinished", 0 },
    { "com/halfbrick/mortar/HttpClient", "RequestPointer", 1 },
    { "com/halfbrick/mortar/HttpClient", "Result", 2 },
    // the snapshot open result (all zero: a failure): isSuccess, data, snapshot, conflict, conflicting*Data
    { "com/halfbrick/mortar/SnapshotOpenResult", "isSuccess", 0 },
    { "com/halfbrick/mortar/SnapshotOpenResult", "data", 1 },
    { "com/halfbrick/mortar/SnapshotOpenResult", "snapshot", 2 },
    { "com/halfbrick/mortar/SnapshotOpenResult", "conflict", 3 },
    { "com/halfbrick/mortar/SnapshotOpenResult", "conflictingServerData", 1 },
    { "com/halfbrick/mortar/SnapshotOpenResult", "conflictingLocalData", 1 },
    { "com/halfbrick/mortar/AchievementsLoadResult", "isSuccessful", 0 },
    { "com/halfbrick/mortar/AchievementsLoadResult", "achievements", 1 },
};
#define N_FIELDS (sizeof(our_fields) / sizeof(our_fields[0]))

jfieldID java_field(jclass cls, const char *name) {
    const char *cname = !placeholder(cls) ? (const char *)cls : NULL;
    if (!cname) return NULL;
    for (size_t i = 0; i < N_FIELDS; i++)
        if (strcmp(our_fields[i].cls, cname) == 0 && strcmp(our_fields[i].name, name) == 0)
            return (jfieldID)&our_fields[i];
    return NULL;
}

int java_is_our_field(jfieldID f) {
    return (const void *)f >= (const void *)our_fields && (const void *)f < (const void *)(our_fields + N_FIELDS);
}

jvalue java_get_field(jobject o, jfieldID f) {
    jobj *j = as_obj(o);
    jvalue zero;
    zero.j = 0;
    const __typeof__(our_fields[0]) *ff = (const __typeof__(our_fields[0]) *)f;
    if (!strncmp(ff->cls, "com/epicgames/", 14) && (!strcmp(ff->name, "VersionCode") || !strcmp(ff->name, "versionCode"))) { zero.i = droid_java_version_code; return zero; }
    if (!strncmp(ff->cls, "com/epicgames/", 14) && !strcmp(ff->name, "versionName")) { zero.l = jstr(droid_java_version_name ? droid_java_version_name : ""); return zero; }
    if (!strncmp(ff->cls, "com/epicgames/", 14) && !strcmp(ff->name, "packageName")) { zero.l = jstr(java_package ? java_package : ""); return zero; }
    int slot = ff->slot;
    return j ? j->field[slot] : zero;
}

void java_set_field(jobject o, jfieldID f, jvalue v) {
    jobj *j = as_obj(o);
    if (j) j->field[((const __typeof__(our_fields[0]) *)f)->slot] = v;
}

// Arguments from a signature: "(Ljava/lang/String;IJ)V" -> classes in order.
int java_arg_kinds(const char *sig, char *kinds, int max) {
    int n = 0;
    const char *s = sig ? strchr(sig, '(') : NULL;
    if (!s) return 0;
    for (s++; *s && *s != ')' && n < max;) {
        char k = *s;
        if (*s == '[') {
            while (*s == '[') s++;
            if (*s == 'L') s = strchr(s, ';');
            if (!s) break;
            s++;
            kinds[n++] = 'L';
            continue;
        }
        if (k == 'L') {
            s = strchr(s, ';');
            if (!s) break;
            s++;
            kinds[n++] = 'L';
            continue;
        }
        kinds[n++] = k;
        s++;
    }
    return n;
}
