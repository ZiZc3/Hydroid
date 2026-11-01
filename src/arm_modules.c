// arm_modules.c — the APK's ARM64 libraries as a dynamic linker sees them:
// loaded by name on demand (their DT_NEEDED first, constructors in the JIT),
// resolving each other before us, and dlopen / dlsym / dladdr /
// dl_iterate_phdr for code that loads libraries itself (Unity loads
// libunity and libil2cpp that way; the GC and C++ unwinder walk the list).

#include "arm_modules.h"
#include "vulkan_bridge.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "arm_cpu.h"

void *linux_abi_resolve(const char *name); // linux_abi.c (sockets first: net_shim.c)
void arm_trace_module(const char *lib, uint64_t base, void *mod); // arm_trace.c (XTool)
void astc_native_attach(const char *lib, uint64_t base, size_t size); // astc_native.cpp
void etc_native_attach(const char *lib, uint64_t base, size_t size);  // etc_native.cpp

#define MOD_MAX 96

typedef struct {
    so_module m;
    char name[96];
    char path[256];
    int fd; // the file the library came from (MADV_DONTNEED restores its bytes)
    int load_n;
    struct { uint64_t vaddr, off, filesz; } load[24];
} arm_mod;

static arm_mod mods[MOD_MAX];
static int mod_count;
static char lib_dir[256];
static arm_extract_fn extract;
static pthread_mutex_t lock;
static __thread const char *dl_error;

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void arm_modules_init(const char *dir, arm_extract_fn fn) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); // constructors may dlopen
    pthread_mutex_init(&lock, &a);
    snprintf(lib_dir, sizeof(lib_dir), "%s", dir);
    extract = fn;
}

#ifdef DROID_PC
// PC diagnostic: is a library's DT_INIT a pure 16 KB page permutation or real decryption?
#define UE4_BLK 0x4000
static uint64_t ue4_hash(const void *p, size_t n) {
    const unsigned char *s = p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ULL; }
    return h;
}
static int ue4_zero_block(const void *p, size_t n) {
    const uint64_t *q = p;
    for (size_t i = 0; i < n / 8; i++)
        if (q[i]) return 0;
    return 1;
}
#endif

static so_module *find(const char *name) {
    for (int i = 0; i < mod_count; i++)
        if (strcmp(mods[i].name, name) == 0) return &mods[i].m;
    return NULL;
}

// Android loads a library with mmap(MAP_PRIVATE) from its file. MADV_DONTNEED on such a page
// throws the process's private (changed) copy away; the next read faults the page in from the
// file again. Our ARM libraries are copies in direct memory, so do that by hand. Returns 1
// when the range was inside a module (the caller must then not zero it).
int arm_module_madvise(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    int handled = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < mod_count; i++) {
        arm_mod *am = &mods[i];
        uintptr_t b = (uintptr_t)am->m.base, end = b + am->m.size;
        uintptr_t s = a > b ? a : b, t = e < end ? e : end;
        if (s >= t) continue;
        handled = 1;
        memset((void *)s, 0, t - s); // BSS, gaps, and a failed read: zero
        if (am->fd < 0) continue;
        for (int k = 0; k < am->load_n; k++) {
            uintptr_t ls = (uintptr_t)am->m.bias + am->load[k].vaddr;
            uintptr_t le = ls + am->load[k].filesz;
            uintptr_t x = s > ls ? s : ls, y = t < le ? t : le;
            if (x >= y) continue;
            size_t n = y - x;
            ssize_t got = pread(am->fd, (void *)x, n, (off_t)(am->load[k].off + (x - ls)));
            if (got > 0 && (size_t)got < n) memset((void *)(x + (size_t)got), 0, n - (size_t)got);
        }
    }
    pthread_mutex_unlock(&lock);
    return handled;
}

// Unreal games: operator new / delete bound as Android's linker does. libUE4.so defines them (its
// FMalloc); the libraries it needs come first in its lookup group on a phone, so libc++_shared's
// own new (a std::string grown in it) is libUE4's too. Loaded before libUE4 here, they had their
// own: UE then freed what its FMalloc never made ("FMallocBinned2 Attempt to free an unrecognized
// block"). Every other library's new / delete (imports and own ones) goes through a stub: the
// host's until libUE4 is loaded, libUE4's after.
static const char *const ue_alloc_names[] = { "_Znwm", "_Znam", "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t",
                                              "_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm", "_ZdlPvRKSt9nothrow_t",
                                              "_ZdaPvRKSt9nothrow_t", NULL };
typedef struct {
    uint32_t code[4];  // ldr x16, slot pointer; ldr x16, [x16]; br x16; nop
    uint64_t slot_at;  // -> ue_slots[i]
} ue_stub;
static ue_stub *ue_stubs;
static volatile uint64_t ue_slots[16];
void *droid_jit_alloc(size_t size); // arm_cpu.cpp: memory ARM code runs from
static int ue_alloc_index(const char *name) {
    for (int i = 0; ue_alloc_names[i]; i++)
        if (!strcmp(ue_alloc_names[i], name)) return i;
    return -1;
}
static void *ue_alloc_stub(const char *name) {
    int i = ue_alloc_index(name);
    if (i < 0) return NULL;
    if (!ue_stubs) { // (cleared: the console's direct memory comes with old contents, the PC's as zeros;
                     // a stale slot_at left the stubs unwritten and libUE4's constructors stopped)
        if (!(ue_stubs = droid_jit_alloc(0x10000))) return NULL;
        memset(ue_stubs, 0, 0x10000);
    }
    ue_stub *s = &ue_stubs[i];
    if (!s->slot_at) {
        if (!ue_slots[i]) ue_slots[i] = (uint64_t)(uintptr_t)arm_import(name, 0); // the host's for now
        s->code[0] = 0x58000090; // ldr x16, #16 (slot_at)
        s->code[1] = 0xF9400210; // ldr x16, [x16]
        s->code[2] = 0xD61F0200; // br x16
        s->code[3] = 0xD503201F; // nop
        s->slot_at = (uint64_t)(uintptr_t)&ue_slots[i];
    }
    return s;
}
static void *ue_interpose(const char *name) { return ue_alloc_stub(name); } // soloader: own definitions
static void ue_bind(so_module *ue) {
    int n = 0;
    for (int i = 0; ue_alloc_names[i]; i++) {
        void *p = so_sym(ue, ue_alloc_names[i]);
        if (p) {
            ue_slots[i] = (uint64_t)(uintptr_t)p;
            n++;
        }
    }
    droid_log("ue4: operator new / delete of the other libraries now libUE4's (%d bound)", n);
}

// soloader's resolver while loading: the libraries already loaded first.
static void *resolve(const char *name, int is_data) {
    if ((droid_quirks & QUIRK_UNREAL) && !is_data && name[0] == '_' && name[1] == 'Z') {
        void *stub = ue_alloc_stub(name);
        if (stub) return stub;
    }
    for (int i = 0; i < mod_count; i++) {
        void *p = so_sym(&mods[i].m, name);
        if (p) return p;
    }
    return arm_import(name, is_data);
}

// libraries never loaded: not the game's, added to a repack (the game never needs them)
static const char *const never_load[] = {
    "libagent.so", // Infatica's proxyware SDK (sells the device's connection): a GTA SA repack
    NULL,
};

so_module *arm_module_load(const char *name) {
    const char *base = base_name(name);
    for (int i = 0; never_load[i]; i++)
        if (!strcmp(base, never_load[i])) {
            static int told;
            if (!told++) droid_log("modules: %s never loaded (not the game's: proxyware)", base);
            return NULL;
        }
    // Unreal games: Tencent's ACE anti-cheat (libanogs.so) kills the game here (a bad free after
    // its server answers). Never loaded: libUE4's AnoSDK* imports become the "missing" stubs
    // (return 0: no report data, nothing to do).
    if ((droid_quirks & QUIRK_UNREAL) && !strcmp(base, "libanogs.so")) {
        static int told;
        if (!told++) droid_log("modules: %s never loaded (anti-cheat: AnoSDK calls answer 0)", base);
        return NULL;
    }
    pthread_mutex_lock(&lock);
    so_module *m = find(base);
    if (m || mod_count == MOD_MAX || !extract) {
        pthread_mutex_unlock(&lock);
        return m;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", lib_dir, base);
    if (extract(base, path) != 0) { // not one of the APK's libraries
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    char needed[64][64];
    int n = so_needed(path, needed, 64);
    if (!strcmp(base, "libUE4.so")) {
        char list[700] = "";
        for (int i = 0; i < n; i++) { strncat(list, needed[i], sizeof(list) - strlen(list) - 2); strcat(list, " "); }
        droid_log("ue4: needs %d: %s", n, list);
    }
    for (int i = 0; i < n; i++) arm_module_load(needed[i]);

    arm_mod *am = &mods[mod_count];
    void *(*prev)(const char *, int) = so_arm_import_resolver;
    so_arm_import_resolver = resolve;
    // (an Unreal game: every library but libUE4 binds its own new / delete to the stubs)
    int is_ue = !strcmp(base, "libUE4.so") || !strcmp(base, "libUnreal.so");
    so_arm_interpose = (droid_quirks & QUIRK_UNREAL) && !is_ue ? ue_interpose : NULL;
    int rc = so_load(path, &am->m);
    so_arm_interpose = NULL;
    so_arm_import_resolver = prev;
    if (rc == 0 && is_ue && (droid_quirks & QUIRK_UNREAL)) ue_bind(&am->m);
    if (rc != 0 || !am->m.arm64) {
        droid_log("modules: %s load fail rc=%d", base, rc);
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    snprintf(am->name, sizeof(am->name), "%s", base);
    snprintf(am->path, sizeof(am->path), "%s", path);
    mod_count++; // visible before its constructors run: they may dlsym it
    arm_trace_module(base, (uint64_t)(uintptr_t)am->m.base, &am->m); // the debug tool's list (arm_trace.c, XTool)
    astc_native_attach(base, (uint64_t)(uintptr_t)am->m.base, am->m.size); // Unity's ASTC decoder: ours
    etc_native_attach(base, (uint64_t)(uintptr_t)am->m.base, am->m.size);  // and its ETC2 / EAC one
    // (Unreal only: its packer's MADV_DONTNEED reads the file again; other games open nothing more)
    am->fd = (droid_quirks & QUIRK_UNREAL) ? open(path, O_RDONLY) : -1;
    am->load_n = 0;
    {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)am->m.phdr;
        for (int i = 0; ph && i < am->m.phnum && am->load_n < 24; i++)
            if (ph[i].p_type == PT_LOAD) {
                am->load[am->load_n].vaddr = ph[i].p_vaddr;
                am->load[am->load_n].off = ph[i].p_offset;
                am->load[am->load_n].filesz = ph[i].p_filesz;
                am->load_n++;
            }
    }
    int ok = 1;
    uint64_t t0 = __builtin_ia32_rdtsc();
    int ue4_probe = strcmp(base, "libUE4.so") == 0;
    void *ue4_entry = ue4_probe ? so_sym(&am->m, "ANativeActivity_onCreate") : NULL;
    uint32_t ue4_word = ue4_entry ? *(volatile uint32_t *)ue4_entry : 0;
#ifdef DROID_PC
    size_t ue4_nb = 0;
    uint64_t *ue4_before = NULL;
    if (ue4_probe && getenv("DROID_UE4_DEBUG")) {
        ue4_nb = am->m.size / UE4_BLK;
        ue4_before = malloc(ue4_nb * sizeof(uint64_t));
        for (size_t i = 0; i < ue4_nb; i++)
            ue4_before[i] = ue4_hash((char *)am->m.base + i * UE4_BLK, UE4_BLK);
    }
#endif
    if (ue4_probe) droid_log("ue4: probe entry %p = %08x", ue4_entry, ue4_word);
    int ctor_first = 0;
    if (ue4_probe && getenv("DROID_UE4_SKIP_INIT") && am->m.ctor_count > 0) {
        ctor_first = 1; // the packer's DT_INIT: it zeroes engine code here
        droid_log("ue4: skipping DT_INIT %#llx (DROID_UE4_SKIP_INIT)",
                  (unsigned long long)am->m.ctors[0]);
    }
    extern int droid_host_trace;
    int ctor_array_at = ((droid_quirks & QUIRK_UNREAL) && am->m.init_fn && am->m.ctor_count > 0 &&
                         am->m.ctors[0] == am->m.init_fn) ? 1 : 0;
    for (int c = ctor_first; c < am->m.ctor_count && ok; c++) {
        if (ue4_probe && c == 0 && getenv("DROID_TRACE_UE4")) droid_host_trace = 1;
        arm_call(am->m.ctors[c], NULL, 0, &ok);
        if (ue4_probe && c == 0) droid_host_trace = 0;
        if (c == 0 && ok && ctor_array_at && am->m.init_array && am->m.init_array_count > 0) {
            // A packer may move code and rewrite DT_INIT_ARRAY in memory after its DT_INIT.
            uint64_t *arr = (uint64_t *)am->m.init_array;
            int n = am->m.init_array_count;
            int changed = 0;
            for (int i = 0; i < n; i++)
                if (arr[i] != am->m.ctors[ctor_array_at + i]) changed++;
            if (ue4_probe)
                droid_log("ue4: init_array after DT_INIT: %d/%d changed, first %#llx %#llx %#llx %#llx",
                          changed, n, (unsigned long long)arr[0], (unsigned long long)arr[1],
                          (unsigned long long)arr[2], (unsigned long long)arr[3]);
            if (changed) {
                for (int i = 0; i < n; i++)
                    if (arr[i] && arr[i] != (uint64_t)-1)
                        am->m.ctors[ctor_array_at + i] = arr[i];
                if (ue4_probe)
                    droid_log("ue4: init_array refreshed from memory");
            }
        }
        if (ue4_probe && c == 0 && am->m.ctor_count > 1) {
            char *b = (char *)am->m.base;
            uint64_t base = (uint64_t)(uintptr_t)b;
            char *now = (char *)so_sym(&am->m, "ANativeActivity_onCreate");
            droid_log("ue4: after DT_INIT sym now %p (off %#llx), entry %08x %08x %08x",
                      now, (unsigned long long)((uint64_t)(uintptr_t)now - base),
                      *(volatile uint32_t *)ue4_entry, *(volatile uint32_t *)((char *)ue4_entry + 4),
                      *(volatile uint32_t *)((char *)ue4_entry + 8));
#ifdef DROID_PC
            if (ue4_before) {
                size_t cap = 1;
                while (cap < ue4_nb * 2) cap <<= 1;
                uint64_t *tab = calloc(cap, sizeof(uint64_t));
                for (size_t i = 0; i < ue4_nb; i++) {
                    uint64_t h = ue4_before[i];
                    if (!h) continue;
                    size_t k = (size_t)(h * 0x9E3779B97F4A7C15ULL) & (cap - 1);
                    while (tab[k]) k = (k + 1) & (cap - 1);
                    tab[k] = h;
                }
                size_t changed = 0, zeroed = 0, permuted = 0, fresh = 0;
                size_t ent = (size_t)((char *)ue4_entry - b) / UE4_BLK;
                uint64_t ent_new = 0, ent_old = ue4_before[ent];
                int ent_zero = 0;
                for (size_t i = 0; i < ue4_nb; i++) {
                    uint64_t h = ue4_hash(b + i * UE4_BLK, UE4_BLK);
                    if (i == ent) { ent_new = h; ent_zero = ue4_zero_block(b + i * UE4_BLK, UE4_BLK); }
                    if (h == ue4_before[i]) continue;
                    changed++;
                    if (ue4_zero_block(b + i * UE4_BLK, UE4_BLK)) { zeroed++; continue; }
                    size_t k = (size_t)(h * 0x9E3779B97F4A7C15ULL) & (cap - 1);
                    int hit = 0;
                    while (tab[k]) {
                        if (tab[k] == h) { hit = 1; break; }
                        k = (k + 1) & (cap - 1);
                    }
                    if (hit) permuted++; else fresh++;
                }
                int oldhits = 0; size_t oldfirst = 0;
                for (size_t i = 0; i < ue4_nb; i++)
                    if (ue4_hash(b + i * UE4_BLK, UE4_BLK) == ent_old) { oldhits++; if (oldhits == 1) oldfirst = i; }
                droid_log("ue4: blocks %zu changed %zu zeroed %zu permuted %zu fresh %zu", ue4_nb,
                          changed, zeroed, permuted, fresh);
                droid_log("ue4: entry block %zu old %016llx new %016llx zero %d oldhits %d first %zu",
                          ent, (unsigned long long)ent_old, (unsigned long long)ent_new, ent_zero,
                          oldhits, oldfirst);
                free(tab);
            }
#endif
        }
        if (ue4_probe && c == 0) {
            const char *dump = getenv("DROID_DUMP_UE4");
            if (dump) {
                FILE *f = fopen(dump, "wb");
                if (f) {
                    fwrite(am->m.base, 1, am->m.size, f);
                    fclose(f);
                    droid_log("ue4: dumped %llu bytes to %s", (unsigned long long)am->m.size, dump);
                }
            }
        }
        if (ue4_probe && ue4_entry) {
            uint32_t w = *(volatile uint32_t *)ue4_entry;
            if (w != ue4_word) {
                droid_log("ue4: ctor %d/%d at %#llx changed entry %08x -> %08x (ok=%d)",
                          c, am->m.ctor_count, (unsigned long long)am->m.ctors[c], ue4_word, w, ok);
                ue4_word = w;
            }
        }
    }
    droid_log("modules: %s at %p (%llu KB), %d constructors ok=%d (%llu Mcycles), %d thunks", base,
              am->m.base, (unsigned long long)(am->m.size >> 10), am->m.ctor_count, ok,
              (unsigned long long)((__builtin_ia32_rdtsc() - t0) / 1000000), arm_thunk_total());
    droid_startup("%s loaded, its startup code run (%llu KB)", base, (unsigned long long)(am->m.size >> 10));
    pthread_mutex_unlock(&lock);
    return &am->m;
}

so_module *arm_module_at(uint64_t addr, const char **name, const char **path) {
    for (int i = 0; i < mod_count; i++) {
        uint64_t b = (uint64_t)mods[i].m.base;
        if (addr >= b && addr < b + mods[i].m.size) {
            if (name) *name = mods[i].name;
            if (path) *path = mods[i].path;
            return &mods[i].m;
        }
    }
    return NULL;
}

int arm_module_count(void) { return mod_count; }
so_module *arm_module(int i) { return i >= 0 && i < mod_count ? &mods[i].m : NULL; }
const char *arm_module_path(int i) { return i >= 0 && i < mod_count ? mods[i].path : NULL; }

// ---- dlopen & co. for ARM code ----------------------------------------------------

// Libraries the system has (we answer their functions): one shared handle.
static int system_handle;
static const char *system_libs[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so",
    "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libz.so", "libaaudio.so", "libmediandk.so",
    "libjnigraphics.so", "libstdc++.so",
};

static int vulkan_handle; // libvulkan.so: our Vulkan bridge

static void *a_dlopen(const char *name, int flags) {
    (void)flags;
    if (!name) return &system_handle; // the program itself: search everything
    const char *base = base_name(name);
    if (strcmp(base, "libvulkan.so") == 0 || strcmp(base, "libvulkan.so.1") == 0) {
        if (vk_bridge_enabled()) return &vulkan_handle; // vulkan_bridge.c
        dl_error = "libvulkan.so: not available";
        droid_log("dlopen(%s): Vulkan off: the game falls back to GLES", name);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(system_libs) / sizeof(system_libs[0]); i++)
        if (strcmp(base, system_libs[i]) == 0) return &system_handle;
    so_module *m = arm_module_load(base);
    if (!m) {
        dl_error = "library not found";
        droid_log("dlopen(%s): not found", name);
    } else if (strcmp(base, "libunity.so") && strcmp(base, "libil2cpp.so") && strcmp(base, "libmain.so")) {
        // a plugin the game's C# opened itself (DllImport): its JNI_OnLoad, once, as Java's
        // System.loadLibrary of it would have run first on a phone. Firebase keeps its JavaVM
        // there: without it, a call through a null JavaVM (Vector: a jump to address 0)
        int java_jni_onload(void *so_module_ptr); // java_layer.c
        java_jni_onload(m);
    }
    return m;
}

static void *a_dlsym(void *handle, const char *name) {
    if (handle == &vulkan_handle) {
        void *p = vk_bridge_dlsym(name);
        if (!p) dl_error = "symbol not found";
        return p;
    }
    if (handle && handle != &system_handle && handle != (void *)-1L) {
        void *p = so_sym((so_module *)handle, name);
        if (!p) dl_error = "symbol not found";
        return p;
    }
    for (int i = 0; i < mod_count; i++) { // RTLD_DEFAULT / the program / system handle
        void *p = so_sym(&mods[i].m, name);
        if (p) return p;
    }
    void *arm_table_find(const char *name);
    // as imports are: the wrapped ones (miHoYo's games only; every other game had the plain ones
    // from dlsym and works so), sockets, Linux layouts, bionic's
    // (and AAudio's callback setters for every game: the plain one hands an ARM function to our
    // audio thread as PS5 code - FMOD from dlsym, Geometry Dash crashed at its first sound)
    int callback_setter = !strncmp(name, "AAudioStreamBuilder_set", 23) && strstr(name, "Callback");
    void *p = (droid_quirks & QUIRK_MIHOYO) || callback_setter ? arm_table_find(name) : NULL;
    if (!p) p = linux_abi_resolve(name);
    if (!p) p = droid_resolve(name);
    if (!p) {
        dl_error = "symbol not found";
        return NULL;
    }
    return (void *)arm_thunk(name, p);
}

static int a_dlclose(void *handle) {
    (void)handle;
    return 0; // libraries stay
}

static const char *a_dlerror(void) {
    const char *e = dl_error;
    dl_error = NULL;
    return e;
}

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} arm_dl_info;

void droid_log_managed_exceptions(void); // bionic_shim.c

static int a_dladdr(const void *addr, arm_dl_info *info) {
    // IL2CPP asks this while it raises a C# exception (its stack trace): the exception, logged
    static int asked;
    if (asked++ < 40) droid_log_managed_exceptions();
    const char *path = NULL;
    so_module *m = arm_module_at((uint64_t)addr, NULL, &path);
    if (!m) return 0;
    info->dli_fname = path;
    info->dli_fbase = m->base;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

// bionic's struct dl_phdr_info (64 bytes on arm64)
typedef struct {
    uint64_t dlpi_addr;
    const char *dlpi_name;
    const void *dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds, dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
} arm_phdr_info;

static int a_dl_iterate_phdr(uint64_t cb, void *data) {
    // the unwinder asks this per frame of a C++ exception: what's thrown, logged (a few)
    static int asked;
    if (asked++ < 60) droid_log_managed_exceptions();
    // ArmProf: the writable segments each library shows (a GC scans them as roots), once
    int droid_tool(const char *name); // game_settings.c
    static int roots_told;
    if (!roots_told && droid_tool("ArmProf")) {
        roots_told = 1;
        unsigned long long sum = 0;
        for (int i = 0; i < mod_count; i++) {
            const Elf64_Phdr *ph = (const Elf64_Phdr *)mods[i].m.phdr;
            if (!ph) continue;
            for (int k = 0; k < (int)mods[i].m.phnum; k++)
                if (ph[k].p_type == PT_LOAD && (ph[k].p_flags & PF_W) && ph[k].p_memsz >= 65536) {
                    sum += ph[k].p_memsz;
                    droid_log("roots: %s writable %llu KB (file %llu KB)", mods[i].path ? mods[i].path : "?",
                              (unsigned long long)ph[k].p_memsz >> 10, (unsigned long long)ph[k].p_filesz >> 10);
                }
        }
        droid_log("roots: %d libraries, %llu KB writable in all (segments of 64 KB and more)", mod_count, sum >> 10);
    }
    typedef int (*cb_fn)(arm_phdr_info *, size_t, void *);
    cb_fn fn = (cb_fn)arm_callback(cb);
    int r = 0;
    for (int i = 0; i < mod_count && r == 0; i++) {
        arm_phdr_info info;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = mods[i].m.bias;
        info.dlpi_name = mods[i].path;
        info.dlpi_phdr = mods[i].m.phdr;
        info.dlpi_phnum = (uint16_t)mods[i].m.phnum;
        info.dlpi_adds = (unsigned long long)mod_count;
        if (!info.dlpi_phdr) continue;
        r = fn(&info, sizeof(info), data);
    }
    return r;
}

#define D(name, fn) { name, (void *)fn }
static const struct { const char *name; void *fn; } table[] = {
    D("dlopen", a_dlopen), D("dlsym", a_dlsym), D("dlclose", a_dlclose), D("dlerror", a_dlerror),
    D("dladdr", a_dladdr), D("dl_iterate_phdr", a_dl_iterate_phdr),
};

void *arm_modules_resolve_dl(const char *name) {
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}
