// shared_jit.h — Android-on-PS5: one code cache for every guest thread (after Ryujinx's
// ARMeilleure, MIT, which keeps one translator per process). dynarmic bakes per-thread pointers
// into its x86 (the callbacks' `this`, the Jit, TLS, the exclusive monitor's slot, the fast
// dispatch table); here they are read through r15 (the thread's JitState) instead, so the same
// code runs on any thread, and links between blocks are patched with single 8-byte atomic writes.
#pragma once

#include <cstdint>

namespace Dynarmic::Backend::X64::Shared {

// a pointer the emitter would bake in that is really per thread: its slot in the JitState (-1: none)
int ArgSlot(std::uint64_t arg);
void RegisterArg(std::uint64_t arg, int jit_state_offset);

}  // namespace Dynarmic::Backend::X64::Shared
