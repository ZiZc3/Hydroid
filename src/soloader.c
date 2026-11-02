// soloader.c — minimal ELF loader for Android x86_64 .so files on PS5.
// Maps PT_LOAD segments, resolves R_X86_64_RELATIVE relocations,
// and provides symbol lookup. Ignores GNU/Android-specific metadata.

#include "soloader.h"
#include "android_shim.h"

void droid_log(const char *fmt, ...);
#include <elf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A title can't mmap anonymous RWX memory: the image lives in CPU-only direct
// memory mapped RW, then made RWX with sceKernelMprotect (as QEMU-VM's JIT).
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len,
                                  size_t align, int type, int64_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                             int64_t phys, size_t align);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelMprotect(const void *addr, size_t len, int prot);

#define DMEM_TYPE_CPU 12
#define DMEM_ALIGN    0x10000ULL
#define KPROT_RW      0x03
#define KPROT_RWX     0x07

#ifndef R_X86_64_JMP_SLOT // Linux's name for it (the PC test runner)
#define R_X86_64_JMP_SLOT R_X86_64_JUMP_SLOT
#endif

void *(*so_import_resolver)(const char *name);
void (*so_missing_import)(const char *name);
void *(*so_arm_import_resolver)(const char *name, int is_data);
void *(*so_arm_interpose)(const char *name);
// ELF thread-local storage of an ARM library (PT_TLS; R_AARCH64_TLSDESC / TLS_TPREL64): its
// template registered with the JIT (arm_cpu.cpp arm_tls_add: its offset from tpidr_el0, every
// thread gets a copy there), and the TLSDESC resolver (ARM code: ldr x0, [x0, #8]; ret).
long (*so_arm_tls_add)(const void *image, size_t filesz, size_t memsz, size_t align);
void *so_arm_tlsdesc_resolver;

static uint64_t missing_import(void) { return 0; }

static int readall(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

// Android's packed relocations ("APS2", DT_ANDROID_RELA: Genshin Impact's 362 MB library,
// 4.4 MB of them): groups of entries sharing an offset step, a type or an addend, in SLEB128
// (bionic's packed_reloc_iterator). Unpacked into a plain table. NULL: not that format.
static int64_t sleb(const uint8_t **p, const uint8_t *end) {
    int64_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        if (*p >= end) return 0;
        b = *(*p)++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -((int64_t)1 << shift);
    return v;
}
static Elf64_Rela *unpack_aps2(const uint8_t *p, size_t size, size_t *out_n) {
    const uint8_t *end = p + size;
    if (size < 4 || memcmp(p, "APS2", 4) != 0) return NULL;
    p += 4;
    int64_t count = sleb(&p, end);
    if (count <= 0 || count > (1 << 26)) return NULL;
    Elf64_Rela *out = malloc((size_t)count * sizeof(*out));
    if (!out) return NULL;
    Elf64_Rela r = { 0, 0, 0 };
    r.r_offset = (uint64_t)sleb(&p, end);
    size_t n = 0;
    while ((int64_t)n < count && p < end) {
        int64_t group = sleb(&p, end), flags = sleb(&p, end), delta = 0;
        const int by_info = flags & 1, by_delta = flags & 2, by_addend = flags & 4, has_addend = flags & 8;
        if (by_delta) delta = sleb(&p, end);
        if (by_info) r.r_info = (uint64_t)sleb(&p, end);
        if (has_addend && by_addend) r.r_addend += sleb(&p, end);
        else if (!has_addend) r.r_addend = 0;
        for (int64_t i = 0; i < group && (int64_t)n < count; i++) {
            r.r_offset += by_delta ? (uint64_t)delta : (uint64_t)sleb(&p, end);
            if (!by_info) r.r_info = (uint64_t)sleb(&p, end);
            if (has_addend && !by_addend) r.r_addend += sleb(&p, end);
            out[n++] = r;
        }
    }
    *out_n = n;
    return out;
}

// RELR (DT_RELR, newer NDKs): relative relocations as addresses and bitmaps of the 63 words
// after; each word gets the load bias added
static size_t apply_relr(const uint64_t *relr, size_t count, char *base, uint64_t lo) {
    uint64_t *where = NULL;
    size_t done = 0;
    for (size_t i = 0; i < count; i++) {
        const uint64_t e = relr[i];
        if (!(e & 1)) {
            where = (uint64_t *)(base + (e - lo));
            *where++ += (uint64_t)base - lo;
            done++;
        } else if (where) {
            for (int b = 1; b < 64; b++)
                if (e >> b & 1) {
                    where[b - 1] += (uint64_t)base - lo;
                    done++;
                }
            where += 63;
        }
    }
    return done;
}

int so_load(const char *path, so_module *mod) {
    memset(mod, 0, sizeof(*mod));

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    // read ELF header
    Elf64_Ehdr ehdr;
    if (readall(fd, &ehdr, sizeof(ehdr)) < 0) { close(fd); return -2; }

    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        (ehdr.e_machine != EM_X86_64 && ehdr.e_machine != EM_AARCH64) ||
        ehdr.e_type != ET_DYN) {
        close(fd);
        return -3;
    }

    // read program headers
    size_t phsize = ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr phdr[64];
    if (ehdr.e_phnum > 64) { close(fd); return -4; }
    lseek(fd, ehdr.e_phoff, SEEK_SET);
    if (readall(fd, phdr, phsize) < 0) { close(fd); return -5; }

    // find virtual address range from PT_LOAD segments
    uint64_t lo = ~(uint64_t)0, hi = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_vaddr < lo) lo = phdr[i].p_vaddr;
        uint64_t end = phdr[i].p_vaddr + phdr[i].p_memsz;
        if (end > hi) hi = end;
    }
    lo = lo & ~0xFFFULL;
    hi = (hi + 0xFFF) & ~0xFFFULL;
    uint64_t total = hi - lo;

    total = (total + DMEM_ALIGN - 1) & ~(DMEM_ALIGN - 1);
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total,
                                      DMEM_ALIGN, DMEM_TYPE_CPU, &phys) != 0) {
        close(fd);
        return -6;
    }
    void *base = droid_va_hint(total); // out of RADV's GPU window (linux_abi.c)
    if (sceKernelMapDirectMemory(&base, total, KPROT_RW, 0, phys, DMEM_ALIGN) != 0) {
        sceKernelReleaseDirectMemory(phys, total);
        close(fd);
        return -8;
    }
    memset(base, 0, total);

    // load each PT_LOAD segment
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        void *seg = (char *)base + (phdr[i].p_vaddr - lo);
        lseek(fd, phdr[i].p_offset, SEEK_SET);
        if (readall(fd, seg, phdr[i].p_filesz) < 0) {
            sceKernelMunmap(base, total);
            sceKernelReleaseDirectMemory(phys, total);
            close(fd);
            return -7;
        }
    }

    // find .dynamic
    Elf64_Dyn *dyn = NULL;
    size_t dyn_count = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)((char *)base + (phdr[i].p_vaddr - lo));
            dyn_count = phdr[i].p_filesz / sizeof(Elf64_Dyn);
            break;
        }
    }
    // its thread-local storage (PT_TLS, 7): the template's place in every thread's block
    long tls_off = -1;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type != 7 || ehdr.e_machine != EM_AARCH64) continue;
        if (so_arm_tls_add)
            tls_off = so_arm_tls_add((char *)base + (phdr[i].p_vaddr - lo), phdr[i].p_filesz, phdr[i].p_memsz,
                                     phdr[i].p_align ? phdr[i].p_align : 8);
        droid_log("soloader: %s: thread-local storage %llu bytes (%llu initialised), at tpidr+%ld", path,
                  (unsigned long long)phdr[i].p_memsz, (unsigned long long)phdr[i].p_filesz, tls_off);
    }

    // parse dynamic entries for the relocation tables
    Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    Elf64_Rela *rela = NULL;
    size_t rela_count = 0;
    Elf64_Rela *jmprel = NULL;
    size_t jmprel_count = 0;
    const uint8_t *packed = NULL; // DT_ANDROID_RELA (APS2)
    size_t packed_size = 0;
    const uint64_t *relr = NULL; // DT_RELR
    size_t relr_count = 0;
    uint64_t init = 0, init_array = 0;
    size_t init_count = 0;

    if (dyn) {
        for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
            switch (dyn[i].d_tag) {
            case DT_SYMTAB:
                symtab = (Elf64_Sym *)((char *)base + (dyn[i].d_un.d_ptr - lo));
                break;
            case DT_STRTAB:
                strtab = (const char *)((char *)base + (dyn[i].d_un.d_ptr - lo));
                break;
            case DT_RELA:
                rela = (Elf64_Rela *)((char *)base + (dyn[i].d_un.d_ptr - lo));
                break;
            case DT_RELASZ:
                rela_count = dyn[i].d_un.d_val / sizeof(Elf64_Rela);
                break;
            case DT_JMPREL:
                jmprel = (Elf64_Rela *)((char *)base + (dyn[i].d_un.d_ptr - lo));
                break;
            case DT_PLTRELSZ:
                jmprel_count = dyn[i].d_un.d_val / sizeof(Elf64_Rela);
                break;
            case 0x60000011: // DT_ANDROID_RELA
                packed = (const uint8_t *)base + (dyn[i].d_un.d_ptr - lo);
                break;
            case 0x60000012: // DT_ANDROID_RELASZ
                packed_size = dyn[i].d_un.d_val;
                break;
            case 0x24: // DT_RELR
            case 0x6fffe000: // DT_ANDROID_RELR
                relr = (const uint64_t *)((char *)base + (dyn[i].d_un.d_ptr - lo));
                break;
            case 0x23: // DT_RELRSZ
            case 0x6fffe001: // DT_ANDROID_RELRSZ
                relr_count = dyn[i].d_un.d_val / sizeof(uint64_t);
                break;
            case DT_INIT:
                init = dyn[i].d_un.d_ptr;
                break;
            case DT_INIT_ARRAY:
                init_array = dyn[i].d_un.d_ptr;
                break;
            case DT_INIT_ARRAYSZ:
                init_count = dyn[i].d_un.d_val / sizeof(uint64_t);
                break;
            }
        }
    }

    const int arm = ehdr.e_machine == EM_AARCH64;
    size_t unpacked_count = 0;
    Elf64_Rela *unpacked = packed && packed_size ? unpack_aps2(packed, packed_size, &unpacked_count) : NULL;
    if (packed && !unpacked) droid_log("soloader: packed relocations not readable (%zu bytes)", packed_size);
    if (relr && relr_count) apply_relr(relr, relr_count, (char *)base, lo);
    Elf64_Rela *tables[3] = { rela, jmprel, unpacked };
    size_t counts[3] = { rela_count, jmprel_count, unpacked_count };
    for (int t = 0; t < 3; t++) {
        for (size_t i = 0; i < counts[t]; i++) {
            const Elf64_Rela *r = &tables[t][i];
            uint32_t type = ELF64_R_TYPE(r->r_info);
            uint64_t *where = (uint64_t *)((char *)base + (r->r_offset - lo));
            // thread-local variables (this library's own, static TLS): TLSDESC (1031) gets the
            // resolver and the variable's offset from tpidr_el0; TLS_TPREL64 (1030) the offset
            if (arm && (type == 1031 || type == 1030)) {
                const Elf64_Sym *sym = &symtab[ELF64_R_SYM(r->r_info)];
                static int told;
                if (tls_off < 0 || (ELF64_R_SYM(r->r_info) && sym->st_shndx == SHN_UNDEF)) {
                    if (told++ < 10)
                        droid_log("soloader: %s: TLS relocation %u for %s not supported (another library's)", path, type,
                                  ELF64_R_SYM(r->r_info) ? strtab + sym->st_name : "(local)");
                    continue;
                }
                const uint64_t off = (uint64_t)tls_off + (ELF64_R_SYM(r->r_info) ? sym->st_value : 0) + (uint64_t)r->r_addend;
                if (type == 1031) {
                    where[0] = (uint64_t)so_arm_tlsdesc_resolver;
                    where[1] = off;
                } else {
                    where[0] = off;
                }
                continue;
            }
            // AArch64's codes for the same four kinds (an ARM64 game for the JIT).
            if (arm) {
                type = type == 1027 ? R_X86_64_RELATIVE : type == 1025 ? R_X86_64_GLOB_DAT
                     : type == 1026 ? R_X86_64_JMP_SLOT : type == 257 ? R_X86_64_64 : 0;
            }
            if (type == R_X86_64_RELATIVE) {
                *where = (uint64_t)base + r->r_addend - lo;
                continue;
            }
            if (type != R_X86_64_JMP_SLOT && type != R_X86_64_GLOB_DAT && type != R_X86_64_64)
                continue;
            const Elf64_Sym *sym = &symtab[ELF64_R_SYM(r->r_info)];
            uint64_t value = 0;
            if (sym->st_shndx != SHN_UNDEF) {
                value = (uint64_t)base + sym->st_value - lo;
                void *bound = arm && so_arm_interpose ? so_arm_interpose(strtab + sym->st_name) : NULL;
                if (bound) value = (uint64_t)bound;
            } else {
                const char *name = strtab + sym->st_name;
                void *found;
                if (arm && so_arm_import_resolver) {
                    int data = ELF64_ST_TYPE(sym->st_info) == STT_OBJECT ||
                               ELF64_ST_TYPE(sym->st_info) == STT_TLS;
                    // missing functions already got a logging stub
                    found = so_arm_import_resolver(name, data);
                    value = (uint64_t)found;
                    if (!found && so_missing_import) so_missing_import(name);
                    *where = value + (type == R_X86_64_64 ? (uint64_t)r->r_addend : 0);
                    continue;
                }
                found = so_import_resolver ? so_import_resolver(name) : NULL;
                if (!found) {
                    if (so_missing_import) so_missing_import(name);
                    found = (void *)missing_import;
                }
                value = (uint64_t)found;
            }
            *where = value + (type == R_X86_64_64 ? (uint64_t)r->r_addend : 0);
        }
    }

    free(unpacked);
    close(fd);
    if (sceKernelMprotect(base, total, KPROT_RWX) != 0) {
        sceKernelMunmap(base, total);
        sceKernelReleaseDirectMemory(phys, total);
        return -9;
    }

    mod->base = base;
    mod->size = total;
    mod->bias = (uint64_t)base - lo;
    mod->phnum = ehdr.e_phnum;
    mod->phdr = NULL;
    for (int i = 0; i < ehdr.e_phnum; i++) // the headers inside a loaded segment
        if (phdr[i].p_type == PT_LOAD && ehdr.e_phoff >= phdr[i].p_offset &&
            ehdr.e_phoff + phsize <= phdr[i].p_offset + phdr[i].p_filesz)
            mod->phdr = (char *)base + (phdr[i].p_vaddr - lo) + (ehdr.e_phoff - phdr[i].p_offset);
    mod->phys = phys;
    mod->arm64 = arm;
    mod->entry = (ehdr.e_entry != 0) ?
                 (void *)((char *)base + ehdr.e_entry - lo) : NULL;

    // Constructors, as the linker runs them: DT_INIT, then DT_INIT_ARRAY
    // (already relocated to absolute addresses).
    // (libunity has 426: the list is as long as the library's.)
    typedef void (*ctor_fn)(void);
    uint64_t *found = malloc((init_count + 1) * sizeof(uint64_t));
    if (!found) return -10;
    int n = 0;
    if (init) found[n++] = (uint64_t)((char *)base + init - lo);
    if (init_array) {
        uint64_t *ctors = (uint64_t *)((char *)base + init_array - lo);
        for (size_t i = 0; i < init_count; i++)
            if (ctors[i] != 0 && ctors[i] != (uint64_t)-1) found[n++] = ctors[i];
    }
    mod->init_fn = init ? (uint64_t)((char *)base + init - lo) : 0;
    mod->init_array = init_array ? (uint64_t)((char *)base + init_array - lo) : 0;
    mod->init_array_count = (int)init_count;
    if (arm) {
        mod->ctors = found;
        mod->ctor_count = n;
    } else {
        for (int i = 0; i < n; i++) ((ctor_fn)found[i])();
        free(found);
    }
    return 0;
}

int so_needed(const char *path, char names[][64], int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    Elf64_Ehdr eh;
    Elf64_Phdr ph[64];
    int n = 0;
    if (readall(fd, &eh, sizeof(eh)) != 0 || eh.e_phnum > 64) goto done;
    lseek(fd, eh.e_phoff, SEEK_SET);
    if (readall(fd, ph, eh.e_phnum * sizeof(Elf64_Phdr)) != 0) goto done;
    Elf64_Dyn dyn[512];
    int ndyn = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        size_t sz = ph[i].p_filesz < sizeof(dyn) ? ph[i].p_filesz : sizeof(dyn);
        lseek(fd, ph[i].p_offset, SEEK_SET);
        if (readall(fd, dyn, sz) != 0) goto done;
        ndyn = (int)(sz / sizeof(Elf64_Dyn));
    }
    uint64_t strtab = 0;
    for (int i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++)
        if (dyn[i].d_tag == DT_STRTAB) strtab = dyn[i].d_un.d_ptr;
    off_t stroff = -1; // vaddr -> file offset
    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && strtab >= ph[i].p_vaddr && strtab < ph[i].p_vaddr + ph[i].p_filesz)
            stroff = (off_t)(ph[i].p_offset + (strtab - ph[i].p_vaddr));
    if (stroff < 0) goto done;
    for (int i = 0; i < ndyn && dyn[i].d_tag != DT_NULL && n < max; i++) {
        if (dyn[i].d_tag != DT_NEEDED) continue;
        lseek(fd, stroff + (off_t)dyn[i].d_un.d_val, SEEK_SET);
        char buf[64] = { 0 };
        if (read(fd, buf, sizeof(buf) - 1) <= 0) continue;
        buf[63] = 0;
        memcpy(names[n++], buf, 64);
    }
done:
    close(fd);
    return n;
}

void *so_sym(so_module *mod, const char *name) {
    // re-parse the dynamic section to find symtab/strtab
    // (simpler: walk from base using the ELF header)
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mod->base;

    uint64_t lo = 0;
    // find the lowest vaddr PT_LOAD
    Elf64_Phdr *ph = (Elf64_Phdr *)((char *)mod->base + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) { lo = ph[i].p_vaddr & ~0xFFFULL; break; }
    }

    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)((char *)mod->base + (ph[i].p_vaddr - lo));
            break;
        }
    }
    if (!dyn) return NULL;

    Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    uint64_t gnu_hash_addr = 0, sysv_hash_addr = 0, strsz = 0;

    for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
        case DT_SYMTAB:
            symtab = (Elf64_Sym *)((char *)mod->base + (dyn[i].d_un.d_ptr - lo));
            break;
        case DT_STRTAB:
            strtab = (const char *)((char *)mod->base + (dyn[i].d_un.d_ptr - lo));
            break;
        case DT_GNU_HASH:
            gnu_hash_addr = dyn[i].d_un.d_ptr;
            break;
        case DT_HASH: // the old (SysV) table: its second word is the number of symbols
            sysv_hash_addr = dyn[i].d_un.d_ptr;
            break;
        case DT_STRSZ:
            strsz = dyn[i].d_un.d_val;
            break;
        }
    }
    if (!symtab || !strtab) return NULL;

    // figure out symbol count from gnu_hash
    // gnu_hash: nbuckets, symndx, bloom_size, bloom_shift, bloom[], buckets[], chains[]
    uint32_t nsyms = 0;
    if (gnu_hash_addr) {
        uint32_t *gh = (uint32_t *)((char *)mod->base + (gnu_hash_addr - lo));
        uint32_t nbuckets = gh[0];
        uint32_t symndx = gh[1];
        uint32_t bloom_size = gh[2];
        uint32_t *buckets = &gh[4 + bloom_size * 2]; // 2 because Elf64 bloom is 8 bytes = 2 uint32
        uint32_t max_chain = 0;
        for (uint32_t i = 0; i < nbuckets; i++) {
            if (buckets[i] > max_chain) max_chain = buckets[i];
        }
        if (max_chain >= symndx) {
            uint32_t *chains = &buckets[nbuckets];
            uint32_t idx = max_chain - symndx;
            while (!(chains[idx] & 1)) idx++;
            nsyms = symndx + idx + 1;
        }
    }
    // only the SysV table (Genshin's libraries): its count - the guess of 256 read past the end
    // of a shorter symbol table, a crash in the next library's imports
    if (nsyms == 0 && sysv_hash_addr) nsyms = ((uint32_t *)((char *)mod->base + (sysv_hash_addr - lo)))[1];
    if (nsyms == 0) nsyms = 256; // fallback: scan up to 256

    for (uint32_t i = 0; i < nsyms; i++) {
        if (symtab[i].st_name == 0) continue;
        if (symtab[i].st_shndx == SHN_UNDEF) continue;
        if (strsz && symtab[i].st_name >= strsz) continue; // not a name: past the strings
        if (strcmp(strtab + symtab[i].st_name, name) == 0) {
            return (char *)mod->base + (symtab[i].st_value - lo);
        }
    }
    return NULL;
}

void so_unload(so_module *mod) {
    free(mod->ctors);
    if (mod->base && mod->size) {
        sceKernelMunmap(mod->base, mod->size);
        sceKernelReleaseDirectMemory(mod->phys, mod->size);
    }
    memset(mod, 0, sizeof(*mod));
}
