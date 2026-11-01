#ifndef ARM_MODULES_H
#define ARM_MODULES_H

#include <stdint.h>

#include "soloader.h"

// Copies the APK library named name (e.g. "libunity.so") to dest; 0 = ok,
// nonzero = the APK has no such library.
typedef int (*arm_extract_fn)(const char *name, const char *dest);

// dir: where the APK's libraries are copied before loading.
void arm_modules_init(const char *dir, arm_extract_fn extract);

// Loads one of the APK's ARM64 libraries by name (and its DT_NEEDED ones
// first), runs its constructors in the JIT. NULL if the APK doesn't have it.
so_module *arm_module_load(const char *name);

so_module *arm_module_at(uint64_t addr, const char **name, const char **path);
int arm_module_count(void);
so_module *arm_module(int i);
const char *arm_module_path(int i);

// dlopen/dlsym/dlclose/dlerror/dladdr/dl_iterate_phdr for ARM code, or NULL.
void *arm_modules_resolve_dl(const char *name);

#endif
