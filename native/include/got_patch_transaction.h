#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

namespace swipegate {

enum class PointerPatchResult { Installed, NotInstalled, RollbackFailed };

// The caller serializes its own installations. CAS avoids overwriting a
// concurrent foreign slot value, but cannot coordinate another module's page
// permission changes. On uncertainty, stop installing instead of retrying.
// publishOriginal must retain the upstream entry even after a clean rollback:
// another thread may already be executing the replacement.
template <size_t Capacity, typename QueryProtection, typename SetProtection, typename PublishOriginal>
PointerPatchResult patchPointerSlots(const std::array<uintptr_t, Capacity> &slots,
        size_t count, void *replacement, QueryProtection queryProtection,
        SetProtection setProtection, PublishOriginal publishOriginal) {
    struct Entry {
        uintptr_t slot = 0;
        void *previous = nullptr;
        int protection = 0;
        bool changed = false;
        bool permissionDirty = false;
    };
    if (count == 0 || count > Capacity || replacement == nullptr) return PointerPatchResult::NotInstalled;
    std::array<Entry, Capacity> entries{};
    void *upstream = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (slots[i] == 0 || slots[i] % alignof(void *) != 0) return PointerPatchResult::NotInstalled;
        const int protection = queryProtection(slots[i]);
        if ((protection & PROT_READ) == 0) return PointerPatchResult::NotInstalled;
        void *previous = __atomic_load_n(reinterpret_cast<void **>(slots[i]), __ATOMIC_ACQUIRE);
        if (previous == nullptr || previous == replacement) return PointerPatchResult::NotInstalled;
        if (i == 0) upstream = previous;
        if (previous != upstream) return PointerPatchResult::NotInstalled;
        entries[i] = {slots[i], previous, protection, false, false};
    }
    if (!publishOriginal(upstream)) return PointerPatchResult::NotInstalled;

    auto rollback = [&](size_t attempted) {
        bool clean = true;
        while (attempted > 0) {
            Entry &entry = entries[--attempted];
            if (entry.changed) {
                if (setProtection(entry.slot, entry.protection | PROT_WRITE)) {
                    entry.permissionDirty = true;
                    void *expected = replacement;
                    if (!__atomic_compare_exchange_n(reinterpret_cast<void **>(entry.slot),
                            &expected, entry.previous, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
                            && expected != entry.previous) {
                        clean = false;
                    }
                } else clean = false;
            }
            if (entry.permissionDirty && !setProtection(entry.slot, entry.protection)) clean = false;
        }
        return clean ? PointerPatchResult::NotInstalled : PointerPatchResult::RollbackFailed;
    };

    for (size_t i = 0; i < count; ++i) {
        Entry &entry = entries[i];
        if (!setProtection(entry.slot, entry.protection | PROT_WRITE)) return rollback(i);
        entry.permissionDirty = true;
        void *expected = entry.previous;
        entry.changed = __atomic_compare_exchange_n(reinterpret_cast<void **>(entry.slot),
                &expected, replacement, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        if (!entry.changed) return rollback(i + 1);
        if (!setProtection(entry.slot, entry.protection)) return rollback(i + 1);
        entry.permissionDirty = false;
    }
    return PointerPatchResult::Installed;
}
}  // namespace swipegate
