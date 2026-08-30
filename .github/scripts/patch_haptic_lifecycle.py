from pathlib import Path

p = Path('native/src/main.cpp')
s = p.read_text()

old = '''AuxHookHealth gBackInvokeHapticHookHealth{};
AuxHookHealth gHapticCaptureHookHealth{};
'''
new = '''AuxHookHealth gSwipeStartHapticHookHealth{};
AuxHookHealth gBackInvokeHapticHookHealth{};
AuxHookHealth gBackCancelledHapticHookHealth{};
AuxHookHealth gHapticCaptureHookHealth{};
'''
if s.count(old) != 1:
    raise SystemExit(f'health declarations mismatch: {s.count(old)}')
s = s.replace(old, new, 1)

start = s.find('bool installAuxGestureHapticHooks(const LibraryInfo &library) {')
end = s.find('\nfloat gateHorizontalDistance(', start)
if start < 0 or end < 0:
    raise SystemExit('installAuxGestureHapticHooks block not found')

replacement = r'''bool ensureAuxGestureHapticHook(
        AuxHookHealth &health,
        uintptr_t target,
        void *replacement,
        void **originalSlot,
        const char *label) {
    if (target == 0 || replacement == nullptr || originalSlot == nullptr || gHookFunction == nullptr) {
        return false;
    }

    if (health.target != 0 && health.target != target) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC lifecycle target changed stage=%s old=%p new=%p; resetting hook health",
                label, reinterpret_cast<void *>(health.target), reinterpret_cast<void *>(target));
        health = AuxHookHealth{};
        __atomic_store_n(originalSlot, nullptr, __ATOMIC_RELEASE);
    }

    if (health.target == target && health.patchReady) {
        std::array<uint8_t, kHookProbeSize> current{};
        if (!readProbeHead(target, current)) return false;
        if (probeEquals(current, health.patchHead)) return true;
        if (!health.originalReady || !probeEquals(current, health.originalHead)) {
            logLine(ANDROID_LOG_ERROR,
                    "HAPTIC lifecycle foreign patch stage=%s target=%p current=%s expectedPatch=%s",
                    label, reinterpret_cast<void *>(target), probeHex(current).c_str(),
                    probeHex(health.patchHead).c_str());
            return false;
        }
        if (gUnhookFunction == nullptr) return false;

        __atomic_store_n(originalSlot, nullptr, __ATOMIC_RELEASE);
        const int unhookRc = gUnhookFunction(reinterpret_cast<void *>(target));
        std::array<uint8_t, kHookProbeSize> afterUnhook{};
        if (!readProbeHead(target, afterUnhook)
                || !probeEquals(afterUnhook, health.originalHead)) {
            logLine(ANDROID_LOG_ERROR,
                    "HAPTIC lifecycle repair unhook failed stage=%s rc=%d target=%p",
                    label, unhookRc, reinterpret_cast<void *>(target));
            return false;
        }
        health.patchReady = false;
        const uint64_t repairs = gHapticHookRepairCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        logLine(ANDROID_LOG_WARN,
                "HAPTIC lifecycle hook restored by runtime stage=%s; rehooking repair=%llu",
                label, static_cast<unsigned long long>(repairs));
    }

    std::array<uint8_t, kHookProbeSize> original{};
    if (!readProbeHead(target, original)) return false;
    if (!health.originalReady) {
        health.target = target;
        health.originalHead = original;
        health.originalReady = true;
    } else if (!probeEquals(original, health.originalHead)) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC lifecycle install refused stage=%s target=%p current=%s expectedOriginal=%s",
                label, reinterpret_cast<void *>(target), probeHex(original).c_str(),
                probeHex(health.originalHead).c_str());
        return false;
    }

    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(target), replacement, &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC lifecycle hook install failed stage=%s rc=%d target=%p",
                label, rc, reinterpret_cast<void *>(target));
        return false;
    }
    __atomic_store_n(originalSlot, backup, __ATOMIC_RELEASE);

    std::array<uint8_t, kHookProbeSize> patched{};
    if (!readProbeHead(target, patched) || probeEquals(patched, health.originalHead)) {
        __atomic_store_n(originalSlot, nullptr, __ATOMIC_RELEASE);
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC lifecycle hook did not patch stage=%s target=%p",
                label, reinterpret_cast<void *>(target));
        return false;
    }

    health.patchHead = patched;
    health.patchReady = true;
    logLine(ANDROID_LOG_INFO,
            "HAPTIC lifecycle hook ready stage=%s profile=%s target=%p repairs=%llu",
            label, gActivePatternName, reinterpret_cast<void *>(target),
            static_cast<unsigned long long>(gHapticHookRepairCount.load(std::memory_order_relaxed)));
    return true;
}

bool installAuxGestureHapticHooks(const LibraryInfo &library) {
    if (library.base == 0 || gHookFunction == nullptr) return false;

    const uintptr_t trackedBase = gAuxGestureScannedBase.load(std::memory_order_acquire);
    if (trackedBase != 0 && trackedBase != library.base) {
        gSwipeStartHapticHookHealth = AuxHookHealth{};
        gBackInvokeHapticHookHealth = AuxHookHealth{};
        gBackCancelledHapticHookHealth = AuxHookHealth{};
        gAuxGestureScannedBase.store(0, std::memory_order_release);
        __atomic_store_n(&gSwipeGateOriginalOnSwipeStart, nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&gSwipeGateOriginalOnBackCancelled, nullptr, __ATOMIC_RELEASE);
    }

    const uintptr_t startTarget = resolveAuxForActiveProfile(
            library, kOnSwipeStartPatternV1, sizeof(kOnSwipeStartPatternV1),
            kOnSwipeStartPatternV2, sizeof(kOnSwipeStartPatternV2));
    const uintptr_t invokeTarget = resolveAuxForActiveProfile(
            library, kOnBackInvokePatternV1, sizeof(kOnBackInvokePatternV1),
            kOnBackInvokePatternV2, sizeof(kOnBackInvokePatternV2));
    const uintptr_t cancelledTarget = resolveAuxForActiveProfile(
            library, kOnBackCancelledPatternV1, sizeof(kOnBackCancelledPatternV1),
            kOnBackCancelledPatternV2, sizeof(kOnBackCancelledPatternV2));

    if (startTarget == 0) {
        logLine(ANDROID_LOG_WARN, "HAPTIC lifecycle hook unresolved stage=swipe-start profile=%s",
                gActivePatternName);
    }
    if (invokeTarget == 0) {
        logLine(ANDROID_LOG_WARN, "HAPTIC lifecycle hook unresolved stage=back-invoke profile=%s",
                gActivePatternName);
    }
    if (cancelledTarget == 0) {
        logLine(ANDROID_LOG_WARN, "HAPTIC lifecycle hook unresolved stage=back-cancelled profile=%s",
                gActivePatternName);
    }

    const bool startReady = startTarget != 0 && ensureAuxGestureHapticHook(
            gSwipeStartHapticHookHealth, startTarget,
            reinterpret_cast<void *>(swipegate_on_swipe_start_hook),
            &gSwipeGateOriginalOnSwipeStart, "swipe-start");
    const bool invokeReady = invokeTarget != 0 && ensureAuxGestureHapticHook(
            gBackInvokeHapticHookHealth, invokeTarget,
            reinterpret_cast<void *>(swipegate_on_back_invoke_hook),
            &gSwipeGateOriginalOnBackInvoke, "back-invoke");
    const bool cancelledReady = cancelledTarget != 0 && ensureAuxGestureHapticHook(
            gBackCancelledHapticHookHealth, cancelledTarget,
            reinterpret_cast<void *>(swipegate_on_back_cancelled_hook),
            &gSwipeGateOriginalOnBackCancelled, "back-cancelled");

    if (startReady || invokeReady || cancelledReady) {
        gAuxGestureScannedBase.store(library.base, std::memory_order_release);
    }
    return startReady && invokeReady && cancelledReady;
}
'''

s = s[:start] + replacement + s[end:]

old = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_swipe_start() {
    gGestureActive.store(true, std::memory_order_release);
    gReadyHapticLatched.store(false, std::memory_order_release);
}
'''
new = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_swipe_start() {
    gGestureActive.store(true, std::memory_order_release);
    gReadyHapticLatched.store(false, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_DEBUG, "HAPTIC lifecycle stage=swipe-start readyLatch=0");
}
'''
if s.count(old) != 1:
    raise SystemExit(f'swipe-start callback mismatch: {s.count(old)}')
s = s.replace(old, new, 1)

old = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {
    gGestureActive.store(false, std::memory_order_release);
    performReturnHaptic("commit", false);
    gReadyHapticLatched.store(false, std::memory_order_release);
}
'''
new = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {
    gGestureActive.store(false, std::memory_order_release);
    performReturnHaptic("commit", false);
    gReadyHapticLatched.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_DEBUG, "HAPTIC lifecycle stage=back-invoke readyLatch=0");
}
'''
if s.count(old) != 1:
    raise SystemExit(f'back-invoke callback mismatch: {s.count(old)}')
s = s.replace(old, new, 1)

old = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_cancelled() {
    gGestureActive.store(false, std::memory_order_release);
    gReadyHapticLatched.store(false, std::memory_order_release);
}
'''
new = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_cancelled() {
    gGestureActive.store(false, std::memory_order_release);
    gReadyHapticLatched.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_DEBUG, "HAPTIC lifecycle stage=back-cancelled readyLatch=0");
}
'''
if s.count(old) != 1:
    raise SystemExit(f'back-cancelled callback mismatch: {s.count(old)}')
s = s.replace(old, new, 1)

p.write_text(s)
