from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


# 1) Native trace: capture the real caller of every stock HyperRT ext-haptic invocation.
replace_once(
    "native/src/main.cpp",
    "#include <sys/system_properties.h>\n",
    "#include <sys/system_properties.h>\n#include <sys/syscall.h>\n",
    "add gettid include",
)

replace_once(
    "native/src/main.cpp",
    "std::atomic<bool> gHapticRuntimeBridgeResolved{false};\n",
    "std::atomic<bool> gHapticRuntimeBridgeResolved{false};\n"
    "std::atomic<uint64_t> gHapticTraceSequence{0};\n",
    "add trace sequence",
)

old_hook_start = """using HapticFeedbackFn = void (*)(void *, int32_t);\n\nvoid hapticFeedbackCaptureHook(void *storage, int32_t constant) {\n    const int64_t now = monotonicMs();\n"""
new_hook_start = r'''using HapticFeedbackFn = void (*)(void *, int32_t);

const char *traceModuleBasename(const char *path) {
    if (path == nullptr || *path == '\0') return "<unresolved>";
    const char *slash = std::strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

void traceStockHapticCall(int32_t constant, int64_t now, uintptr_t callerReturnAddress) {
    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;
    Dl_info callerInfo{};
    const bool callerResolved = callsite != 0
            && dladdr(reinterpret_cast<void *>(callsite), &callerInfo) != 0
            && callerInfo.dli_fbase != nullptr;
    const uintptr_t moduleBase = callerResolved
            ? reinterpret_cast<uintptr_t>(callerInfo.dli_fbase) : 0u;
    const uintptr_t lrRva = moduleBase != 0 && callerReturnAddress >= moduleBase
            ? callerReturnAddress - moduleBase : 0u;
    const uintptr_t callsiteRva = moduleBase != 0 && callsite >= moduleBase
            ? callsite - moduleBase : 0u;
    const bool launcherCaller = callerResolved && callerInfo.dli_fname != nullptr
            && std::strstr(callerInfo.dli_fname, kTargetLibrary) != nullptr;

    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
    const int64_t deltaReadyMs = readyAt > 0 && now >= readyAt ? now - readyAt : -1;
    const int segment = gHapticGestureSegment.load(std::memory_order_acquire);
    const bool eligible = gReadyReleaseDedupEligible.load(std::memory_order_acquire);
    const bool releaseHook = gReleaseFeedbackHookInstalled.load(std::memory_order_acquire);
    const uintptr_t releaseTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);
    const uintptr_t launcherBase = gHookedBase.load(std::memory_order_acquire);
    const uintptr_t releaseTargetRva = launcherBase != 0 && releaseTarget >= launcherBase
            ? releaseTarget - launcherBase : 0u;
    const uint64_t sequence = gHapticTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const long tid = static_cast<long>(syscall(SYS_gettid));

    logLine(ANDROID_LOG_INFO,
            "HAPTIC_TRACE stock-call seq=%llu tid=%ld constant=%d caller=%p callsite=%p module=%s moduleBase=%p lrRva=0x%zx callsiteRva=0x%zx launcherCaller=%d launcherBase=%p deltaReadyMs=%lld segment=%d eligible=%d releaseHook=%d releaseTarget=%p releaseTargetRva=0x%zx scopeArmed=%d",
            static_cast<unsigned long long>(sequence), tid, constant,
            reinterpret_cast<void *>(callerReturnAddress), reinterpret_cast<void *>(callsite),
            callerResolved ? traceModuleBasename(callerInfo.dli_fname) : "<unresolved>",
            reinterpret_cast<void *>(moduleBase), static_cast<size_t>(lrRva),
            static_cast<size_t>(callsiteRva), launcherCaller ? 1 : 0,
            reinterpret_cast<void *>(launcherBase), static_cast<long long>(deltaReadyMs),
            segment, eligible ? 1 : 0, releaseHook ? 1 : 0,
            reinterpret_cast<void *>(releaseTarget), static_cast<size_t>(releaseTargetRva),
            gReleaseFeedbackScopeArmed ? 1 : 0);
}

__attribute__((noinline)) void hapticFeedbackCaptureHook(void *storage, int32_t constant) {
    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points
    // at the instruction immediately after the real callsite in libapp_launcher.so. Capture
    // it before doing any other work. Synthetic Ready feedback uses the original trampoline
    // and therefore never enters this function.
    void *rawReturnAddress = __builtin_return_address(0);
    const uintptr_t callerReturnAddress = reinterpret_cast<uintptr_t>(
            __builtin_extract_return_addr(rawReturnAddress)) & kPointerAddressMask;
    const int64_t now = monotonicMs();
    traceStockHapticCall(constant, now, callerReturnAddress);
'''
replace_once(
    "native/src/main.cpp",
    old_hook_start,
    new_hook_start,
    "instrument HyperRT capture hook",
)

old_boundary = """    const int64_t now = monotonicMs();\n    const bool eligible = gReadyReleaseDedupEligible.exchange(false, std::memory_order_acq_rel);\n    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);\n    const int64_t delta = readyAt > 0 && now >= readyAt ? now - readyAt : -1;\n    const bool suppress = eligible && delta >= 0 && delta < kReadyReleaseDedupMs\n            && gHapticCaptureHookInstalled.load(std::memory_order_acquire);\n\n    gReleaseFeedbackScopeReadyAtMs = suppress ? readyAt : 0;\n"""
new_boundary = """    const int64_t now = monotonicMs();\n    const bool eligible = gReadyReleaseDedupEligible.exchange(false, std::memory_order_acq_rel);\n    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);\n    const int64_t delta = readyAt > 0 && now >= readyAt ? now - readyAt : -1;\n    const bool suppress = eligible && delta >= 0 && delta < kReadyReleaseDedupMs\n            && gHapticCaptureHookInstalled.load(std::memory_order_acquire);\n    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_TRACE release-boundary-enter tid=%ld eligible=%d deltaReadyMs=%lld segment=%d suppress=%d target=%p\",\n            static_cast<long>(syscall(SYS_gettid)), eligible ? 1 : 0,\n            static_cast<long long>(delta),\n            gHapticGestureSegment.load(std::memory_order_acquire), suppress ? 1 : 0,\n            reinterpret_cast<void *>(gReleaseFeedbackTarget.load(std::memory_order_acquire)));\n\n    gReleaseFeedbackScopeReadyAtMs = suppress ? readyAt : 0;\n"""
replace_once(
    "native/src/main.cpp",
    old_boundary,
    new_boundary,
    "trace release helper entry",
)

old_install = """    // 8.0.x stock hand-up lives in GestureBackArrowView::on_swipe_stop ->\n    // check_and_perform_haptic_feedback, before on_back_invoke. Hook that exact helper.\n    installReleaseFeedbackHapticHook(library);\n    return true;\n"""
new_install = """    // 8.0.x stock hand-up candidate from 6174. Keep it installed while the provider-level\n    // caller trace determines the actual 6179 release callsite.\n    const bool releaseBoundaryReady = installReleaseFeedbackHapticHook(library);\n    const uintptr_t releaseBoundaryTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);\n    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_TRACE release-boundary-status installed=%d target=%p targetRva=0x%zx launcherBase=%p\",\n            releaseBoundaryReady ? 1 : 0, reinterpret_cast<void *>(releaseBoundaryTarget),\n            library.base != 0 && releaseBoundaryTarget >= library.base\n                    ? static_cast<size_t>(releaseBoundaryTarget - library.base) : 0u,\n            reinterpret_cast<void *>(library.base));\n    return true;\n"""
replace_once(
    "native/src/main.cpp",
    old_install,
    new_install,
    "log release boundary install status",
)

old_health = """                logLine(ANDROID_LOG_INFO,\n                        \"HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu\",\n                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),\n                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),\n                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));\n"""
new_health = """                const uintptr_t releaseTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);\n                logLine(ANDROID_LOG_INFO,\n                        \"HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu hapticCapture=%d releaseHook=%d releaseTarget=%p releaseTargetRva=0x%zx\",\n                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),\n                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),\n                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)),\n                        gHapticCaptureHookInstalled.load(std::memory_order_acquire) ? 1 : 0,\n                        gReleaseFeedbackHookInstalled.load(std::memory_order_acquire) ? 1 : 0,\n                        reinterpret_cast<void *>(releaseTarget),\n                        library.base != 0 && releaseTarget >= library.base\n                                ? static_cast<size_t>(releaseTarget - library.base) : 0u);\n"""
replace_once(
    "native/src/main.cpp",
    old_health,
    new_health,
    "surface haptic hook state in diagnostics",
)

replace_once(
    "app/build.gradle.kts",
    "        // dev.16: verified Launcher 8.x release-boundary haptic dedup at GestureBackArrowView.\n"
    "        versionCode = 58\n"
    "        versionName = \"0.8.1-dev.16\"\n",
    "        // dev.17: capture real Launcher/HyperRT stock haptic caller LR/RVA on 8.x.\n"
    "        versionCode = 59\n"
    "        versionName = \"0.8.1-dev.17\"\n",
    "version bump",
)

# Verify that the trace can identify an actual stock caller and that synthetic Ready still
# bypasses the capture hook via gOriginalHapticFeedback.
main_text = Path("native/src/main.cpp").read_text()
for required in [
    "HAPTIC_TRACE stock-call",
    "callsiteRva=0x%zx",
    "deltaReadyMs=%lld",
    "launcherCaller=%d",
    "release-boundary-enter",
    "hapticCapture=%d releaseHook=%d",
    "__builtin_return_address(0)",
    "gOriginalHapticFeedback.load",
    "kReadyReleaseDedupMs = 750",
]:
    if required not in main_text:
        raise SystemExit(f"required trace invariant missing: {required}")

# The staging machinery is intentionally one-shot.
Path("scripts/apply_haptic_caller_trace.py").unlink()
Path(".github/workflows/run-haptic-caller-trace.yml").unlink()
