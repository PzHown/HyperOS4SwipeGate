#include "got_patch_transaction.h"
#include <array>
#include <cstdlib>
#include <iostream>

using swipegate::PointerPatchResult;
namespace {
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) { std::cerr << message << '\n'; std::abort(); }
}
int upstreamObject, replacementObject, foreignObject;
void *upstream = &upstreamObject;
void *replacement = &replacementObject;
void *foreign = &foreignObject;
struct Fixture {
    std::array<void *, 2> values{upstream, upstream};
    std::array<uintptr_t, 2> slots{reinterpret_cast<uintptr_t>(&values[0]), reinterpret_cast<uintptr_t>(&values[1])};
    std::array<int, 2> protections{PROT_READ, PROT_READ};
    void *published = nullptr;
    int calls = 0;
    int failCall = -1;
    bool failRollback = false;
    bool interfere = false;
    PointerPatchResult run() {
        return swipegate::patchPointerSlots(slots, slots.size(), replacement,
            [&](uintptr_t slot) { return protections[slot == slots[0] ? 0 : 1]; },
            [&](uintptr_t slot, int protection) {
                ++calls;
                if (calls == failCall || (failRollback && calls > failCall)) return false;
                if (interfere && calls == 3) values[1] = foreign;
                protections[slot == slots[0] ? 0 : 1] = protection;
                return true;
            },
            [&](void *previous) {
                check(values[0] == upstream, "upstream published before any pointer replacement");
                published = previous;
                return true;
            });
    }
};
}
int main() {
    {
        Fixture f;
        check(f.run() == PointerPatchResult::Installed, "installation succeeds");
        check(f.published == upstream, "actual previous hook retained, not replaced by libc symbol");
        check(f.values[0] == replacement && f.values[1] == replacement, "all slots patched");
        check(f.protections[0] == PROT_READ && f.protections[1] == PROT_READ, "protections restored");
    }
    for (int failure = 1; failure <= 4; ++failure) {
        Fixture f; f.failCall = failure;
        check(f.run() == PointerPatchResult::NotInstalled, "each partial failure rolls back");
        check(f.values[0] == upstream && f.values[1] == upstream, "rollback restores both pointers");
        check(f.protections[0] == PROT_READ && f.protections[1] == PROT_READ, "rollback restores both protections");
        check(f.published == upstream, "upstream remains valid for an in-flight replacement call");
    }
    {
        Fixture f; f.values[1] = foreign;
        check(f.run() == PointerPatchResult::NotInstalled, "inconsistent previous chains rejected before writing");
        check(f.calls == 0 && f.published == nullptr, "preflight is nonmutating");
    }
    {
        Fixture f; f.values[0] = replacement;
        check(f.run() == PointerPatchResult::NotInstalled, "self-recursive original rejected");
        check(f.calls == 0, "self-hook causes no permission mutation");
    }
    {
        Fixture f; f.failCall = 2; f.failRollback = true;
        check(f.run() == PointerPatchResult::RollbackFailed, "unrecoverable permission failure explicitly reported");
        check(f.published == upstream, "failed rollback still retains valid upstream");
    }
    {
        Fixture f; f.interfere = true;
        check(f.run() == PointerPatchResult::NotInstalled, "concurrent foreign slot wins compare-and-exchange");
        check(f.values[0] == upstream && f.values[1] == foreign, "rollback never clobbers newer foreign hook");
        check(f.protections[0] == PROT_READ && f.protections[1] == PROT_READ, "permissions restored after CAS failure");
    }
    std::cout << "PASS: " << checks << " GOT transaction assertions\n";
}
