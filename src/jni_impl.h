#ifndef JNI_IMPL_H
#define JNI_IMPL_H

// FalsoJNI set up, with our RegisterNatives in its table.
void jni_setup(void);

// What the fake Context answers: package name, base APK path, the folder
// the APK's libraries are copied to, split APK paths, the Activity object.
void jni_set_version(const char *name, long code); // the manifest's versionName / versionCode
void jni_set_app(const char *package, const char *apk_path, const char *lib_dir,
                 const char *const *splits, int split_count, void *activity);

// A function the game registered with RegisterNatives, by its Java name.
void *jni_find_native(const char *name);

#endif
