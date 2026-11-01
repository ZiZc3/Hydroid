// ---- ART mode (Java mode, PC for now): the ARM code's JNI goes to ART's real JNIEnv ----
// Included by arm_imports.c. ART's own table, thunked; only what the ARM ABI changes is wrapped:
// varargs and ARM va_lists (the method's signature, seen at GetMethodID, sorts the arguments),
// RegisterNatives (x86 entries into dynarmic), the VM. The env/vm pointers swap at the border
// both ways (arm_arg0_hooks, arm_cpu.cpp).

static JavaVM *art_vm;
static __thread JNIEnv *art_env_tls;
static int jni_ready;

static JNIEnv *art_env(void) {
    if (art_env_tls) return art_env_tls;
    JNIEnv *e = NULL;
    if ((*art_vm)->GetEnv(art_vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK)
        (*art_vm)->AttachCurrentThread(art_vm, &e, NULL);
    return art_env_tls = e;
}
#define AE art_env()
#define AH (*AE)

#define ART_JNI_T(N, T)                                                                              \
    static T art_Call##N##Method(JNIEnv *e, jobject o, jmethodID m, ...) {                         \
        (void)e;                                                                                     \
        va_list ap;                                                                                  \
        va_start(ap, m);                                                                             \
        T r = AH->Call##N##MethodV(AE, o, m, ap);                                                    \
        va_end(ap);                                                                                  \
        return r;                                                                                    \
    }                                                                                                \
    static T art_Call##N##MethodV(JNIEnv *e, jobject o, jmethodID m, const void *ap) {               \
        (void)e;                                                                                     \
        x86_va x;                                                                                    \
        return AH->Call##N##MethodV(AE, o, m, convert_sig(&x, m, ap));                               \
    }                                                                                                \
    static T art_CallStatic##N##Method(JNIEnv *e, jclass c, jmethodID m, ...) {                    \
        (void)e;                                                                                     \
        va_list ap;                                                                                  \
        va_start(ap, m);                                                                             \
        T r = AH->CallStatic##N##MethodV(AE, c, m, ap);                                              \
        va_end(ap);                                                                                  \
        return r;                                                                                    \
    }                                                                                                \
    static T art_CallStatic##N##MethodV(JNIEnv *e, jclass c, jmethodID m, const void *ap) {          \
        (void)e;                                                                                     \
        x86_va x;                                                                                    \
        return AH->CallStatic##N##MethodV(AE, c, m, convert_sig(&x, m, ap));                         \
    }                                                                                                \
    static T art_CallNonvirtual##N##Method(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) {     \
        (void)e;                                                                                     \
        va_list ap;                                                                                  \
        va_start(ap, m);                                                                             \
        T r = AH->CallNonvirtual##N##MethodV(AE, o, c, m, ap);                                       \
        va_end(ap);                                                                                  \
        return r;                                                                                    \
    }                                                                                                \
    static T art_CallNonvirtual##N##MethodV(JNIEnv *e, jobject o, jclass c, jmethodID m,             \
                                            const void *ap) {                                        \
        (void)e;                                                                                     \
        x86_va x;                                                                                    \
        return AH->CallNonvirtual##N##MethodV(AE, o, c, m, convert_sig(&x, m, ap));                  \
    }
ART_JNI_T(Object, jobject)
ART_JNI_T(Boolean, jboolean)
ART_JNI_T(Byte, jbyte)
ART_JNI_T(Char, jchar)
ART_JNI_T(Short, jshort)
ART_JNI_T(Int, jint)
ART_JNI_T(Long, jlong)
ART_JNI_T(Float, jfloat)
ART_JNI_T(Double, jdouble)

static void art_CallVoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    AH->CallVoidMethodV(AE, o, m, ap);
    va_end(ap);
}
static void art_CallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, const void *ap) {
    (void)e;
    x86_va x;
    AH->CallVoidMethodV(AE, o, m, convert_sig(&x, m, ap));
}
static void art_CallStaticVoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    AH->CallStaticVoidMethodV(AE, c, m, ap);
    va_end(ap);
}
static void art_CallStaticVoidMethodV(JNIEnv *e, jclass c, jmethodID m, const void *ap) {
    (void)e;
    x86_va x;
    AH->CallStaticVoidMethodV(AE, c, m, convert_sig(&x, m, ap));
}
static void art_CallNonvirtualVoidMethod(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    AH->CallNonvirtualVoidMethodV(AE, o, c, m, ap);
    va_end(ap);
}
static void art_CallNonvirtualVoidMethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, const void *ap) {
    (void)e;
    x86_va x;
    AH->CallNonvirtualVoidMethodV(AE, o, c, m, convert_sig(&x, m, ap));
}
static jobject art_NewObject(JNIEnv *e, jclass c, jmethodID m, ...) {
    (void)e;
    va_list ap;
    va_start(ap, m);
    jobject r = AH->NewObjectV(AE, c, m, ap);
    va_end(ap);
    return r;
}
static jobject art_NewObjectV(JNIEnv *e, jclass c, jmethodID m, const void *ap) {
    (void)e;
    x86_va x;
    return AH->NewObjectV(AE, c, m, convert_sig(&x, m, ap));
}
// HYDROID_ART_JNILOG: every Java method the game's ARM code looks up (what it asks the framework)
static int jni_log = -1;
static void log_lookup(const char *kind, const char *name, const char *sig, jmethodID id) {
    if (jni_log < 0) jni_log = getenv("HYDROID_ART_JNILOG") != NULL;
    if (jni_log) droid_log("art: game looks up %s %s%s%s", kind, name, sig, id ? "" : " (missing)");
}

static jmethodID art_GetMethodID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e;
    jmethodID id = AH->GetMethodID(AE, c, name, sig);
    log_lookup("method", name, sig, id);
    remember_sig(id, sig);
    return id;
}
static jmethodID art_GetStaticMethodID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e;
    jmethodID id = AH->GetStaticMethodID(AE, c, name, sig);
    log_lookup("static", name, sig, id);
    remember_sig(id, sig);
    return id;
}
static jint art_RegisterNatives(JNIEnv *e, jclass c, const JNINativeMethod *m, jint n) {
    (void)e;
    JNINativeMethod *copy = malloc((size_t)(n > 0 ? n : 1) * sizeof(*copy));
    if (!copy) return JNI_ERR;
    for (jint i = 0; i < n; i++) {
        copy[i] = m[i];
        copy[i].fnPtr = m[i].fnPtr ? arm_callback((uint64_t)m[i].fnPtr) : NULL;
    }
    jint r = AH->RegisterNatives(AE, c, copy, n);
    free(copy); // ART keeps the entry points, not the array
    return r;
}
static jint art_GetJavaVM(JNIEnv *e, JavaVM **vm) {
    (void)e;
    *vm = &arm_vm_obj;
    return JNI_OK;
}
static jint art_AttachCurrentThread(JavaVM *vm, JNIEnv **env, void *args) {
    (void)vm; (void)args;
    art_env(); // a game thread joins ART
    *env = &arm_env_obj;
    return JNI_OK;
}
static jint art_GetEnv(JavaVM *vm, void **env, jint version) {
    (void)vm;
    if (!env) return JNI_EINVAL;
    JNIEnv *e = NULL;
    if ((*art_vm)->GetEnv(art_vm, (void **)&e, version) != JNI_OK) {
        *env = NULL;
        return JNI_EDETACHED;
    }
    *env = &arm_env_obj;
    return JNI_OK;
}
static jint art_DetachCurrentThread(JavaVM *vm) {
    (void)vm;
    art_env_tls = NULL;
    return (*art_vm)->DetachCurrentThread(art_vm);
}

// The border: ARM -> host, our env/vm become ART's; host -> ARM, ART's become ours.
static uint64_t art_arg0_to_host(uint64_t x0) {
    if (x0 == (uint64_t)&arm_env_obj) return (uint64_t)art_env();
    if (x0 == (uint64_t)&arm_vm_obj) return (uint64_t)art_vm;
    return x0;
}
static uint64_t art_arg0_to_guest(uint64_t x0) {
    if (!x0) return x0;
    if (x0 == (uint64_t)art_vm) return (uint64_t)&arm_vm_obj;
    if (art_env_tls && x0 == (uint64_t)art_env_tls) return (uint64_t)&arm_env_obj;
    JNIEnv *e = NULL;
    if ((*art_vm)->GetEnv(art_vm, (void **)&e, JNI_VERSION_1_6) == JNI_OK && x0 == (uint64_t)e) {
        art_env_tls = e;
        return (uint64_t)&arm_env_obj;
    }
    return x0;
}

#define ART_SET_V(N)                                                                    \
    SET(arm_jni_iface, Call##N##Method, art_Call##N##Method);                           \
    SET(arm_jni_iface, Call##N##MethodV, art_Call##N##MethodV);                         \
    SET(arm_jni_iface, CallNonvirtual##N##Method, art_CallNonvirtual##N##Method);       \
    SET(arm_jni_iface, CallNonvirtual##N##MethodV, art_CallNonvirtual##N##MethodV);     \
    SET(arm_jni_iface, CallStatic##N##Method, art_CallStatic##N##Method);               \
    SET(arm_jni_iface, CallStatic##N##MethodV, art_CallStatic##N##MethodV)

// Called once, before any ARM library loads, when Java mode runs on ART.
void arm_jni_setup_art(JavaVM *vm) {
    if (jni_ready) return;
    jni_ready = 1;
    art_vm = vm;
    arm_jni_iface = *AH;
    ART_SET_V(Object); ART_SET_V(Boolean); ART_SET_V(Byte); ART_SET_V(Char); ART_SET_V(Short);
    ART_SET_V(Int); ART_SET_V(Long); ART_SET_V(Float); ART_SET_V(Double); ART_SET_V(Void);
    SET(arm_jni_iface, NewObject, art_NewObject);
    SET(arm_jni_iface, NewObjectV, art_NewObjectV);
    SET(arm_jni_iface, GetMethodID, art_GetMethodID);
    SET(arm_jni_iface, GetStaticMethodID, art_GetStaticMethodID);
    SET(arm_jni_iface, RegisterNatives, art_RegisterNatives);
    SET(arm_jni_iface, GetJavaVM, art_GetJavaVM);
    thunk_all(&arm_jni_iface, sizeof(arm_jni_iface), 4, "jni");

    arm_vm_iface = **vm;
    SET(arm_vm_iface, AttachCurrentThread, art_AttachCurrentThread);
    SET(arm_vm_iface, AttachCurrentThreadAsDaemon, art_AttachCurrentThread);
    SET(arm_vm_iface, GetEnv, art_GetEnv);
    SET(arm_vm_iface, DetachCurrentThread, art_DetachCurrentThread);
    thunk_all(&arm_vm_iface, sizeof(arm_vm_iface), 3, "jvm");

    arm_arg0_hooks(art_arg0_to_host, art_arg0_to_guest);
}
