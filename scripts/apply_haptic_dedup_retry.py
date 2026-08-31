from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = root / "native/src/main.cpp"
text = main.read_text()

replacements = [
    (
'''constexpr int64_t kHapticFeatureResolveIntervalMs = 5000;
constexpr int64_t kHapticArcMaxAgeMs = 5 * 60 * 1000;
constexpr int32_t kHapticConstant = 0;''',
'''constexpr int64_t kHapticFeatureResolveIntervalMs = 5000;
constexpr int64_t kHapticInitialResolveRetryMs = 500;
constexpr uint32_t kHapticInitialResolveFastAttempts = 10;
constexpr int64_t kHapticConfirmSuppressMs = 120;
constexpr int32_t kHapticConstant = 0;'''),
    (
'''std::atomic<bool> gHapticUnavailableLogged{false};
std::atomic<int64_t> gLastHapticFeatureResolveMs{0};''',
'''std::atomic<bool> gHapticUnavailableLogged{false};
std::atomic<int64_t> gLastHapticFeatureResolveMs{0};
std::atomic<uint32_t> gHapticResolveFailures{0};
std::atomic<int64_t> gNativeHapticSuppressUntilMs{0};
std::atomic<int64_t> gLastInjectedHapticAtMs{0};'''),
    (
'''void hapticFeedbackCaptureHook(void *storage, int32_t constant) {
    if (storage != nullptr) {
        uintptr_t rawArc = 0;
        std::memcpy(&rawArc, storage, sizeof(rawArc));
        const uintptr_t addressBits = rawArc & kPointerAddressMask;
        if (addressBits >= 0x10000u) {
            // Preserve the original tagged pointer exactly as Xiaomi supplied it.  The
            // untagged value is used only for sanity checking, never for replay.
            gCapturedHapticArc.store(rawArc, std::memory_order_release);
            gCapturedHapticArcAtMs.store(monotonicMs(), std::memory_order_release);
            gHapticUnavailableLogged.store(false, std::memory_order_release);
        }
    }

    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    if (original != nullptr) original(storage, constant);
}''',
'''void hapticFeedbackCaptureHook(void *storage, int32_t constant) {
    const int64_t now = monotonicMs();
    if (storage != nullptr) {
        uintptr_t rawArc = 0;
        std::memcpy(&rawArc, storage, sizeof(rawArc));
        const uintptr_t addressBits = rawArc & kPointerAddressMask;
        if (addressBits >= 0x10000u) {
            // Preserve the original tagged pointer exactly as Xiaomi supplied it.  The
            // untagged value is used only for sanity checking, never for replay.
            gCapturedHapticArc.store(rawArc, std::memory_order_release);
            gCapturedHapticArcAtMs.store(now, std::memory_order_release);
            gHapticUnavailableLogged.store(false, std::memory_order_release);
        }
    }

    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    if (original == nullptr) return;

    // The module's injected threshold haptic calls the trampoline directly, so only Xiaomi's
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
}'''),
    (
'''    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);''',
'''    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gHapticResolveFailures.store(0, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);'''),
    (
'''    const int64_t capturedAtMs = gCapturedHapticArcAtMs.load(std::memory_order_acquire);
    const int64_t now = monotonicMs();
    const bool stale = capturedAtMs <= 0 || now < capturedAtMs
            || now - capturedAtMs > kHapticArcMaxAgeMs;
    if (original == nullptr || addressBits < 0x10000u || stale) {
        bool expected = false;
        if (gHapticUnavailableLogged.compare_exchange_strong(expected, true)) {
            const char *reason = original == nullptr ? "capture-hook-not-ready"
                    : addressBits < 0x10000u ? "runtime-arc-not-captured"
                    : "runtime-arc-stale";
            logLine(ANDROID_LOG_WARN,
                    "HAPTIC_V2 skipped stage=%s reason=%s hook=%d arc=%p ageMs=%lld maxAgeMs=%lld",
                    stage == nullptr ? "unknown" : stage, reason,
                    original == nullptr ? 0 : 1, reinterpret_cast<void *>(rawArc),
                    capturedAtMs <= 0 || now < capturedAtMs ? -1LL
                            : static_cast<long long>(now - capturedAtMs),
                    static_cast<long long>(kHapticArcMaxAgeMs));
        }
        return false;
    }''',
'''    const int64_t capturedAtMs = gCapturedHapticArcAtMs.load(std::memory_order_acquire);
    const int64_t now = monotonicMs();
    if (original == nullptr || addressBits < 0x10000u) {
        bool expected = false;
        if (gHapticUnavailableLogged.compare_exchange_strong(expected, true)) {
            const char *reason = original == nullptr ? "capture-hook-not-ready"
                    : "runtime-arc-not-captured";
            logLine(ANDROID_LOG_WARN,
                    "HAPTIC_V2 skipped stage=%s reason=%s hook=%d arc=%p ageMs=%lld retry=1",
                    stage == nullptr ? "unknown" : stage, reason,
                    original == nullptr ? 0 : 1, reinterpret_cast<void *>(rawArc),
                    capturedAtMs <= 0 || now < capturedAtMs ? -1LL
                            : static_cast<long long>(now - capturedAtMs));
        }
        return false;
    }'''),
    (
'''    original(&storage, kHapticConstant);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=ext constant=%d arcAgeMs=%lld",
            stage == nullptr ? "unknown" : stage, kHapticConstant,
            static_cast<long long>(now - capturedAtMs));
    return true;''',
'''    original(&storage, kHapticConstant);
    gLastInjectedHapticAtMs.store(now, std::memory_order_release);
    gNativeHapticSuppressUntilMs.store(now + kHapticConfirmSuppressMs, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=ext constant=%d arcAgeMs=%lld confirmSuppressMs=%lld",
            stage == nullptr ? "unknown" : stage, kHapticConstant,
            capturedAtMs <= 0 || now < capturedAtMs ? -1LL
                    : static_cast<long long>(now - capturedAtMs),
            static_cast<long long>(kHapticConfirmSuppressMs));
    return true;'''),
    (
'''    const int previousHapticSegment = gHapticGestureSegment.exchange(
            hapticSegment, std::memory_order_acq_rel);
    if (hapticSegment == 1 && previousHapticSegment != 1) {
        performNativeHaptic(previousHapticSegment == 2 ? "return-first" : "first");
    }''',
'''    if (hapticSegment != 1) {
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
    }'''),
    (
'''                const int64_t now = monotonicMs();
                int64_t last = gLastHapticFeatureResolveMs.load(std::memory_order_relaxed);
                if (now - last >= kHapticFeatureResolveIntervalMs
                        && gLastHapticFeatureResolveMs.compare_exchange_strong(
                                last, now, std::memory_order_relaxed)) {
                    installHapticCaptureHookFromLauncherImport(library, "watchdog-feature-probe");
                }''',
'''                const int64_t now = monotonicMs();
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
                }'''),
    (
'''            "HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-worker-only ext-only constant=0 tagged-arc arc-max-age=300000ms first-segment-only no-module-second no-module-commit no-apk-open no-haptic-dlopen no-hook-mutex");''',
'''            "HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-worker-only ext-only constant=0 tagged-arc process-lifetime-arc retry-on-miss=1 confirm-dedup=120ms first-segment-only no-module-second no-module-commit no-apk-open no-haptic-dlopen no-hook-mutex");'''),
]

for old, new in replacements:
    if old not in text:
        raise SystemExit("missing expected block:\n" + old[:300])
    text = text.replace(old, new, 1)

main.write_text(text)

build = root / "app/build.gradle.kts"
b = build.read_text()
b = b.replace('versionCode = 43', 'versionCode = 44', 1)
b = b.replace('versionName = "0.8.1-dev.1"', 'versionName = "0.8.1-dev.2"', 1)
if 'versionCode = 44' not in b or 'versionName = "0.8.1-dev.2"' not in b:
    raise SystemExit('version replacement failed')
build.write_text(b)
