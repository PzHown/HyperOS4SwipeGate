from pathlib import Path


def one(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 match, got {n}")
    return text.replace(old, new, 1)


p = Path("native/src/main.cpp")
s = p.read_text()

s = one(
    s,
    '__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke();\n',
    '__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke();\n'
    '__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke_exit();\n',
    "extern exit",
)

s = one(
    s,
    'constexpr int64_t kHapticConfirmSuppressMs = 1000;\n',
    'constexpr int64_t kReadyReleaseDedupMs = 140;\n',
    "dedup window",
)

anchor = 'std::atomic<int64_t> gLastInjectedHapticAtMs{0};\n'
s = one(
    s,
    anchor,
    anchor
    + 'std::atomic<uintptr_t> gGetGlobalRuntime{0};\n'
    + 'std::atomic<void *> gRuntimeDecStrong{nullptr};\n'
    + 'std::atomic<bool> gHapticRuntimeBridgeResolved{false};\n'
    + 'std::atomic<int64_t> gLastReadyHapticAtMs{0};\n'
    + 'std::atomic<bool> gReadyReleaseDedupEligible{false};\n'
    + 'thread_local bool gReleaseDedupArmed = false;\n'
    + 'thread_local bool gReleaseHapticSuppressed = false;\n',
    "runtime bridge state",
)

old_hook_block = '''    // The module's injected threshold haptic calls the trampoline directly, so only Xiaomi's
    // native haptics arrive here.  Suppress at most one native haptic when it follows our
    // first-segment feedback within a very short window and the gesture is still in segment 1.
    int64_t suppressUntil = gNativeHapticSuppressUntilMs.load(std::memory_order_acquire);
    if (suppressUntil > 0) {
        if (now <= suppressUntil
                && gHapticGestureSegment.load(std::memory_order_acquire) == 1) {
            if (gNativeHapticSuppressUntilMs.compare_exchange_strong(
                    suppressUntil, 0, std::memory_order_acq_rel)) {
                const int64_t injectedAt = gLastInjectedHapticAtMs.load(std::memory_order_acquire);
                logLine(ANDROID_LOG_INFO,
                        "HAPTIC_V2 native feedback suppressed reason=near-threshold-confirm constant=%d deltaMs=%lld windowMs=%lld",
                        constant,
                        injectedAt > 0 && now >= injectedAt
                                ? static_cast<long long>(now - injectedAt) : -1LL,
                        static_cast<long long>(kHapticConfirmSuppressMs));
                return;
            }
        } else if (now > suppressUntil) {
            gNativeHapticSuppressUntilMs.compare_exchange_strong(
                    suppressUntil, 0, std::memory_order_acq_rel);
        }
    }

    original(storage, constant);
'''
new_hook_block = '''    // Only a stock Release haptic inside the on_back_invoke scope can be deduplicated.
    // Threshold / Three-hold feedback is outside this scope and is never touched.
    if (gReleaseDedupArmed && constant == kHapticConstant) {
        gReleaseDedupArmed = false;
        gReleaseHapticSuppressed = true;
        const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release suppressed reason=ready-release-dedup constant=%d deltaMs=%lld windowMs=%lld",
                constant,
                readyAt > 0 && now >= readyAt ? static_cast<long long>(now - readyAt) : -1LL,
                static_cast<long long>(kReadyReleaseDedupMs));
        return;
    }

    original(storage, constant);
'''
s = one(s, old_hook_block, new_hook_block, "release-only dispatch hook")

insert_before = 'bool performNativeHaptic(const char *stage) {\n'
bridge = r'''using GetGlobalRuntimeFn = void *(*)();
using RuntimeDecStrongFn = void (*)(void *);

bool decodeBlTarget(uintptr_t pc, uint32_t insn, uintptr_t *target) {
    if (target == nullptr || (insn & 0xfc000000u) != 0x94000000u) return false;
    int64_t imm26 = static_cast<int64_t>(insn & 0x03ffffffu);
    if (imm26 & 0x02000000LL) imm26 |= ~0x03ffffffLL;
    *target = static_cast<uintptr_t>(static_cast<int64_t>(pc) + imm26 * 4LL);
    return true;
}

bool pltUsesSlot(const LibraryInfo &library, uintptr_t plt, uintptr_t slot) {
    if (plt == 0 || slot == 0 || !libraryContainsRange(library, plt, 16)) return false;
    uint32_t i[4]{};
    std::memcpy(i, reinterpret_cast<const void *>(plt), sizeof(i));
    if ((i[0] & 0x9f00001fu) != 0x90000010u) return false; // adrp x16
    if ((i[2] & 0xffc003ffu) != 0x91000210u) return false; // add x16,x16,#imm
    if (i[3] != 0xd61f0220u) return false;                 // br x17
    int64_t imm21 = (static_cast<int64_t>((i[0] >> 5) & 0x7ffffu) << 2)
            | static_cast<int64_t>((i[0] >> 29) & 3u);
    if (imm21 & (1LL << 20)) imm21 |= ~((1LL << 21) - 1LL);
    const uintptr_t page = static_cast<uintptr_t>(
            static_cast<int64_t>(plt & ~static_cast<uintptr_t>(0xfff)) + imm21 * 4096LL);
    const uintptr_t off = static_cast<uintptr_t>((i[2] >> 10) & 0xfffu)
            << (((i[2] >> 22) & 1u) ? 12 : 0);
    return page + off == slot;
}

bool resolveHyperRtRuntimeBridge(const LibraryInfo &library) {
    if (gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) return true;
    const ImportedFunctionResolution haptic = resolveImportedFunction(library, kHapticSymbol);
    const ImportedFunctionResolution dec = resolveImportedFunction(library, "Runtime_dec_strong");
    if (haptic.matches != 1 || haptic.slot == 0
            || dec.matches != 1 || dec.slot == 0 || dec.target == nullptr) return false;

    uintptr_t runtimeTarget = 0;
    size_t hits = 0;
    for (size_t r = 0; r < library.executableRangeCount; ++r) {
        const ExecutableRange &range = library.executableRanges[r];
        if (range.start == 0 || range.size < 36) continue;
        const uintptr_t first = (range.start + 23u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - 12u;
        for (uintptr_t pc = first; pc <= last; pc += 4u) {
            uint32_t call = 0;
            std::memcpy(&call, reinterpret_cast<const void *>(pc), 4);
            uintptr_t hapticPlt = 0;
            if (!decodeBlTarget(pc, call, &hapticPlt) || !pltUsesSlot(library, hapticPlt, haptic.slot)) continue;

            uint32_t movW1 = 0, save = 0, restore = 0, getCall = 0, decCall = 0;
            std::memcpy(&movW1, reinterpret_cast<const void *>(pc - 4), 4);
            std::memcpy(&save, reinterpret_cast<const void *>(pc - 16), 4);
            std::memcpy(&restore, reinterpret_cast<const void *>(pc + 4), 4);
            std::memcpy(&getCall, reinterpret_cast<const void *>(pc - 20), 4);
            std::memcpy(&decCall, reinterpret_cast<const void *>(pc + 8), 4);
            if (movW1 != 0x2a1f03e1u) continue; // mov w1,wzr
            if ((save & 0xffffffe0u) != 0xaa0003e0u) continue; // mov xN,x0
            const uint32_t reg = save & 0x1fu;
            if (restore != (0xaa0003e0u | (reg << 16))) continue; // mov x0,xN

            uintptr_t candidate = 0, decPlt = 0;
            if (!decodeBlTarget(pc - 20, getCall, &candidate)
                    || !decodeBlTarget(pc + 8, decCall, &decPlt)
                    || !pltUsesSlot(library, decPlt, dec.slot)
                    || !libraryContainsRange(library, candidate, 4)) continue;
            if (runtimeTarget != 0 && runtimeTarget != candidate) return false;
            runtimeTarget = candidate;
            ++hits;
        }
    }
    if (runtimeTarget == 0 || hits == 0) return false;
    gGetGlobalRuntime.store(runtimeTarget, std::memory_order_release);
    gRuntimeDecStrong.store(dec.target, std::memory_order_release);
    gHapticRuntimeBridgeResolved.store(true, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 HyperRT runtime bridge resolved getRuntime=%p decStrong=%p stockCallsites=%zu",
            reinterpret_cast<void *>(runtimeTarget), dec.target, hits);
    return true;
}

'''
s = one(s, insert_before, bridge + insert_before, "runtime bridge helper")

start = s.index('bool performNativeHaptic(const char *stage) {')
end = s.index('\nuintptr_t resolveUniqueAuxPattern(', start)
new_perform = r'''bool performNativeHaptic(const char *stage) {
    swipegate_control_sync_if_due();
    if (swipegate_control_haptic_enabled() != 1) return false;

    const auto haptic = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    const auto getRuntime = reinterpret_cast<GetGlobalRuntimeFn>(
            gGetGlobalRuntime.load(std::memory_order_acquire));
    const auto decStrong = reinterpret_cast<RuntimeDecStrongFn>(
            gRuntimeDecStrong.load(std::memory_order_acquire));
    if (haptic == nullptr || getRuntime == nullptr || decStrong == nullptr
            || !gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) {
        bool expected = false;
        if (gHapticUnavailableLogged.compare_exchange_strong(expected, true)) {
            logLine(ANDROID_LOG_WARN,
                    "HAPTIC_V2 skipped stage=%s reason=hyperrt-runtime-bridge-not-ready hook=%d bridge=%d",
                    stage == nullptr ? "unknown" : stage,
                    haptic != nullptr ? 1 : 0,
                    gHapticRuntimeBridgeResolved.load(std::memory_order_acquire) ? 1 : 0);
        }
        return false;
    }

    void *runtime = getRuntime();
    if (runtime == nullptr) return false;
    void *storage = runtime;
    haptic(&storage, kHapticConstant); // trampoline: bypass Release-only dispatch hook
    decStrong(runtime);

    const int64_t now = monotonicMs();
    gLastReadyHapticAtMs.store(now, std::memory_order_release);
    gReadyReleaseDedupEligible.store(true, std::memory_order_release);
    gLastInjectedHapticAtMs.store(now, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=hyperrt-stock constant=%d readyReleaseDedupMs=%lld",
            stage == nullptr ? "unknown" : stage, kHapticConstant,
            static_cast<long long>(kReadyReleaseDedupMs));
    return true;
}
'''
s = s[:start] + new_perform + s[end:]

old_seg = '''    if (hapticSegment != 1) {
        gNativeHapticSuppressUntilMs.store(0, std::memory_order_release);
    }
'''
new_seg = '''    if (hapticSegment == 2) {
        // Threshold / Three-hold remains 100% Xiaomi-owned and cancels Ready->Release dedup
        // until the gesture explicitly re-enters Ready.
        gReadyReleaseDedupEligible.store(false, std::memory_order_release);
    }
'''
s = one(s, old_seg, new_seg, "threshold unaffected")

s = one(
    s,
    '    // No module commit hook: preserve Xiaomi\'s native second-segment/commit haptic exactly once.\n    return true;\n',
    '    // on_back_invoke is only a scope marker for Ready->Release dedup; it does not create haptics.\n'
    '    installBackInvokeHapticHook(library);\n'
    '    return true;\n',
    "install release scope hook",
)

old_worker = '''            if (!gHapticCaptureHookInstalled.load(std::memory_order_acquire)) {
                const int64_t now = monotonicMs();
'''
new_worker = '''            if (!gHapticCaptureHookInstalled.load(std::memory_order_acquire)
                    || !gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) {
                const int64_t now = monotonicMs();
'''
s = one(s, old_worker, new_worker, "worker condition")

old_probe = '''                    if (!installHapticCaptureHookFromLauncherImport(
                            library, "watchdog-feature-probe")) {
                        gHapticResolveFailures.fetch_add(1, std::memory_order_relaxed);
                    }
'''
new_probe = '''                    const bool hookReady = gHapticCaptureHookInstalled.load(std::memory_order_acquire)
                            || installHapticCaptureHookFromLauncherImport(
                                    library, "watchdog-feature-probe");
                    const bool runtimeReady = gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)
                            || resolveHyperRtRuntimeBridge(library);
                    if (!hookReady || !runtimeReady) {
                        gHapticResolveFailures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        gHapticResolveFailures.store(0, std::memory_order_release);
                    }
'''
s = one(s, old_probe, new_probe, "worker probe")

old_back = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {
    // Kept ABI-compatible with the assembly shim, but dev.8 no longer installs this hook.
    // If a stale in-process hook ever reaches it, only reset segment state; never replay commit.
    gHapticGestureSegment.store(0, std::memory_order_release);
}
'''
new_back = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {
    gReleaseDedupArmed = false;
    gReleaseHapticSuppressed = false;
    const bool eligible = gReadyReleaseDedupEligible.exchange(false, std::memory_order_acq_rel);
    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
    const int64_t now = monotonicMs();
    const int64_t delta = readyAt > 0 && now >= readyAt ? now - readyAt : -1;
    if (eligible && delta >= 0 && delta < kReadyReleaseDedupMs
            && gHapticCaptureHookInstalled.load(std::memory_order_acquire)) {
        gReleaseDedupArmed = true;
    }
}

__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke_exit() {
    gReleaseDedupArmed = false;
    gReleaseHapticSuppressed = false;
    gHapticGestureSegment.store(0, std::memory_order_release);
}
'''
s = one(s, old_back, new_back, "release scope lifecycle")

old_policy = '''            "HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved process-lifetime-arc retry-on-miss=1 confirm-dedup-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex",
            static_cast<long long>(kHapticConfirmSuppressMs));
'''
new_policy = '''            "HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 constant=0",
            static_cast<long long>(kReadyReleaseDedupMs));
'''
s = one(s, old_policy, new_policy, "policy log")

p.write_text(s)

asm = Path("native/src/swipe_hook_entry.S")
a = asm.read_text()
old_tail = '''    cbz x16, .Lback_invoke_done
    blr x16
.Lback_invoke_done:
    ldr x30, [sp, #0x48]
    add sp, sp, #0x110
    ret
'''
new_tail = '''    cbz x16, .Lback_invoke_after_original
    blr x16
.Lback_invoke_after_original:
    // Preserve the opaque return value while closing the thread-local Release dedup scope.
    str x0, [sp, #0xd0]
    str q0, [sp, #0xe0]
    bl swipegate_haptic_on_back_invoke_exit
    ldr q0, [sp, #0xe0]
    ldr x0, [sp, #0xd0]
    ldr x30, [sp, #0x48]
    add sp, sp, #0x110
    ret
'''
a = one(a, old_tail, new_tail, "assembly scope exit")
asm.write_text(a)

gradle = Path("app/build.gradle.kts")
g = gradle.read_text()
g = one(g, 'versionCode = 46', 'versionCode = 47', "versionCode")
g = one(g, 'versionName = "0.8.1-dev.4"', 'versionName = "0.8.1-dev.5"', "versionName")
gradle.write_text(g)
