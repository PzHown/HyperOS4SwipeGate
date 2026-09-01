from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


def replace_span(path: str, start: str, end: str, replacement: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    try:
        start_at = text.index(start)
        end_at = text.index(end, start_at)
    except ValueError as exc:
        raise SystemExit(f"{label}: marker missing: {exc}")
    p.write_text(text[:start_at] + replacement + text[end_at:])


main_path = "native/src/main.cpp"

# Remove the obsolete GestureBackArrowView helper fingerprint. Runtime tracing on 6179 proved
# that stock hand-up feedback does not enter this helper; the real callsite is in
# GestureStubViewWindow::handle_back_gesture at RVA 0x654298, identical to 6174.
replace_span(
    main_path,
    "// Launcher 8.0.x GestureBackArrowView::check_and_perform_haptic_feedback entry.\n",
    "static_assert(sizeof(kOnSwipeProcessPatternV1)",
    "",
    "remove obsolete release helper pattern",
)
replace_once(
    main_path,
    "static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));\n"
    "static_assert(sizeof(kReleaseFeedbackPatternV1) == sizeof(kReleaseFeedbackMaskV1));\n",
    "static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));\n",
    "remove obsolete release pattern assert",
)

old_state = """std::atomic<uint64_t> gHapticTraceSequence{0};\nstd::atomic<int64_t> gLastReadyHapticAtMs{0};\nstd::atomic<bool> gReadyReleaseDedupEligible{false};\n// Launcher 8.0.x emits the stock hand-up vibration synchronously from\n// GestureBackArrowView::check_and_perform_haptic_feedback during on_swipe_stop. The\n// boundary hook keeps Xiaomi's state bookkeeping intact and only scopes the nested\n// constant=0 HyperRT call for one-shot suppression.\nstd::atomic<void *> gOriginalReleaseFeedbackHelper{nullptr};\nstd::atomic<uintptr_t> gReleaseFeedbackTarget{0};\nstd::atomic<bool> gReleaseFeedbackHookInstalled{false};\nthread_local bool gReleaseFeedbackScopeArmed = false;\nthread_local int64_t gReleaseFeedbackScopeReadyAtMs = 0;\n// Haptic segment state is independent from the Launcher hook health state.\n"""
new_state = """std::atomic<uint64_t> gHapticTraceSequence{0};\nstd::atomic<int64_t> gLastReadyHapticAtMs{0};\nstd::atomic<bool> gReadyReleaseDedupEligible{false};\n// Runtime tracing on Launcher 8.01.02.6179 proved the stock hand-up vibration reaches\n// HyperRT from GestureStubViewWindow::handle_back_gesture callsite RVA 0x654298. The same\n// callsite exists in 6174. Resolve the instruction semantically from its surrounding\n// get_global_runtime -> constant 0 -> ext haptic -> Runtime_dec_strong sequence; never\n// suppress unrelated constant=0 haptics.\nstd::atomic<uintptr_t> gStockBackReleaseHapticCallsite{0};\nstd::atomic<bool> gStockBackReleaseHapticCallsiteResolved{false};\n// Haptic segment state is independent from the Launcher hook health state.\n"""
replace_once(main_path, old_state, new_state, "replace release helper state")

old_trace_tail = """    const bool eligible = gReadyReleaseDedupEligible.load(std::memory_order_acquire);\n    const bool releaseHook = gReleaseFeedbackHookInstalled.load(std::memory_order_acquire);\n    const uintptr_t releaseTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);\n    const uintptr_t launcherBase = gHookedBase.load(std::memory_order_acquire);\n    const uintptr_t releaseTargetRva = launcherBase != 0 && releaseTarget >= launcherBase\n            ? releaseTarget - launcherBase : 0u;\n    const uint64_t sequence = gHapticTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;\n    const long tid = static_cast<long>(syscall(SYS_gettid));\n\n    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_TRACE stock-call seq=%llu tid=%ld constant=%d caller=%p callsite=%p module=%s moduleBase=%p lrRva=0x%zx callsiteRva=0x%zx launcherCaller=%d launcherBase=%p deltaReadyMs=%lld segment=%d eligible=%d releaseHook=%d releaseTarget=%p releaseTargetRva=0x%zx scopeArmed=%d\",\n            static_cast<unsigned long long>(sequence), tid, constant,\n            reinterpret_cast<void *>(callerReturnAddress), reinterpret_cast<void *>(callsite),\n            callerResolved ? traceModuleBasename(callerInfo.dli_fname) : \"<unresolved>\",\n            reinterpret_cast<void *>(moduleBase), static_cast<size_t>(lrRva),\n            static_cast<size_t>(callsiteRva), launcherCaller ? 1 : 0,\n            reinterpret_cast<void *>(launcherBase), static_cast<long long>(deltaReadyMs),\n            segment, eligible ? 1 : 0, releaseHook ? 1 : 0,\n            reinterpret_cast<void *>(releaseTarget), static_cast<size_t>(releaseTargetRva),\n            gReleaseFeedbackScopeArmed ? 1 : 0);\n"""
new_trace_tail = """    const bool eligible = gReadyReleaseDedupEligible.load(std::memory_order_acquire);\n    const uintptr_t releaseCallsite = gStockBackReleaseHapticCallsite.load(std::memory_order_acquire);\n    const uintptr_t launcherBase = gHookedBase.load(std::memory_order_acquire);\n    const uintptr_t releaseCallsiteRva = launcherBase != 0 && releaseCallsite >= launcherBase\n            ? releaseCallsite - launcherBase : 0u;\n    const bool releaseMatch = releaseCallsite != 0 && callsite == releaseCallsite;\n    const uint64_t sequence = gHapticTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;\n    const long tid = static_cast<long>(syscall(SYS_gettid));\n\n    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_TRACE stock-call seq=%llu tid=%ld constant=%d caller=%p callsite=%p module=%s moduleBase=%p lrRva=0x%zx callsiteRva=0x%zx launcherCaller=%d launcherBase=%p deltaReadyMs=%lld segment=%d eligible=%d releaseCallsite=%p releaseCallsiteRva=0x%zx releaseMatch=%d\",\n            static_cast<unsigned long long>(sequence), tid, constant,\n            reinterpret_cast<void *>(callerReturnAddress), reinterpret_cast<void *>(callsite),\n            callerResolved ? traceModuleBasename(callerInfo.dli_fname) : \"<unresolved>\",\n            reinterpret_cast<void *>(moduleBase), static_cast<size_t>(lrRva),\n            static_cast<size_t>(callsiteRva), launcherCaller ? 1 : 0,\n            reinterpret_cast<void *>(launcherBase), static_cast<long long>(deltaReadyMs),\n            segment, eligible ? 1 : 0, reinterpret_cast<void *>(releaseCallsite),\n            static_cast<size_t>(releaseCallsiteRva), releaseMatch ? 1 : 0);\n"""
replace_once(main_path, old_trace_tail, new_trace_tail, "update haptic trace fields")

old_suppress = """    // The stock 8.0.x hand-up effect is synchronous inside the resolved\n    // GestureBackArrowView release helper. Suppress only the nested constant=0 call while\n    // that exact helper is active. The helper itself still runs and sets feedback_done,\n    // preserving Xiaomi's release state machine.\n    if (constant == kHapticConstant && gReleaseFeedbackScopeArmed) {\n        gReleaseFeedbackScopeArmed = false;\n        const int64_t readyAt = gReleaseFeedbackScopeReadyAtMs;\n        gReleaseFeedbackScopeReadyAtMs = 0;\n        logLine(ANDROID_LOG_INFO,\n                \"HAPTIC_V2 release suppressed reason=ready-release-boundary constant=%d deltaMs=%lld windowMs=%lld source=GestureBackArrowView::check_and_perform_haptic_feedback\",\n                constant,\n                readyAt > 0 && now >= readyAt\n                        ? static_cast<long long>(now - readyAt) : -1LL,\n                static_cast<long long>(kReadyReleaseDedupMs));\n        return;\n    }\n\n    original(storage, constant);\n"""
new_suppress = """    // 6179 runtime evidence identifies the real hand-up haptic as the validated direct\n    // HyperRT callsite in GestureStubViewWindow::handle_back_gesture. Scope dedup to this\n    // single instruction; every other native haptic remains untouched. Returning here skips\n    // only HyperRT perform_ext_haptic_feedback. Launcher resumes at the next instruction and\n    // still performs Runtime_dec_strong and the rest of its stock release bookkeeping.\n    const uintptr_t releaseCallsite = gStockBackReleaseHapticCallsite.load(std::memory_order_acquire);\n    if (constant == kHapticConstant && releaseCallsite != 0 && callsite == releaseCallsite) {\n        const bool eligible = gReadyReleaseDedupEligible.exchange(false, std::memory_order_acq_rel);\n        const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);\n        const int64_t delta = readyAt > 0 && now >= readyAt ? now - readyAt : -1;\n        const bool suppress = eligible && delta >= 0 && delta < kReadyReleaseDedupMs;\n\n        // A committed Back ends the first-segment lifecycle. This was previously reset from\n        // the wrong GestureBackArrowView helper, which 6179 never entered.\n        gHapticGestureSegment.store(0, std::memory_order_release);\n\n        if (suppress) {\n            logLine(ANDROID_LOG_INFO,\n                    \"HAPTIC_V2 release suppressed reason=ready-release-callsite constant=%d deltaMs=%lld windowMs=%lld source=GestureStubViewWindow::handle_back_gesture callsite=%p\",\n                    constant, static_cast<long long>(delta),\n                    static_cast<long long>(kReadyReleaseDedupMs),\n                    reinterpret_cast<void *>(callsite));\n            return;\n        }\n        logLine(ANDROID_LOG_INFO,\n                \"HAPTIC_V2 release preserved reason=callsite-window constant=%d eligible=%d deltaMs=%lld windowMs=%lld callsite=%p\",\n                constant, eligible ? 1 : 0, static_cast<long long>(delta),\n                static_cast<long long>(kReadyReleaseDedupMs),\n                reinterpret_cast<void *>(callsite));\n    }\n\n    original(storage, constant);\n"""
replace_once(main_path, old_suppress, new_suppress, "replace release suppression")

# Replace the obsolete helper resolver/hook block with a semantic resolver for the real direct
# HyperRT callsite. The instruction fingerprint exactly matches 6174 and the 6179 runtime RVA.
resolver = r'''uintptr_t resolveStockBackReleaseHapticCallsite(const LibraryInfo &library, const char **featureName) {
    if (featureName != nullptr) *featureName = nullptr;
    if (library.base == 0) return 0;

    const ImportedFunctionResolution haptic = resolveImportedFunction(library, kHapticSymbol);
    const ImportedFunctionResolution decStrong = resolveImportedFunction(library, "Runtime_dec_strong");
    if (haptic.matches != 1 || haptic.slot == 0
            || decStrong.matches != 1 || decStrong.slot == 0) {
        if (featureName != nullptr) *featureName = "imports-unresolved";
        return 0;
    }

    uintptr_t found = 0;
    size_t matches = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < 40) continue;
        const uintptr_t first = (range.start + 20u + 3u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - 12u;
        for (uintptr_t pc = first; pc <= last; pc += 4u) {
            uint32_t hapticCallInsn = 0;
            std::memcpy(&hapticCallInsn, reinterpret_cast<const void *>(pc), sizeof(hapticCallInsn));
            uintptr_t hapticPlt = 0;
            if (!decodeBlTarget(pc, hapticCallInsn, &hapticPlt)
                    || !pltReferencesSlot(library, hapticPlt, haptic.slot)) {
                continue;
            }

            uint32_t getRuntimeCallInsn = 0;
            uint32_t saveRuntime = 0;
            uint32_t storageArg = 0;
            uint32_t movW1Zero = 0;
            uint32_t restoreRuntime = 0;
            uint32_t decStrongCallInsn = 0;
            std::memcpy(&getRuntimeCallInsn,
                        reinterpret_cast<const void *>(pc - 20u), sizeof(getRuntimeCallInsn));
            std::memcpy(&saveRuntime,
                        reinterpret_cast<const void *>(pc - 16u), sizeof(saveRuntime));
            std::memcpy(&storageArg,
                        reinterpret_cast<const void *>(pc - 8u), sizeof(storageArg));
            std::memcpy(&movW1Zero,
                        reinterpret_cast<const void *>(pc - 4u), sizeof(movW1Zero));
            std::memcpy(&restoreRuntime,
                        reinterpret_cast<const void *>(pc + 4u), sizeof(restoreRuntime));
            std::memcpy(&decStrongCallInsn,
                        reinterpret_cast<const void *>(pc + 8u), sizeof(decStrongCallInsn));

            // 6174 and runtime-confirmed 6179 sequence around RVA 0x654298:
            //   bl get_global_runtime
            //   mov x22,x0
            //   ...
            //   sub x0,x29,#0xe8
            //   mov w1,wzr
            //   bl HapticFeedback_perform_ext_haptic_feedback
            //   mov x0,x22
            //   bl Runtime_dec_strong
            if (saveRuntime != 0xaa0003f6u) continue;       // mov x22,x0
            if (storageArg != 0xd103a3a0u) continue;        // sub x0,x29,#0xe8
            if (movW1Zero != 0x2a1f03e1u) continue;        // mov w1,wzr
            if (restoreRuntime != 0xaa1603e0u) continue;   // mov x0,x22

            uintptr_t getRuntimeTarget = 0;
            uintptr_t decStrongPlt = 0;
            if (!decodeBlTarget(pc - 20u, getRuntimeCallInsn, &getRuntimeTarget)
                    || !libraryContainsRange(library, getRuntimeTarget, 4)
                    || !decodeBlTarget(pc + 8u, decStrongCallInsn, &decStrongPlt)
                    || !pltReferencesSlot(library, decStrongPlt, decStrong.slot)) {
                continue;
            }

            found = pc;
            if (++matches > 1) {
                if (featureName != nullptr) *featureName = "ambiguous";
                return 0;
            }
        }
    }

    if (matches == 1 && featureName != nullptr) *featureName = "stock-back-release-haptic-v1";
    return matches == 1 ? found : 0;
}

bool resolveAndPublishStockBackReleaseHapticCallsite(const LibraryInfo &library, const char *source) {
    const char *featureName = nullptr;
    const uintptr_t callsite = resolveStockBackReleaseHapticCallsite(library, &featureName);
    if (callsite == 0) {
        gStockBackReleaseHapticCallsiteResolved.store(false, std::memory_order_release);
        gStockBackReleaseHapticCallsite.store(0, std::memory_order_release);
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 release callsite unresolved source=%s feature=%s activeProfile=%s; stock release remains untouched",
                source == nullptr ? "unknown" : source,
                featureName == nullptr ? "none" : featureName, gActivePatternName);
        return false;
    }

    gStockBackReleaseHapticCallsite.store(callsite, std::memory_order_release);
    gStockBackReleaseHapticCallsiteResolved.store(true, std::memory_order_release);
    const uintptr_t rva = callsite >= library.base ? callsite - library.base : 0u;
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 release callsite ready source=%s feature=%s callsite=%p callsiteRva=0x%zx reference6174_6179=0x654298 exactReference=%d",
            source == nullptr ? "unknown" : source,
            featureName == nullptr ? "unknown" : featureName,
            reinterpret_cast<void *>(callsite), static_cast<size_t>(rva),
            rva == 0x654298u ? 1 : 0);
    return true;
}

'''
replace_span(
    main_path,
    "using ReleaseFeedbackHelperFn = void (*)(void *);\n",
    "float gateHorizontalDistance(",
    resolver,
    "replace obsolete release helper block",
)

old_install = """    // 8.0.x stock hand-up candidate from 6174. Keep it installed while the provider-level\n    // caller trace determines the actual 6179 release callsite.\n    const bool releaseBoundaryReady = installReleaseFeedbackHapticHook(library);\n    const uintptr_t releaseBoundaryTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);\n    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_TRACE release-boundary-status installed=%d target=%p targetRva=0x%zx launcherBase=%p\",\n            releaseBoundaryReady ? 1 : 0, reinterpret_cast<void *>(releaseBoundaryTarget),\n            library.base != 0 && releaseBoundaryTarget >= library.base\n                    ? static_cast<size_t>(releaseBoundaryTarget - library.base) : 0u,\n            reinterpret_cast<void *>(library.base));\n    return true;\n"""
new_install = """    // Runtime tracing on 6179 identified the real stock hand-up HyperRT callsite. Resolve\n    // its structural fingerprint now; no secondary Launcher function hook is required.\n    resolveAndPublishStockBackReleaseHapticCallsite(library, \"primary-hook-install\");\n    return true;\n"""
replace_once(main_path, old_install, new_install, "replace release helper install")

old_reset = """    gActivePatternName = \"<none>\";\n    gActiveResolverDetail = \"<none>\";\n    gReleaseFeedbackHookInstalled.store(false, std::memory_order_release);\n    gReleaseFeedbackTarget.store(0, std::memory_order_release);\n    gOriginalReleaseFeedbackHelper.store(nullptr, std::memory_order_release);\n"""
new_reset = """    gActivePatternName = \"<none>\";\n    gActiveResolverDetail = \"<none>\";\n    gStockBackReleaseHapticCallsiteResolved.store(false, std::memory_order_release);\n    gStockBackReleaseHapticCallsite.store(0, std::memory_order_release);\n"""
replace_once(main_path, old_reset, new_reset, "reset release callsite on remap")

old_health = """                const uintptr_t releaseTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);\n                logLine(ANDROID_LOG_INFO,\n                        \"HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu hapticCapture=%d releaseHook=%d releaseTarget=%p releaseTargetRva=0x%zx\",\n                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),\n                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),\n                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)),\n                        gHapticCaptureHookInstalled.load(std::memory_order_acquire) ? 1 : 0,\n                        gReleaseFeedbackHookInstalled.load(std::memory_order_acquire) ? 1 : 0,\n                        reinterpret_cast<void *>(releaseTarget),\n                        library.base != 0 && releaseTarget >= library.base\n                                ? static_cast<size_t>(releaseTarget - library.base) : 0u);\n"""
new_health = """                const uintptr_t releaseCallsite = gStockBackReleaseHapticCallsite.load(\n                        std::memory_order_acquire);\n                logLine(ANDROID_LOG_INFO,\n                        \"HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu hapticCapture=%d releaseCallsiteReady=%d releaseCallsite=%p releaseCallsiteRva=0x%zx\",\n                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),\n                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),\n                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)),\n                        gHapticCaptureHookInstalled.load(std::memory_order_acquire) ? 1 : 0,\n                        gStockBackReleaseHapticCallsiteResolved.load(std::memory_order_acquire) ? 1 : 0,\n                        reinterpret_cast<void *>(releaseCallsite),\n                        library.base != 0 && releaseCallsite >= library.base\n                                ? static_cast<size_t>(releaseCallsite - library.base) : 0u);\n"""
replace_once(main_path, old_health, new_health, "update health release callsite state")

old_watchdog = """            ensureHook(library, \"watchdog\");\n            if (!gReleaseFeedbackHookInstalled.load(std::memory_order_acquire)) {\n                installReleaseFeedbackHapticHook(library);\n            }\n            swipegate_back_break_maintain();\n"""
new_watchdog = """            ensureHook(library, \"watchdog\");\n            if (!gStockBackReleaseHapticCallsiteResolved.load(std::memory_order_acquire)) {\n                resolveAndPublishStockBackReleaseHapticCallsite(library, \"watchdog\");\n            }\n            swipegate_back_break_maintain();\n"""
replace_once(main_path, old_watchdog, new_watchdog, "watchdog release callsite resolver")

replace_once(
    main_path,
    "            \"HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 release-boundary=GestureBackArrowView constant=0\",\n",
    "            \"HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 release-source=GestureStubViewWindow::handle_back_gesture callsite-scoped=1 constant=0\",\n",
    "update native init policy log",
)

replace_once(
    "app/build.gradle.kts",
    "        // dev.17: capture real Launcher/HyperRT stock haptic caller LR/RVA on 8.x.\n"
    "        versionCode = 59\n"
    "        versionName = \"0.8.1-dev.17\"\n",
    "        // dev.18: dedup the runtime-proven stock Back release HyperRT callsite.\n"
    "        versionCode = 60\n"
    "        versionName = \"0.8.1-dev.18\"\n",
    "version bump",
)

# One-shot staging cleanup. The final commit contains only product source changes.
Path("scripts/apply_release_callsite_fix.py").unlink()
Path(".github/workflows/run-release-callsite-fix.yml").unlink()

main_text = Path(main_path).read_text()
for forbidden in [
    "gReleaseFeedbackScopeArmed",
    "gReleaseFeedbackScopeReadyAtMs",
    "gReleaseFeedbackHookInstalled",
    "gReleaseFeedbackTarget",
    "gOriginalReleaseFeedbackHelper",
    "installReleaseFeedbackHapticHook",
    "releaseFeedbackHelperHook",
    "release-ready-state-back-v1",
]:
    if forbidden in main_text:
        raise SystemExit(f"obsolete release-helper symbol remains: {forbidden}")
for required in [
    "stock-back-release-haptic-v1",
    "gStockBackReleaseHapticCallsite",
    "reason=ready-release-callsite",
    "GestureStubViewWindow::handle_back_gesture",
    "storageArg != 0xd103a3a0u",
    "rva == 0x654298u",
    "kReadyReleaseDedupMs = 750",
]:
    if required not in main_text:
        raise SystemExit(f"required release-callsite invariant missing: {required}")
