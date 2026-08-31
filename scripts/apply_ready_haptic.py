from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    return text.replace(old, new, 1)


def replace_between(text: str, start: str, end: str, body: str, label: str) -> str:
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"{label}: start not found")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"{label}: end not found")
    return text[:a] + body + text[b:]


path = Path("native/src/main.cpp")
text = path.read_text()

text = replace_once(
    text,
    '__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke();\n',
    '__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke();\n'
    '__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke_exit();\n',
    "extern release exit",
)

text = replace_once(
    text,
    'constexpr int64_t kHapticConfirmSuppressMs = 1000;\n'
    'constexpr int32_t kHapticConstant = 0;\n'
    'constexpr uintptr_t kPointerAddressMask = 0x00ffffffffffffffull;\n',
    'constexpr int64_t kReadyReleaseDedupMs = 140;\n'
    'constexpr int32_t kHapticConstant = 0;\n',
    "haptic constants",
)

state_start = "// Haptic V2 is deliberately isolated from gHookMutex."
state_end = "std::mutex gHookMutex;"
new_state = '''// Haptic V2 is deliberately isolated from gHookMutex. The provider hook is retained only
// to suppress a stock Release haptic inside the on_back_invoke scope. Ready feedback itself
// uses Xiaomi's own HyperRT runtime acquisition path resolved from the stock call sequence.
std::atomic<void *> gOriginalHapticFeedback{nullptr};
std::atomic<uintptr_t> gGetGlobalRuntime{0};
std::atomic<void *> gRuntimeDecStrong{nullptr};
std::atomic<bool> gHapticInstallInProgress{false};
std::atomic<bool> gHapticCaptureHookInstalled{false};
std::atomic<bool> gHapticRuntimeBridgeResolved{false};
std::atomic<bool> gHapticUnavailableLogged{false};
std::atomic<int64_t> gLastHapticFeatureResolveMs{0};
std::atomic<uint32_t> gHapticResolveFailures{0};
std::atomic<int64_t> gLastReadyHapticAtMs{0};
std::atomic<bool> gReadyReleaseDedupEligible{false};
// 0 = outside/idle, 1 = Ready, 2 = Threshold/Three-hold.
std::atomic<int> gHapticGestureSegment{0};
std::atomic<uintptr_t> gBackInvokeTarget{0};
std::atomic<bool> gBackInvokeHookInstalled{false};
thread_local bool gReleaseDedupArmed = false;
thread_local bool gReleaseHapticSuppressed = false;

'''
text = replace_between(text, state_start, state_end, new_state, "haptic state")

haptic_start = "using HapticFeedbackFn = void (*)(void *, int32_t);"
haptic_end = "uintptr_t resolveUniqueAuxPattern("
new_haptic = r'''using HapticFeedbackFn = void (*)(void *, int32_t);
using GetGlobalRuntimeFn = void *(*)();
using RuntimeDecStrongFn = void (*)(void *);

bool decodeBlTarget(uintptr_t pc, uint32_t instruction, uintptr_t *target) {
    if (target == nullptr || (instruction & 0xfc000000u) != 0x94000000u) return false;
    int64_t imm26 = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((imm26 & 0x02000000LL) != 0) imm26 |= ~0x03ffffffLL;
    const int64_t delta = imm26 * 4LL;
    *target = static_cast<uintptr_t>(static_cast<int64_t>(pc) + delta);
    return true;
}

bool pltReferencesSlot(const LibraryInfo &library, uintptr_t plt, uintptr_t expectedSlot) {
    if (plt == 0 || expectedSlot == 0 || !libraryContainsRange(library, plt, 16)) return false;
    uint32_t insn[4]{};
    std::memcpy(insn, reinterpret_cast<const void *>(plt), sizeof(insn));

    if ((insn[0] & 0x9f00001fu) != 0x90000010u) return false;
    if ((insn[2] & 0xffc003ffu) != 0x91000210u) return false;
    if (insn[3] != 0xd61f0220u) return false;

    int64_t immhi = static_cast<int64_t>((insn[0] >> 5) & 0x7ffffu);
    const int64_t immlo = static_cast<int64_t>((insn[0] >> 29) & 0x3u);
    int64_t imm21 = (immhi << 2) | immlo;
    if ((imm21 & (1LL << 20)) != 0) imm21 |= ~((1LL << 21) - 1LL);
    const uintptr_t page = static_cast<uintptr_t>(
            static_cast<int64_t>(plt & ~static_cast<uintptr_t>(0xfff)) + imm21 * 4096LL);
    const uint32_t shift = (insn[2] >> 22) & 0x1u;
    const uintptr_t addImm = static_cast<uintptr_t>((insn[2] >> 10) & 0xfffu)
            << (shift != 0 ? 12 : 0);
    return page + addImm == expectedSlot;
}

bool resolveHyperRtRuntimeBridge(const LibraryInfo &library) {
    if (gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) return true;
    if (library.base == 0) return false;

    const ImportedFunctionResolution haptic = resolveImportedFunction(library, kHapticSymbol);
    const ImportedFunctionResolution decStrong = resolveImportedFunction(library, "Runtime_dec_strong");
    if (haptic.matches != 1 || haptic.slot == 0
            || decStrong.matches != 1 || decStrong.slot == 0 || decStrong.target == nullptr) {
        return false;
    }

    uintptr_t runtimeTarget = 0;
    size_t corroboratedCallsites = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < 36) continue;
        const uintptr_t start = (range.start + 20u + 3u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - 12u;
        for (uintptr_t pc = start; pc <= last; pc += 4u) {
            uint32_t callInsn = 0;
            std::memcpy(&callInsn, reinterpret_cast<const void *>(pc), sizeof(callInsn));
            uintptr_t callTarget = 0;
            if (!decodeBlTarget(pc, callInsn, &callTarget)
                    || !pltReferencesSlot(library, callTarget, haptic.slot)) continue;

            uint32_t movW1 = 0;
            uint32_t saveRuntime = 0;
            uint32_t restoreRuntime = 0;
            uint32_t runtimeCallInsn = 0;
            uint32_t decCallInsn = 0;
            std::memcpy(&movW1, reinterpret_cast<const void *>(pc - 4u), sizeof(movW1));
            std::memcpy(&saveRuntime, reinterpret_cast<const void *>(pc - 16u), sizeof(saveRuntime));
            std::memcpy(&restoreRuntime, reinterpret_cast<const void *>(pc + 4u), sizeof(restoreRuntime));
            std::memcpy(&runtimeCallInsn, reinterpret_cast<const void *>(pc - 20u), sizeof(runtimeCallInsn));
            std::memcpy(&decCallInsn, reinterpret_cast<const void *>(pc + 8u), sizeof(decCallInsn));

            if (movW1 != 0x2a1f03e1u) continue;
            if ((saveRuntime & 0xffffffe0u) != 0xaa0003e0u) continue;
            const uint32_t runtimeReg = saveRuntime & 0x1fu;
            if (restoreRuntime != (0xaa0003e0u | (runtimeReg << 16))) continue;

            uintptr_t candidateRuntime = 0;
            uintptr_t decPlt = 0;
            if (!decodeBlTarget(pc - 20u, runtimeCallInsn, &candidateRuntime)
                    || !decodeBlTarget(pc + 8u, decCallInsn, &decPlt)
                    || !pltReferencesSlot(library, decPlt, decStrong.slot)) continue;
            if (!libraryContainsRange(library, candidateRuntime, 4)) continue;

            if (runtimeTarget == 0) {
                runtimeTarget = candidateRuntime;
            } else if (runtimeTarget != candidateRuntime) {
                logLine(ANDROID_LOG_WARN,
                        "HAPTIC_V2 HyperRT runtime resolver ambiguous first=%p next=%p",
                        reinterpret_cast<void *>(runtimeTarget),
                        reinterpret_cast<void *>(candidateRuntime));
                return false;
            }
            ++corroboratedCallsites;
        }
    }

    if (runtimeTarget == 0 || corroboratedCallsites == 0) return false;
    gGetGlobalRuntime.store(runtimeTarget, std::memory_order_release);
    gRuntimeDecStrong.store(decStrong.target, std::memory_order_release);
    gHapticRuntimeBridgeResolved.store(true, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 HyperRT runtime bridge resolved getRuntime=%p decStrong=%p stockCallsites=%zu",
            reinterpret_cast<void *>(runtimeTarget), decStrong.target, corroboratedCallsites);
    return true;
}

void hapticFeedbackDispatchHook(void *storage, int32_t constant) {
    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    if (original == nullptr) return;

    if (gReleaseDedupArmed && constant == kHapticConstant) {
        gReleaseDedupArmed = false;
        gReleaseHapticSuppressed = true;
        const int64_t now = monotonicMs();
        const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release suppressed reason=ready-release-dedup constant=%d deltaMs=%lld windowMs=%lld",
                constant,
                readyAt > 0 && now >= readyAt ? static_cast<long long>(now - readyAt) : -1LL,
                static_cast<long long>(kReadyReleaseDedupMs));
        return;
    }
    original(storage, constant);
}

bool installHapticCaptureHookTarget(void *target, const char *source) {
    if (target == nullptr || gHookFunction == nullptr || !isLauncherProcess()) return false;
    if (gHapticCaptureHookInstalled.load(std::memory_order_acquire)) return true;

    bool expected = false;
    if (!gHapticInstallInProgress.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return gHapticCaptureHookInstalled.load(std::memory_order_acquire);
    }

    void *backup = nullptr;
    const int rc = gHookFunction(
            target, reinterpret_cast<void *>(hapticFeedbackDispatchHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC_V2 dispatch hook failed source=%s rc=%d target=%p backup=%p",
                source == nullptr ? "unknown" : source, rc, target, backup);
        gHapticInstallInProgress.store(false, std::memory_order_release);
        return false;
    }

    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gHapticResolveFailures.store(0, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);
    gHapticInstallInProgress.store(false, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 dispatch hook ready source=%s target=%p feature=%s release-dedup-only=1",
            source == nullptr ? "unknown" : source, target, kHapticSymbol);
    return true;
}

bool installHapticCaptureHookFromLauncherImport(const LibraryInfo &library, const char *source) {
    if (library.base == 0 || gHookFunction == nullptr || !isLauncherProcess()) return false;
    if (gHapticCaptureHookInstalled.load(std::memory_order_acquire)) return true;

    const ImportedFunctionResolution resolution = resolveImportedFunction(library, kHapticSymbol);
    if (resolution.matches != 1 || resolution.target == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 loaded-elf import unresolved source=%s symbol=%s matches=%zu launcher=%s",
                source == nullptr ? "unknown" : source, kHapticSymbol,
                resolution.matches, library.path.c_str());
        return false;
    }
    Dl_info providerInfo{};
    if (dladdr(resolution.target, &providerInfo) == 0
            || providerInfo.dli_fname == nullptr
            || std::strstr(providerInfo.dli_fname, kHapticProviderLibrary) == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 loaded-elf target rejected source=%s target=%p provider=%s",
                source == nullptr ? "unknown" : source, resolution.target,
                providerInfo.dli_fname == nullptr ? "<unknown>" : providerInfo.dli_fname);
        return false;
    }
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 loaded-elf import resolved source=%s symbol=%s reloc=%s got=%p target=%p provider=%s launcher=%s",
            source == nullptr ? "unknown" : source, kHapticSymbol,
            resolution.relocationKind == nullptr ? "unknown" : resolution.relocationKind,
            reinterpret_cast<void *>(resolution.slot), resolution.target, providerInfo.dli_fname,
            library.path.c_str());
    return installHapticCaptureHookTarget(resolution.target, "launcher-import-feature");
}

bool performNativeHaptic(const char *stage) {
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
                    "HAPTIC_V2 skipped stage=%s reason=hyperrt-runtime-bridge-not-ready hook=%d bridge=%d getRuntime=%p decStrong=%p",
                    stage == nullptr ? "unknown" : stage,
                    haptic == nullptr ? 0 : 1,
                    gHapticRuntimeBridgeResolved.load(std::memory_order_acquire) ? 1 : 0,
                    reinterpret_cast<void *>(getRuntime), reinterpret_cast<void *>(decStrong));
        }
        return false;
    }

    void *runtime = getRuntime();
    if (runtime == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 skipped stage=%s reason=get-global-runtime-null",
                stage == nullptr ? "unknown" : stage);
        return false;
    }
    void *storage = runtime;
    haptic(&storage, kHapticConstant);
    decStrong(runtime);

    const int64_t now = monotonicMs();
    gLastReadyHapticAtMs.store(now, std::memory_order_release);
    gReadyReleaseDedupEligible.store(true, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=hyperrt-stock constant=%d readyReleaseDedupMs=%lld",
            stage == nullptr ? "unknown" : stage, kHapticConstant,
            static_cast<long long>(kReadyReleaseDedupMs));
    return true;
}

'''
text = replace_between(text, haptic_start, haptic_end, new_haptic, "haptic implementation")

old_segment = '''    if (hapticSegment != 1) {
        gNativeHapticSuppressUntilMs.store(0, std::memory_order_release);
    }
    const int previousHapticSegment = gHapticGestureSegment.exchange(
            hapticSegment, std::memory_order_acq_rel);
    if (hapticSegment == 1 && previousHapticSegment != 1) {
        const char *stage = previousHapticSegment == 2 ? "return-first" : "first";
        if (!performNativeHaptic(stage)) {
            // Do not consume the segment transition when the feedback could not actually be
            // emitted.  Restoring the previous state lets a later frame retry as soon as the
            // capture hook/Arc becomes available instead of losing the first haptic forever.
            int expectedSegment = 1;
            if (gHapticGestureSegment.compare_exchange_strong(
                    expectedSegment, previousHapticSegment, std::memory_order_acq_rel)) {
                logLine(ANDROID_LOG_INFO,
                        "HAPTIC_V2 retry armed stage=%s previousSegment=%d reason=feedback-not-emitted",
                        stage, previousHapticSegment);
            }
        }
    }
'''
new_segment = '''    if (hapticSegment == 2) {
        // Threshold/Three-hold is Xiaomi-owned. Never suppress it and invalidate any Ready->Release
        // dedup eligibility until the gesture explicitly re-enters Ready.
        gReadyReleaseDedupEligible.store(false, std::memory_order_release);
    }
    const int previousHapticSegment = gHapticGestureSegment.exchange(
            hapticSegment, std::memory_order_acq_rel);
    if (hapticSegment == 1 && previousHapticSegment != 1) {
        const char *stage = previousHapticSegment == 2 ? "return-first" : "first";
        if (!performNativeHaptic(stage)) {
            gReadyReleaseDedupEligible.store(false, std::memory_order_release);
            int expectedSegment = 1;
            if (gHapticGestureSegment.compare_exchange_strong(
                    expectedSegment, previousHapticSegment, std::memory_order_acq_rel)) {
                logLine(ANDROID_LOG_INFO,
                        "HAPTIC_V2 retry armed stage=%s previousSegment=%d reason=feedback-not-emitted",
                        stage, previousHapticSegment);
            }
        }
    }
'''
text = replace_once(text, old_segment, new_segment, "gesture segment logic")

text = replace_once(
    text,
    "    // No module commit hook: preserve Xiaomi's native second-segment/commit haptic exactly once.\n"
    "    return true;\n",
    "    // Hook on_back_invoke only as a scope marker for Ready->Release dedup. Xiaomi still owns\n"
    "    // Threshold and Release generation; the hook never creates either feedback.\n"
    "    installBackInvokeHapticHook(library);\n"
    "    return true;\n",
    "install release scope hook",
)

text = replace_once(
    text,
    "    gBackInvokeHookInstalled.store(false, std::memory_order_release);\n"
    "    gBackInvokeTarget.store(0, std::memory_order_release);\n"
    "    __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);\n",
    "    gBackInvokeHookInstalled.store(false, std::memory_order_release);\n"
    "    gBackInvokeTarget.store(0, std::memory_order_release);\n"
    "    __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);\n"
    "    gGetGlobalRuntime.store(0, std::memory_order_release);\n"
    "    gRuntimeDecStrong.store(nullptr, std::memory_order_release);\n"
    "    gHapticRuntimeBridgeResolved.store(false, std::memory_order_release);\n"
    "    gReadyReleaseDedupEligible.store(false, std::memory_order_release);\n",
    "remap haptic reset",
)

worker_old = '''            if (!gHapticCaptureHookInstalled.load(std::memory_order_acquire)) {
                const int64_t now = monotonicMs();
                const uint32_t failures = gHapticResolveFailures.load(std::memory_order_relaxed);
                const int64_t retryInterval = failures < kHapticInitialResolveFastAttempts
                        ? kHapticInitialResolveRetryMs : kHapticFeatureResolveIntervalMs;
                int64_t last = gLastHapticFeatureResolveMs.load(std::memory_order_relaxed);
                if (now - last >= retryInterval
                        && gLastHapticFeatureResolveMs.compare_exchange_strong(
                                last, now, std::memory_order_relaxed)) {
                    if (!installHapticCaptureHookFromLauncherImport(
                            library, "watchdog-feature-probe")) {
                        gHapticResolveFailures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
'''
worker_new = '''            if (!gHapticCaptureHookInstalled.load(std::memory_order_acquire)
                    || !gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) {
                const int64_t now = monotonicMs();
                const uint32_t failures = gHapticResolveFailures.load(std::memory_order_relaxed);
                const int64_t retryInterval = failures < kHapticInitialResolveFastAttempts
                        ? kHapticInitialResolveRetryMs : kHapticFeatureResolveIntervalMs;
                int64_t last = gLastHapticFeatureResolveMs.load(std::memory_order_relaxed);
                if (now - last >= retryInterval
                        && gLastHapticFeatureResolveMs.compare_exchange_strong(
                                last, now, std::memory_order_relaxed)) {
                    const bool hookReady = gHapticCaptureHookInstalled.load(std::memory_order_acquire)
                            || installHapticCaptureHookFromLauncherImport(
                                    library, "watchdog-feature-probe");
                    const bool runtimeReady = gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)
                            || resolveHyperRtRuntimeBridge(library);
                    if (!hookReady || !runtimeReady) {
                        gHapticResolveFailures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        gHapticResolveFailures.store(0, std::memory_order_release);
                    }
                }
            }
'''
text = replace_once(text, worker_old, worker_new, "watchdog haptic resolver")

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
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release dedup armed deltaMs=%lld windowMs=%lld",
                static_cast<long long>(delta), static_cast<long long>(kReadyReleaseDedupMs));
    }
}

__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke_exit() {
    if (gReleaseDedupArmed && !gReleaseHapticSuppressed) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release dedup scope ended without stock release haptic");
    }
    gReleaseDedupArmed = false;
    gReleaseHapticSuppressed = false;
    gHapticGestureSegment.store(0, std::memory_order_release);
}
'''
text = replace_once(text, old_back, new_back, "back invoke dedup lifecycle")

old_log = '''    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved process-lifetime-arc retry-on-miss=1 confirm-dedup-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex",
            static_cast<long long>(kHapticConfirmSuppressMs));
'''
new_log = '''    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 constant=0",
            static_cast<long long>(kReadyReleaseDedupMs));
'''
text = replace_once(text, old_log, new_log, "native init haptic policy")
path.write_text(text)

asm = Path("native/src/swipe_hook_entry.S")
s = asm.read_text()
if ".hidden swipegate_haptic_on_back_invoke_exit\n" not in s:
    s = replace_once(
        s,
        ".hidden swipegate_haptic_on_back_invoke\n",
        ".hidden swipegate_haptic_on_back_invoke\n.hidden swipegate_haptic_on_back_invoke_exit\n",
        "asm hidden exit",
    )
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
    // Preserve the opaque return value while clearing the thread-local Release dedup scope.
    str x0, [sp, #0xd0]
    str q0, [sp, #0xe0]
    bl swipegate_haptic_on_back_invoke_exit
    ldr q0, [sp, #0xe0]
    ldr x0, [sp, #0xd0]
    ldr x30, [sp, #0x48]
    add sp, sp, #0x110
    ret
'''
s = replace_once(s, old_tail, new_tail, "asm release scope exit")
asm.write_text(s)

gradle = Path("app/build.gradle.kts")
g = gradle.read_text()
g = replace_once(g, "versionCode = 46", "versionCode = 47", "versionCode")
g = replace_once(g, 'versionName = "0.8.1-dev.4"', 'versionName = "0.8.1-dev.5"', "versionName")
gradle.write_text(g)
