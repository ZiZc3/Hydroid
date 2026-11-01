#ifndef SOLOADER_H
#define SOLOADER_H

#include <stdint.h>

typedef struct {
    void *base;
    uint64_t size;
    void *entry;
    int64_t phys;
    int arm64; // an ARM64 library: its code runs in the JIT (arm_cpu)
    // An ARM64 library's constructors (DT_INIT, then all of DT_INIT_ARRAY),
    // for the caller to run in the JIT; an x86-64 library's have already run.
    uint64_t *ctors;
    int ctor_count;
    // DT_INIT and DT_INIT_ARRAY as loaded, so a packer's rewritten array can
    // be read again after its DT_INIT runs.
    uint64_t init_fn;
    uint64_t init_array;
    int init_array_count;
    // For dl_iterate_phdr / dladdr: load bias (base - lowest vaddr), the
    // program headers in memory.
    uint64_t bias;
    const void *phdr;
    int phnum;
} so_module;

// The DT_NEEDED names of an ELF .so file (before loading it). Returns the count.
int so_needed(const char *path, char names[][64], int max);

// Load an x86_64 or AArch64 ELF .so from a file path into memory: PT_LOAD
// segments, relocations, imports through so_import_resolver. Returns 0 on
// success and fills mod.
int so_load(const char *path, so_module *mod);

// Find an exported symbol by name. Returns NULL if not found.
void *so_sym(so_module *mod, const char *name);

// Imports the .so needs from other libraries (libandroid, libaaudio, libc):
// the host answers by name. Unknown names call a stub returning 0.
extern void *(*so_import_resolver)(const char *name);
extern void (*so_missing_import)(const char *name);
// For ARM64 libraries: is_data = the symbol is an object, not a function.
extern void *(*so_arm_import_resolver)(const char *name, int is_data);
// An ARM library's own definition of a symbol, bound elsewhere instead (non-NULL: there) - the
// operator new / delete an Unreal game's libraries share with libUE4.so, as Android's linker binds them
extern void *(*so_arm_interpose)(const char *name);

// Unload: unmap memory.
void so_unload(so_module *mod);

#endif
