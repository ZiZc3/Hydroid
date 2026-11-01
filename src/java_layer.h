#ifndef JAVA_LAYER_H
#define JAVA_LAYER_H

#include "FalsoJNI.h"

// Methods of our Java classes (StringBuilder, Class, InputStream...), found
// by class + name + signature. NULL: not ours (FalsoJNI answers it).
jmethodID java_method(jclass cls, const char *name, const char *sig);
int java_is_ours(jmethodID id);
const char *java_sig(jmethodID id);
jvalue java_call(jmethodID id, jobject self, const jvalue *args);

int java_is_object(jobject o);       // one of our objects (not a FalsoJNI string)
int java_is_string(jobject o);       // a FalsoJNI java.lang.String
const char *java_text(jobject o);    // a String's text (or a raw C string's)
jclass java_class_of(jobject o);     // its class (never a placeholder)
void java_note_class(jclass c);      // c is a class handle (from FindClass)
jboolean java_instance_of(jobject o, jclass cls);

// Interface methods (callbacks, listeners, Runnable): an ID even without code; a call
// on one of Unity's proxies goes to the proxy. NULL for other classes.
jmethodID java_dynamic_method(jclass cls, const char *name, const char *sig);
jmethodID java_stub_method(jclass cls, const char *name, const char *sig); // no code: answers 0 / null
jmethodID java_from_reflected(jobject method); // our Method object -> its ID, or NULL
jfieldID java_from_reflected_field(jobject field); // our Field object -> its ID, or NULL
jobject java_to_reflected(jmethodID id);       // an interface method's one Method object, or NULL

// Per-object fields of our classes (Message.what ...). NULL / 0: FalsoJNI's.
jfieldID java_field(jclass cls, const char *name);
int java_is_our_field(jfieldID f);
jvalue java_get_field(jobject o, jfieldID f);
void java_set_field(jobject o, jfieldID f, jvalue v);

// The argument kinds of a method signature ('I', 'J', 'F', 'D', 'Z', 'L', ...).
int java_arg_kinds(const char *sig, char *kinds, int max);

// Input events for nativeInjectEvent: MotionEvent (pointers x/y, or joystick axes[48]) and KeyEvent.
jobject java_motion_event(int action, int source, int device, int count, const int *ids, const float *x,
                          const float *y, const float *axes, int64_t time_ns, int64_t down_ns);
jobject java_key_event(int action, int keycode, int source, int device, int repeat, int64_t time_ns,
                       int64_t down_ns);

// java.nio direct ByteBuffers (address + capacity).
jobject java_new_direct_buffer(void *address, int64_t capacity);
void *java_direct_address(jobject o);
int64_t java_direct_capacity(jobject o);

void java_free_event(jobject o); // a MotionEvent / KeyEvent we made

// A loaded library's JNI_OnLoad, run once (System.load, and Unity's libmain)

int java_jni_onload(void *so_module_ptr); // an so_module *

jobject java_new_string(const char *s); // a Java String object from a C string

#endif
