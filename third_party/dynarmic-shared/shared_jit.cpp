// shared_jit.cpp — see shared_jit.h

#include "dynarmic/backend/x64/shared_jit.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace Dynarmic::Backend::X64::Shared {

namespace {
std::shared_mutex m; // (read at every callback a translation emits, by workshops in parallel)
std::unordered_map<std::uint64_t, int>& Slots() {
    static std::unordered_map<std::uint64_t, int> slots;
    return slots;
}
}  // namespace

int ArgSlot(std::uint64_t arg) {
    std::shared_lock g(m);
    const auto it = Slots().find(arg);
    return it == Slots().end() ? -1 : it->second;
}

void RegisterArg(std::uint64_t arg, int jit_state_offset) {
    std::unique_lock g(m);
    Slots()[arg] = jit_state_offset;
}

}  // namespace Dynarmic::Backend::X64::Shared
