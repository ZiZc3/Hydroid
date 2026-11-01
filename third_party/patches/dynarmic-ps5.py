"""dynarmic on the PS5: its code cache comes from the title (droid_jit_alloc:
direct memory made read-write-execute), as a PS5 title can't mmap anonymous
RWX memory. Run on suyu-emu/dynarmic a2ef15df6d; safe to run twice."""
import sys

path = sys.argv[1] + '/src/dynarmic/backend/x64/block_of_code.cpp'
s = open(path, encoding='utf-8').read()
marker = 'class CustomXbyakAllocator : public Xbyak::Allocator {\npublic:\n#ifdef _WIN32'
patched = '''#if defined(__PROSPERO__)
extern "C" void* droid_jit_alloc(size_t size);
extern "C" void droid_jit_free(void* p);
#endif

class CustomXbyakAllocator : public Xbyak::Allocator {
public:
#if defined(__PROSPERO__)
    uint8_t* alloc(size_t size) override {
        void* p = droid_jit_alloc(size);
        if (p == nullptr) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        return static_cast<uint8_t*>(p);
    }

    void free(uint8_t* p) override {
        droid_jit_free(p);
    }

    bool useProtect() const override { return false; }
#elif defined(_WIN32)'''
if 'droid_jit_alloc' not in s:
    assert marker in s, 'dynarmic changed: update the PS5 patch'
    s = s.replace(marker, patched, 1)
    open(path, 'w', encoding='utf-8').write(s)
print('dynarmic: PS5 code cache patch applied')

# suyu's tree ships mcl/include/boost/*.hpp forwarding headers that point at a
# developer's Windows path (C:/Users/charl/...): drop them, the real Boost is used.
import os, shutil
stray = sys.argv[1] + '/externals/mcl/include/boost'
if os.path.isdir(stray):
    shutil.rmtree(stray)
    print('dynarmic: removed stray mcl/include/boost forwarding headers')

# Translation time, per block, to Hydroid (droid_translate_ns, arm_cpu.cpp: the "ms translating"
# of the speed report), and no VerificationPass (a check of dynarmic's own IR, ~2% of a translation)
path = sys.argv[1] + '/src/dynarmic/backend/x64/a64_interface.cpp'
s = open(path, encoding='utf-8').read()
if 'droid_translate_ns' not in s:
    a = '''        // JIT Compile
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };'''
    assert a in s, 'dynarmic changed: update the translate-time patch'
    s = s.replace(a, '''        // JIT Compile (timed: Android-on-PS5's speed report)
        const auto translate_start = std::chrono::steady_clock::now();
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };''', 1)
    b = '''        Optimization::VerificationPass(ir_block);
        return emitter.Emit(ir_block).entrypoint;'''
    assert b in s, 'dynarmic changed: update the translate-time patch'
    s = s.replace(b, '''        const CodePtr entry = emitter.Emit(ir_block).entrypoint;
        droid_translate_ns(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - translate_start).count()));
        return entry;''', 1)
    c = 'namespace Dynarmic::A64 {'
    assert c in s
    s = s.replace(c, '#include <chrono>\nextern "C" void droid_translate_ns(uint64_t ns); // arm_cpu.cpp\n\n' + c, 1)
    open(path, 'w', encoding='utf-8').write(s)
    print('dynarmic: translate-time patch applied')

# Translate-ahead (Android-on-PS5's warm list, arm_cpu.cpp): Jit::Precompile(pc) translates the
# block at pc now, without running it; the translate-time report gets the block's address
path = sys.argv[1] + '/src/dynarmic/backend/x64/a64_interface.cpp'
s = open(path, encoding='utf-8').read()
if 'droid_translate_ns(uint64_t ns, uint64_t pc' not in s:
    a = 'extern "C" void droid_translate_ns(uint64_t ns); // arm_cpu.cpp'
    assert a in s, 'dynarmic: the translate-time patch is not there'
    s = s.replace(a, 'extern "C" void droid_translate_ns(uint64_t ns, uint64_t pc); // arm_cpu.cpp', 1)
    b = '''            std::chrono::steady_clock::now() - translate_start).count()));'''
    assert b in s
    s = s.replace(b, '''            std::chrono::steady_clock::now() - translate_start).count()),
            A64::LocationDescriptor{current_location}.PC());''', 1)
    c = '''    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};'''
    assert c in s
    s = s.replace(c, '''    void Precompile(u64 pc) {
        GetBlock(IR::LocationDescriptor{A64::LocationDescriptor{pc, FP::FPCR{jit_state.fpcr}, false}});
    }

''' + c, 1)
    d = '''void Jit::ClearCache() {
    impl->ClearCache();
}'''
    assert d in s
    s = s.replace(d, d + '''

void Jit::Precompile(std::uint64_t pc) {
    impl->Precompile(pc);
}''', 1)
    open(path, 'w', encoding='utf-8').write(s)
    h = sys.argv[1] + '/src/dynarmic/interface/A64/a64.h'
    t = open(h, encoding='utf-8').read()
    e = '''    void ClearCache();'''
    assert e in t
    t = t.replace(e, e + '''

    /// Translates the block at pc now, without running it (Android-on-PS5's translate-ahead).
    void Precompile(std::uint64_t pc);''', 1)
    open(h, 'w', encoding='utf-8').write(t)
    print('dynarmic: translate-ahead patch applied')

# Where a translation's time goes (Android-on-PS5's speed report): the ARM decode (Translate),
# the IR passes, the x86 emit, in TSC cycles to droid_translate_phases (arm_cpu.cpp)
path = sys.argv[1] + '/src/dynarmic/backend/x64/a64_interface.cpp'
s = open(path, encoding='utf-8').read()
if 'droid_translate_phases' not in s:
    a = 'extern "C" void droid_translate_ns(uint64_t ns, uint64_t pc); // arm_cpu.cpp'
    assert a in s, 'dynarmic: the translate-time patch is not there'
    s = s.replace(a, a + '\nextern "C" void droid_translate_phases(uint64_t decode, uint64_t passes, uint64_t emit); // arm_cpu.cpp', 1)
    b = '''        Optimization::PolyfillPass(ir_block, polyfill_options);'''
    assert b in s
    s = s.replace(b, '''        const uint64_t phase_decoded = __builtin_ia32_rdtsc();
        Optimization::PolyfillPass(ir_block, polyfill_options);''', 1)
    c = '''        const CodePtr entry = emitter.Emit(ir_block).entrypoint;'''
    assert c in s
    s = s.replace(c, '''        const uint64_t phase_passed = __builtin_ia32_rdtsc();
        const CodePtr entry = emitter.Emit(ir_block).entrypoint;
        const uint64_t phase_emitted = __builtin_ia32_rdtsc();
        droid_translate_phases(phase_decoded - phase_start, phase_passed - phase_decoded, phase_emitted - phase_passed);''', 1)
    d = '''        const auto translate_start = std::chrono::steady_clock::now();'''
    assert d in s
    s = s.replace(d, d + '''
        const uint64_t phase_start = __builtin_ia32_rdtsc();''', 1)
    open(path, 'w', encoding='utf-8').write(s)
    print('dynarmic: translate-phases patch applied')



# Saved translations (PPTC): taken out again (user, 2026-10-26: "not safe, remove it"). A tree
# patched by it is put back the way the earlier patches left it. (A backup of the patched tree:
# K:\hydroid-work\backups\dynarmic-with-pptc-2026-10-26.tgz)
x64 = sys.argv[1] + '/src/dynarmic/backend/x64/'


def unedit(path, fn):
    if not os.path.exists(path):
        return
    s = open(path, encoding='utf-8').read()
    t = fn(s)
    if t != s:
        open(path, 'w', encoding='utf-8').write(t)


def drop_lines(s, *needles):
    return '\n'.join(l for l in s.split('\n') if not any(n in l for n in needles))


for f in ('pptc.h', 'pptc.cpp'):
    if os.path.exists(x64 + f):
        os.remove(x64 + f)
unedit(sys.argv[1] + '/src/dynarmic/CMakeLists.txt', lambda s: drop_lines(s, 'backend/x64/pptc.cpp', 'backend/x64/pptc.h'))
unedit(x64 + 'constant_pool.h', lambda s: drop_lines(s, 'PoolBegin()', 'PoolBytes()', 'GetConstantPtr('))


def pool_cpp(s):
    i = s.find('const void* ConstantPool::GetConstantPtr(u64 lower, u64 upper) {')
    if i < 0:
        return s
    j = s.find('\n}\n\n', i) + len('\n}\n\n')
    return s[:i] + s[j:]


unedit(x64 + 'constant_pool.cpp', pool_cpp)
unedit(x64 + 'block_of_code.h', lambda s: drop_lines(s, 'ConstantPoolBegin()', 'ConstantPoolBytes()', 'ConstantPtr(u64 lower'))


def emit_h(s):
    s = drop_lines(s, '// saved translations (pptc.cpp)', 'void PptcKeep(', 'BlockDescriptor PptcLoad(')
    return s.replace('\nnamespace Pptc {\nstruct Saved;\nstruct Layout;\n}  // namespace Pptc', '')


unedit(x64 + 'a64_emit_x64.h', emit_h)
for name in ('a64_emit_x64.cpp', 'emit_x64.cpp', 'emit_x64_memory.cpp.inc', 'a64_emit_x64_memory.cpp', 'a32_emit_x64_memory.cpp'):
    unedit(x64 + name, lambda s: drop_lines(s, '#include "dynarmic/backend/x64/pptc.h"', 'Pptc::Current()'))


def iface(s):
    s = drop_lines(s, '#include "dynarmic/backend/x64/pptc.h"')
    # the miss path: from "if (Backend::X64::Pptc::On()) {" to its closing brace before "// JIT Compile"
    a = '        if (Backend::X64::Pptc::On()) {\n'
    i = s.find(a)
    if i >= 0:
        j = s.find("        // JIT Compile (timed: Android-on-PS5's speed report)", i)
        s = s[:i] + s[j:]
    # the keep path: back to the plain emit
    b = '        const bool pptc_on = Backend::X64::Pptc::On();\n'
    i = s.find(b)
    if i >= 0:
        j = s.find('            emitter.PptcKeep(', i)
        j = s.find('\n        }', j) + len('\n        }')
        s = s[:i] + '        const CodePtr entry = emitter.Emit(ir_block).entrypoint;' + s[j:]
    return s


unedit(x64 + 'a64_interface.cpp', iface)
print('dynarmic: pptc taken out (if it was in)')

# One code cache for every guest thread (after Ryujinx's one-translator-per-process; files in
# third_party/dynarmic-shared). Per-thread pointers are read through r15 (the JitState) instead of
# being baked into the x86, links between blocks are patched with single aligned 8-byte writes
# (another thread may be running the code), and A64::UserConfig::droid_shared_code makes a Jit use
# the one shared code cache + emitter (Hydroid: tools.txt "SharedJit"). Without it, a Jit keeps its
# own as before - the r15 reads then give its own pointers.
import shutil as _sh2
here = os.path.dirname(os.path.abspath(__file__))
x64 = sys.argv[1] + '/src/dynarmic/backend/x64/'
for f in ('shared_jit.h', 'shared_jit.cpp'):
    _sh2.copyfile(os.path.join(here, '..', 'dynarmic-shared', f), x64 + f)


def sedit(path, fn):
    s = open(path, encoding='utf-8').read()
    t = fn(s)
    if t != s:
        open(path, 'w', encoding='utf-8').write(t)


def rep(s, a, b, count=1):
    assert a in s, a[:80]
    return s.replace(a, b, count)


def cmake(s):
    if 'backend/x64/shared_jit.cpp' in s:
        return s
    return rep(s, '            backend/x64/a64_emit_x64.cpp\n',
               '            backend/x64/a64_emit_x64.cpp\n            backend/x64/shared_jit.cpp\n            backend/x64/shared_jit.h\n')


sedit(sys.argv[1] + '/src/dynarmic/CMakeLists.txt', cmake)


def config(s):
    if 'droid_shared_code' in s:
        return s
    a = '    size_t code_cache_size = 128 * 1024 * 1024;  // bytes\n'
    return rep(s, a, a + '\n    /// Android-on-PS5: use the one code cache shared by every Jit that sets this (the first one\n'
                         '    /// made decides its size and its callbacks object, which must live as long as the process)\n'
                         '    bool droid_shared_code = false;\n')


sedit(sys.argv[1] + '/src/dynarmic/interface/A64/config.h', config)

SLOTS = ('    // Android-on-PS5: the per-thread pointers, read through r15 (one code cache for every thread)\n'
         '    u64 sh_impl = 0, sh_callbacks = 0, sh_tpidr = 0, sh_tpidrro = 0, sh_mon_addr = 0, sh_mon_val = 0, sh_dispatch = 0;\n')
for js in ('a64_jitstate.h', 'a32_jitstate.h'):
    def slots(s):
        if 'sh_callbacks' in s:
            return s
        return rep(s, '    u32 fpsr_qc = 0;\n', '    u32 fpsr_qc = 0;\n' + SLOTS)
    sedit(x64 + js, slots)


# the callbacks' `this` (and the Jit, for the dispatcher's block lookup): through r15 when it is a
# per-thread pointer the Jit registered
def callback(s):
    if 'Shared::ArgSlot' in s:
        return s
    s = rep(s, '#include "dynarmic/backend/x64/block_of_code.h"',
            '#include "dynarmic/backend/x64/block_of_code.h"\n#include "dynarmic/backend/x64/shared_jit.h"')
    for reg in ('ABI_PARAM1', 'ABI_PARAM2'):
        a = '    code.mov(code.%s, arg);' % reg
        b = ('    if (const int slot = Shared::ArgSlot(arg); slot >= 0)\n'
             '        code.mov(code.%s, code.qword[code.r15 + slot]);\n'
             '    else\n'
             '        code.mov(code.%s, arg);') % (reg, reg)
        s = s.replace(a, b)
    return s


sedit(x64 + 'callback.cpp', callback)


def a64_emit(s):
    if 'sh_tpidr' in s:
        return s
    s = rep(s, '        code.mov(result, u64(conf.tpidr_el0));',
            '        code.mov(result, qword[r15 + offsetof(A64JitState, sh_tpidr)]);')
    s = rep(s, '        code.mov(result, u64(conf.tpidrro_el0));',
            '        code.mov(result, qword[r15 + offsetof(A64JitState, sh_tpidrro)]);')
    s = rep(s, '        code.mov(addr, u64(conf.tpidr_el0));',
            '        code.mov(addr, qword[r15 + offsetof(A64JitState, sh_tpidr)]);')
    # this thread's fast dispatch table
    s = rep(s, '        code.mov(r12, reinterpret_cast<u64>(fast_dispatch_table.data()));',
            '        code.mov(r12, qword[r15 + offsetof(A64JitState, sh_dispatch)]);')
    s = rep(s, '        fast_dispatch_table_lookup = code.getCurr<FastDispatchEntry& (*)(u64)>();\n'
               '        code.mov(code.ABI_PARAM2, reinterpret_cast<u64>(fast_dispatch_table.data()));\n',
            '        fast_dispatch_table_lookup = code.getCurr<FastDispatchEntry& (*)(u64, FastDispatchEntry*)>(); // (the table: param 2)\n')
    s = rep(s, '        fast_dispatch_table.fill({});',
            '        for (FastDispatchEntry* t : dispatch_tables) std::fill(t, t + fast_dispatch_table_size, FastDispatchEntry{});')
    s = rep(s, '        (*fast_dispatch_table_lookup)(location.Value()) = {};',
            '        for (FastDispatchEntry* t : dispatch_tables) (*fast_dispatch_table_lookup)(location.Value(), t) = {};')
    # link spots on 8 bytes, so they can be patched with one atomic write while other threads run
    for kind in ('jg', 'jz', 'jmp'):
        for ind in ('        ', '    '):
            a = ind + 'patch_information[terminal.next].%s.push_back(code.getCurr());' % kind
            if a in s:
                break
        s = rep(s, a, ind + 'code.align(8); // (an 8-byte atomic patch)\n' + a)
    s += '''
namespace Dynarmic::Backend::X64 {

void A64EmitX64::AddDispatchTable(void* table) {
    auto* t = static_cast<FastDispatchEntry*>(table);
    std::fill(t, t + fast_dispatch_table_size, FastDispatchEntry{});
    dispatch_tables.push_back(t);
}

void A64EmitX64::RemoveDispatchTable(void* table) {
    dispatch_tables.erase(std::remove(dispatch_tables.begin(), dispatch_tables.end(), static_cast<FastDispatchEntry*>(table)),
                          dispatch_tables.end());
}

// a link turned back into "return to the dispatcher with this PC" (a block invalidated while other
// threads may run the code that jumps to it): a small stub at the end of the code, jumped to
CodePtr A64EmitX64::EmitUnlinkedStub(const IR::LocationDescriptor& target_desc) {
    code.align(8);
    const CodePtr stub = code.getCurr();
    code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
    code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
    code.jmp(code.GetReturnFromRunCodeAddress());
    return stub;
}

}  // namespace Dynarmic::Backend::X64
'''
    return s


sedit(x64 + 'a64_emit_x64.cpp', a64_emit)


def a64_emit_h(s):
    if 'dispatch_tables' in s:
        return s
    s = rep(s, '    std::array<FastDispatchEntry, fast_dispatch_table_size> fast_dispatch_table;',
            '    std::vector<FastDispatchEntry*> dispatch_tables; // one per Jit (the JitState\'s sh_dispatch)')
    s = rep(s, '    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64) = nullptr;',
            '    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64, FastDispatchEntry*) = nullptr;')
    s = rep(s, 'class A64EmitX64 final : public EmitX64 {\npublic:',
            'class A64EmitX64 final : public EmitX64 {\npublic:\n'
            '    // Android-on-PS5: a Jit\'s fast dispatch table (16 MB), added / removed (one code cache for many Jits)\n'
            '    static constexpr std::size_t DispatchTableBytes() { return 0x100000 * 0x10; }\n'
            '    void AddDispatchTable(void* table);\n'
            '    void RemoveDispatchTable(void* table);\n'
            '    CodePtr EmitUnlinkedStub(const IR::LocationDescriptor& target_desc) override;')
    return s


sedit(x64 + 'a64_emit_x64.h', a64_emit_h)


def emit_h(s):
    if 'EmitUnlinkedStub' in s:
        return s
    return rep(s, '    void Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);',
               '    void Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);\n'
               '    virtual CodePtr EmitUnlinkedStub(const IR::LocationDescriptor&) { return nullptr; } // (A64: one)')


sedit(x64 + 'emit_x64.h', emit_h)


def emit_cpp(s):
    if 'AtomicPatch8' in s:
        return s
    # the return stack's code pointer: its 8-byte immediate on 8 bytes
    s = rep(s, '    patch_information[target].mov_rcx.push_back(code.getCurr());',
            '    while ((code.getCurr<u64>() + 2) % 8) code.nop(); // (mov rcx, imm64: the imm64 patched in one write)\n'
            '    patch_information[target].mov_rcx.push_back(code.getCurr());')
    old_start = 'void EmitX64::Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {\n'
    i = s.index(old_start)
    j = s.index('\nvoid EmitX64::Unpatch(', i)
    new = r'''namespace {
// one aligned 8-byte store: a thread running the code sees the old bytes or the new, never half
bool AtomicPatch8(CodePtr at, u64 bytes) {
    if (mcl::bit_cast<u64>(at) % 8) return false;
    __atomic_store_n(reinterpret_cast<u64*>(const_cast<u8*>(reinterpret_cast<const u8*>(at))), bytes, __ATOMIC_RELEASE);
    return true;
}
u64 Rel32Bytes(std::initializer_list<u8> head, CodePtr at, CodePtr target, std::initializer_list<u8> tail) {
    u8 b[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    size_t n = 0;
    for (u8 x : head) b[n++] = x;
    const s32 rel = static_cast<s32>(mcl::bit_cast<s64>(target) - mcl::bit_cast<s64>(at) - static_cast<s64>(n + 4));
    std::memcpy(b + n, &rel, 4);
    n += 4;
    for (u8 x : tail) b[n++] = x;
    u64 v;
    std::memcpy(&v, b, 8);
    return v;
}
}  // namespace

// Links patched while other threads may run the code (one code cache for every thread): a jg / jz
// spot gets "jcc target; jmp +15" (to the end of its 23 bytes), a jmp spot "jmp target", the return
// stack's mov rcx its imm64 - each one aligned 8-byte write. No target: the jump goes to a stub
// that returns to the dispatcher with the PC (EmitUnlinkedStub). A spot not aligned (an old
// build's): rewritten the old way.
void EmitX64::Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr save_code_ptr = code.getCurr();
    const PatchInformation& patch_info = patch_information[target_desc];
    CodePtr stub = nullptr;
    auto dest = [&]() -> CodePtr {
        if (target_code_ptr) return target_code_ptr;
        if (!stub) {
            code.SetCodePtr(save_code_ptr);
            stub = EmitUnlinkedStub(target_desc);
        }
        return stub;
    };

    for (CodePtr location : patch_info.jg) {
        const CodePtr d = dest();
        if (!d || !AtomicPatch8(location, Rel32Bytes({0x0F, 0x8F}, location, d, {0xEB, 0x0F}))) {
            code.SetCodePtr(location);
            EmitPatchJg(target_desc, target_code_ptr);
        }
    }
    for (CodePtr location : patch_info.jz) {
        const CodePtr d = dest();
        if (!d || !AtomicPatch8(location, Rel32Bytes({0x0F, 0x84}, location, d, {0xEB, 0x0F}))) {
            code.SetCodePtr(location);
            EmitPatchJz(target_desc, target_code_ptr);
        }
    }
    for (CodePtr location : patch_info.jmp) {
        const CodePtr d = dest();
        if (!d || !AtomicPatch8(location, Rel32Bytes({0xE9}, location, d, {}))) {
            code.SetCodePtr(location);
            EmitPatchJmp(target_desc, target_code_ptr);
        }
    }
    for (CodePtr location : patch_info.mov_rcx) {
        const u64 v = mcl::bit_cast<u64>(target_code_ptr ? target_code_ptr : code.GetReturnFromRunCodeAddress());
        if (!AtomicPatch8(static_cast<const u8*>(location) + 2, v)) {
            code.SetCodePtr(location);
            EmitPatchMovRcx(target_code_ptr);
        }
    }

    code.SetCodePtr(stub ? code.getCurr() : save_code_ptr);
}
'''
    s = s[:i] + new + s[j:]
    if '#include <cstring>' not in s:
        s = s.replace('#include "dynarmic/backend/x64/emit_x64.h"', '#include "dynarmic/backend/x64/emit_x64.h"\n\n#include <cstring>', 1)
    return s


sedit(x64 + 'emit_x64.cpp', emit_cpp)


def mem_inc(s):
    if 'sh_mon_addr' in s:
        return s
    s = s.replace('    code.mov(tmp, mcl::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));',
                  '    code.mov(tmp, qword[r15 + offsetof(AxxJitState, sh_mon_addr)]); // (this thread\'s slot)')
    s = s.replace('    code.mov(tmp, mcl::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));',
                  '    code.mov(tmp, qword[r15 + offsetof(AxxJitState, sh_mon_val)]);')
    return s


sedit(x64 + 'emit_x64_memory.cpp.inc', mem_inc)


def iface(s):
    if 'struct CodeHome {' in s:
        return s
    s = rep(s, '#include "dynarmic/backend/x64/devirtualize.h"',
            '#include "dynarmic/backend/x64/devirtualize.h"\n#include "dynarmic/backend/x64/exclusive_monitor_friend.h"\n'
            '#include "dynarmic/backend/x64/shared_jit.h"')
    s = rep(s, '''    Impl(Jit* jit, UserConfig conf)
            : conf(conf)
            , block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf))
            , emitter(block_of_code, conf, jit)
            , polyfill_options(GenPolyfillOptions(block_of_code)) {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
    }

    ~Impl() = default;''', '''    Impl(Jit* jit, UserConfig conf)
            : conf(conf)
            , backend(Attach(jit, conf))
            , block_of_code(backend.block_of_code)
            , emitter(backend.emitter)
            , polyfill_options(backend.polyfill_options) {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
        dispatch_table.reset(new u64[A64EmitX64::DispatchTableBytes() / 8]);
        {
            std::lock_guard<std::recursive_mutex> g(backend.m);
            emitter.AddDispatchTable(dispatch_table.get());
        }
        FillSlots();
    }

    ~Impl() {
        std::lock_guard<std::recursive_mutex> g(backend.m);
        emitter.RemoveDispatchTable(dispatch_table.get());
    }

    // the code cache + emitter: this Jit's own, or the one every droid_shared_code Jit shares
    struct CodeHome {
        BlockOfCode block_of_code;
        A64EmitX64 emitter;
        Optimization::PolyfillOptions polyfill_options;
        std::recursive_mutex m;
        CodeHome(Impl* impl, Jit* jit, const UserConfig& conf)
                : block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, impl, conf), JitStateInfo{impl->jit_state}, conf.code_cache_size, GenRCP(conf))
                , emitter(block_of_code, conf, jit)
                , polyfill_options(GenPolyfillOptions(block_of_code)) {}
    };
    CodeHome& Attach(Jit* jit, const UserConfig& c) {
        // the pointers the emitter bakes in that are this thread's: read through r15 instead
        Backend::X64::Shared::RegisterArg(reinterpret_cast<u64>(c.callbacks), offsetof(A64JitState, sh_callbacks));
        Backend::X64::Shared::RegisterArg(reinterpret_cast<u64>(this), offsetof(A64JitState, sh_impl));
        FillSlots(c);
        if (!c.droid_shared_code) {
            own_backend = std::make_unique<CodeHome>(this, jit, c);
            return *own_backend;
        }
        static std::mutex shared_m;
        static CodeHome* shared = nullptr;
        std::lock_guard<std::mutex> g(shared_m);
        if (!shared) shared = new CodeHome(this, jit, c); // (lives as long as the process)
        return *shared;
    }
    void FillSlots() { FillSlots(conf); }
    void FillSlots(const UserConfig& c) {
        jit_state.sh_impl = reinterpret_cast<u64>(this);
        jit_state.sh_callbacks = reinterpret_cast<u64>(c.callbacks);
        jit_state.sh_tpidr = reinterpret_cast<u64>(c.tpidr_el0);
        jit_state.sh_tpidrro = reinterpret_cast<u64>(c.tpidrro_el0);
        if (c.global_monitor) {
            jit_state.sh_mon_addr = reinterpret_cast<u64>(GetExclusiveMonitorAddressPointer(c.global_monitor, c.processor_id));
            jit_state.sh_mon_val = reinterpret_cast<u64>(GetExclusiveMonitorValuePointer(c.global_monitor, c.processor_id));
        }
        jit_state.sh_dispatch = reinterpret_cast<u64>(dispatch_table.get());
    }''')
    s = rep(s, '''    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
    }''', '''    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
        FillSlots();
    }''')
    s = rep(s, '''    CodePtr GetBlock(IR::LocationDescriptor current_location) {
        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;
''', '''    CodePtr GetBlock(IR::LocationDescriptor current_location) {
        std::lock_guard<std::recursive_mutex> backend_lock(backend.m); // (one code cache, many threads)
        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;
''')
    # the cache full: shared, other threads may run any of it - never emptied under them
    s = rep(s, '''        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {''',
            '''        if (conf.droid_shared_code && block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            droid_log("arm: the shared JIT code cache is full: stopping (make it bigger)");
            std::abort();
        }
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {''')
    s = rep(s, '''            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
                block_of_code.ClearCache();
                emitter.ClearCache();
            } else {''', '''            jit_state.ResetRSB();
            std::lock_guard<std::recursive_mutex> backend_lock(backend.m);
            if (invalidate_entire_cache && conf.droid_shared_code) {
                // shared: every block dropped (links undone, dispatch tables emptied), the code
                // left where it is - another thread may be running it
                emitter.DropAllBlocks();
            } else if (invalidate_entire_cache) {
                block_of_code.ClearCache();
                emitter.ClearCache();
            } else {''')
    s = rep(s, '''    const UserConfig conf;
    A64JitState jit_state;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
''', '''    const UserConfig conf;
    A64JitState jit_state;
    std::unique_ptr<u64[]> dispatch_table; // this thread's fast dispatch table (16 MB)
    std::unique_ptr<CodeHome> own_backend;
    CodeHome& backend;
    BlockOfCode& block_of_code;
    A64EmitX64& emitter;
    Optimization::PolyfillOptions& polyfill_options;
''')
    return s


sedit(x64 + 'a64_interface.cpp', iface)


def drop_all(s):
    if 'DropAllBlocks' in s and 'void A64EmitX64::DropAllBlocks' in s:
        return s
    return s + '''
namespace Dynarmic::Backend::X64 {

void A64EmitX64::DropAllBlocks() {
    tsl::robin_set<IR::LocationDescriptor> all;
    for (const auto& kv : block_descriptors) all.insert(kv.first);
    InvalidateBasicBlocks(all);
    block_ranges.ClearCache();
    ClearFastDispatchTable();
}

}  // namespace Dynarmic::Backend::X64
'''


sedit(x64 + 'a64_emit_x64.cpp', drop_all)
sedit(x64 + 'a64_emit_x64.h', lambda s: s if 'DropAllBlocks' in s else rep(
    s, '    void AddDispatchTable(void* table);', '    void AddDispatchTable(void* table);\n    void DropAllBlocks(); // (shared: every block forgotten, its code left)'))
print('dynarmic: one shared code cache (droid_shared_code) applied')


# Warm start (Hydroid's warm threads translate last launch's blocks ahead into the shared code
# cache): the translate-time report also gives the block's FPCR (part of its key)
def fpcr_arg(s):
    if 'droid_translate_ns(uint64_t ns, uint64_t pc, uint64_t fpcr)' in s:
        return s
    s = rep(s, 'extern "C" void droid_translate_ns(uint64_t ns, uint64_t pc); // arm_cpu.cpp',
            'extern "C" void droid_translate_ns(uint64_t ns, uint64_t pc, uint64_t fpcr); // arm_cpu.cpp')
    return rep(s, '            A64::LocationDescriptor{current_location}.PC());',
               '            A64::LocationDescriptor{current_location}.PC(), A64::LocationDescriptor{current_location}.FPCR().Value());')


sedit(x64 + 'a64_interface.cpp', fpcr_arg)
print('dynarmic: translate report with the FPCR applied')


# Warm start's translations give way: a game thread waiting for the shared code cache goes first
# (two warm threads held the lock nearly all the time: Crossy Road's first frame 7.8 s -> 19.6 s)
def game_first(s):
    if 'backend.want' in s:
        return s
    s = rep(s, '        std::recursive_mutex m;\n        CodeHome(Impl* impl',
            '        std::recursive_mutex m;\n        std::atomic<int> want{0}; // game threads waiting for m (Precompile waits for 0)\n        CodeHome(Impl* impl')
    s = rep(s, '''    void Precompile(u64 pc) {
        GetBlock(IR::LocationDescriptor{A64::LocationDescriptor{pc, FP::FPCR{jit_state.fpcr}, false}});
    }''', '''    void Precompile(u64 pc) {
        // (translate-ahead: only while no game thread is waiting for the code cache)
        while (backend.want.load(std::memory_order_relaxed) > 0) std::this_thread::yield();
        in_precompile = true;
        GetBlock(IR::LocationDescriptor{A64::LocationDescriptor{pc, FP::FPCR{jit_state.fpcr}, false}});
        in_precompile = false;
    }
    bool in_precompile = false;''')
    s = rep(s, '''        std::lock_guard<std::recursive_mutex> backend_lock(backend.m); // (one code cache, many threads)
        if (auto block = emitter.GetBasicBlock(current_location))''', '''        if (!in_precompile) backend.want.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::recursive_mutex> backend_lock(backend.m); // (one code cache, many threads)
        if (!in_precompile) backend.want.fetch_sub(1, std::memory_order_relaxed);
        if (auto block = emitter.GetBasicBlock(current_location))''')
    if '#include <thread>' not in s:
        s = s.replace('#include <mutex>', '#include <mutex>\n#include <thread>', 1)
    if '#include <atomic>' not in s:
        s = s.replace('#include <mutex>', '#include <atomic>\n#include <mutex>', 1)
    return s


sedit(x64 + 'a64_interface.cpp', game_first)
print('dynarmic: warm translations give way to game threads applied')


# Parallel translate-ahead (Hydroid: PARALLEL_WARM_GAMES): UserConfig::droid_workshop gives a warm
# thread's Jit a workshop - its own code cache, emitter and lock - so warm threads translate in
# parallel instead of taking turns at the shared cache's one lock. A directory of every block made
# (any home) is looked in before translating; at an invalidation the workshops are dropped too.
def workshops_config(s):
    if 'droid_workshop' in s:
        return s
    return rep(s, '    bool droid_shared_code = false;\n', '''    bool droid_shared_code = false;

    /// Android-on-PS5, with droid_shared_code: > 0 makes this Jit a translate-ahead workshop - its own
    /// code cache, emitter and lock (code_cache_size), so workshops translate in parallel; what it
    /// makes goes in the directory every shared Jit looks in before translating. Its callbacks object
    /// must never run code and live as long as the process (a warm thread's).
    int droid_workshop = 0;
''')


def workshops_emit_h(s):
    if 'ClearDispatchTables' in s:
        return s
    return rep(s, '    void DropAllBlocks(); // (shared: every block forgotten, its code left)',
               '    void DropAllBlocks(); // (shared: every block forgotten, its code left)\n'
               "    void ClearDispatchTables() { ClearFastDispatchTable(); } // (every Jit's: they may hold a workshop's blocks)")


def workshops(s):
    if 'droid_workshop' in s:
        return s
    s = rep(s, '#include <mutex>\n#include <thread>\n',
            '#include <mutex>\n#include <shared_mutex>\n#include <thread>\n#include <unordered_map>\n#include <vector>\n')
    s = rep(s, 'namespace Dynarmic::A64 {\n\nusing namespace Backend::X64;', '''namespace Dynarmic::A64 {

using namespace Backend::X64;

// ---- Android-on-PS5: translate-ahead workshops (UserConfig::droid_workshop) ----------------
// Each workshop has its own code cache, emitter and lock, so warm threads translate in parallel
// (the shared code cache takes one translation at a time). The directory holds every block any
// home made: a shared Jit looks there before translating, a workshop before making one again.
// At a cache invalidation the workshops' blocks are dropped and the directory emptied.
// Lock order: a home's lock, then dir_m (never the other way).
namespace {
std::shared_mutex dir_m;
std::unordered_map<u64, CodePtr>& Directory() {
    static std::unordered_map<u64, CodePtr> d;
    return d;
}
std::atomic<bool> dir_used{false}; // a workshop exists
CodePtr DirFind(u64 key) {
    std::shared_lock g(dir_m);
    const auto& d = Directory();
    const auto it = d.find(key);
    return it == d.end() ? nullptr : it->second;
}
void DirAdd(u64 key, CodePtr p) {
    std::unique_lock g(dir_m);
    Directory().emplace(key, p);
}
}  // namespace''')
    s = rep(s, '''        FillSlots(c);
        if (!c.droid_shared_code) {''', '''        FillSlots(c);
        if (c.droid_shared_code && c.droid_workshop > 0) { // a workshop: its own home, code made shareable
            own_backend = std::make_unique<CodeHome>(this, jit, c);
            {
                std::lock_guard<std::mutex> g(WorkshopsM());
                Workshops().push_back(own_backend.get());
            }
            dir_used = true;
            return *own_backend;
        }
        if (!c.droid_shared_code) {''')
    s = rep(s, '''    void Precompile(u64 pc) {
        // (translate-ahead: only while no game thread is waiting for the code cache)
        while (backend.want.load(std::memory_order_relaxed) > 0) std::this_thread::yield();''', '''    // the workshops (their homes live as long as the process: warm threads' Jits are kept)
    static std::mutex& WorkshopsM() {
        static std::mutex m;
        return m;
    }
    static std::vector<CodeHome*>& Workshops() {
        static std::vector<CodeHome*> w;
        return w;
    }
    // a cache invalidation in the shared home: every workshop's blocks dropped, the directory emptied,
    // every Jit's fast dispatch table cleared (they may point into a workshop). Called holding the
    // shared home's lock (shared -> workshop -> dir_m: the lock order).
    void DropWorkshops() {
        if (!dir_used.load(std::memory_order_relaxed)) return;
        {
            std::lock_guard<std::mutex> g(WorkshopsM());
            for (CodeHome* w : Workshops()) {
                std::lock_guard<std::recursive_mutex> wg(w->m);
                w->emitter.DropAllBlocks();
            }
        }
        {
            std::unique_lock g(dir_m);
            Directory().clear();
        }
        emitter.ClearDispatchTables();
    }
    bool workshop_full = false;

    void Precompile(u64 pc) {
        // (translate-ahead: only while no game thread is waiting for the code cache; a workshop's is its own)
        while (conf.droid_workshop == 0 && backend.want.load(std::memory_order_relaxed) > 0) std::this_thread::yield();
        if (workshop_full) return;''')
    s = rep(s, '''    CodePtr GetBlock(IR::LocationDescriptor current_location) {
        if (!in_precompile) backend.want.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::recursive_mutex> backend_lock(backend.m); // (one code cache, many threads)
        if (!in_precompile) backend.want.fetch_sub(1, std::memory_order_relaxed);
        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;

        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
''', '''    CodePtr GetBlock(IR::LocationDescriptor current_location) {
        const bool workshop = conf.droid_workshop > 0;
        if (!in_precompile && !workshop) backend.want.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::recursive_mutex> backend_lock(backend.m); // (one code cache, many threads)
        if (!in_precompile && !workshop) backend.want.fetch_sub(1, std::memory_order_relaxed);
        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;
        const bool dir = dir_used.load(std::memory_order_relaxed);
        if (dir) // made already, in a workshop (or, for a workshop, in the shared home)
            if (const CodePtr p = DirFind(current_location.Value())) return p;

        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (workshop && block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            if (!workshop_full) droid_log("arm: workshop %d is full: it stops translating ahead", conf.droid_workshop);
            workshop_full = true;
            return nullptr; // (only Precompile asks a workshop)
        }
''')
    s = rep(s, '''            A64::LocationDescriptor{current_location}.PC(), A64::LocationDescriptor{current_location}.FPCR().Value());
        return entry;''', '''            A64::LocationDescriptor{current_location}.PC(), A64::LocationDescriptor{current_location}.FPCR().Value());
        if (dir) DirAdd(current_location.Value(), entry);
        return entry;''')
    s = rep(s, '''                emitter.DropAllBlocks();
            } else if (invalidate_entire_cache) {''', '''                emitter.DropAllBlocks();
                DropWorkshops();
            } else if (invalidate_entire_cache) {''')
    s = rep(s, '''                emitter.InvalidateCacheRanges(invalid_cache_ranges);
            }''', '''                emitter.InvalidateCacheRanges(invalid_cache_ranges);
                if (conf.droid_shared_code) DropWorkshops(); // (their copies of those blocks too)
            }''')
    return s


sedit(sys.argv[1] + '/src/dynarmic/interface/A64/config.h', workshops_config)
sedit(x64 + 'a64_emit_x64.h', workshops_emit_h)
sedit(x64 + 'a64_interface.cpp', workshops)
print('dynarmic: parallel translate-ahead workshops applied')
