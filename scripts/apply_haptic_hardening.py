from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = root / "native/src/main.cpp"
text = main.read_text()

replacements = [
    (
'''constexpr const char *kHapticLibraryNeedle = "libhyper_os_background_tasks_public";
constexpr const char *kHapticSymbol = "HapticFeedback_perform_ext_haptic_feedback";
constexpr const char *kStandardHapticSymbol = "HapticFeedback_perform_haptic_feedback";
constexpr int64_t kHapticFeatureResolveIntervalMs = 5000;
constexpr int32_t kReadyHapticConstant = 27;
constexpr int32_t kCommitHapticConstant = 0;
constexpr uintptr_t kPointerTagMask = 0x00ffffffffffffffull;''',
'''constexpr const char *kHapticProviderLibrary = "libhyper_os_background_tasks_public.so";
constexpr const char *kHapticSymbol = "HapticFeedback_perform_ext_haptic_feedback";
constexpr int64_t kHapticFeatureResolveIntervalMs = 5000;
constexpr int64_t kHapticArcMaxAgeMs = 5 * 60 * 1000;
constexpr int32_t kHapticConstant = 0;
constexpr uintptr_t kPointerAddressMask = 0x00ffffffffffffffull;'''
    ),
    (
'''std::atomic<void *> gOriginalHapticFeedback{nullptr};
std::atomic<void *> gStandardHapticFeedback{nullptr};
std::atomic<uintptr_t> gCapturedHapticArc{0};
std::atomic<bool> gHapticInstallInProgress{false};''',
'''std::atomic<void *> gOriginalHapticFeedback{nullptr};
std::atomic<uintptr_t> gCapturedHapticArc{0};
std::atomic<int64_t> gCapturedHapticArcAtMs{0};
std::atomic<bool> gHapticInstallInProgress{false};'''
    ),
    (
'''void hapticFeedbackCaptureHook(void *storage, int32_t constant) {
    if (storage != nullptr) {
        uintptr_t arc = 0;
        std::memcpy(&arc, storage, sizeof(arc));
        arc &= kPointerTagMask;
        if (arc >= 0x10000u) {
            gCapturedHapticArc.store(arc, std::memory_order_release);
            gHapticUnavailableLogged.store(false, std::memory_order_release);
        }
    }

    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    if (original != nullptr) original(storage, constant);
}

bool installHapticCaptureHookTarget(void *target, void *standard, const char *source) {''',
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
}

bool installHapticCaptureHookTarget(void *target, const char *source) {'''
    ),
    (
'''    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gStandardHapticFeedback.store(standard, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);
    gHapticInstallInProgress.store(false, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 capture hook ready source=%s target=%p standard=%p feature=%s watchdog=off no-dlopen=1",
            source == nullptr ? "unknown" : source, target, standard, kHapticSymbol);
    return true;
}

bool installHapticCaptureHookFromHandle(void *handle) {
    if (handle == nullptr || gHookFunction == nullptr || !isLauncherProcess()) return false;
    void *target = dlsym(handle, kHapticSymbol);
    void *standard = dlsym(handle, kStandardHapticSymbol);
    if (target == nullptr) return false;
    return installHapticCaptureHookTarget(target, standard, "provider-loader-fallback");
}
''',
'''    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);
    gHapticInstallInProgress.store(false, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 capture hook ready source=%s target=%p feature=%s workerOnly=1 no-dlsym=1 no-dlopen=1",
            source == nullptr ? "unknown" : source, target, kHapticSymbol);
    return true;
}
'''
    ),
    (
'''    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 loaded-elf import resolved source=%s symbol=%s reloc=%s got=%p target=%p launcher=%s",
            source == nullptr ? "unknown" : source, kHapticSymbol,
            resolution.relocationKind == nullptr ? "unknown" : resolution.relocationKind,
            reinterpret_cast<void *>(resolution.slot), resolution.target, library.path.c_str());
    return installHapticCaptureHookTarget(resolution.target, nullptr, "launcher-import-feature");
}

bool performNativeHaptic(const char *stage, bool light) {
    swipegate_control_sync_if_due();
    if (swipegate_control_haptic_enabled() != 1) return false;

    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    const uintptr_t arc = gCapturedHapticArc.load(std::memory_order_acquire);
    if (original == nullptr || arc < 0x10000u) {
        bool expected = false;
        if (gHapticUnavailableLogged.compare_exchange_strong(expected, true)) {
            logLine(ANDROID_LOG_WARN,
                    "HAPTIC_V2 skipped stage=%s reason=%s hook=%d arc=%p",
                    stage == nullptr ? "unknown" : stage,
                    original == nullptr ? "capture-hook-not-ready" : "runtime-arc-not-captured",
                    original == nullptr ? 0 : 1, reinterpret_cast<void *>(arc));
        }
        return false;
    }

    void *storage = reinterpret_cast<void *>(arc);
    if (light) {
        const auto standard = reinterpret_cast<HapticFeedbackFn>(
                gStandardHapticFeedback.load(std::memory_order_acquire));
        if (standard != nullptr) {
            standard(&storage, kReadyHapticConstant);
            logLine(ANDROID_LOG_INFO,
                    "HAPTIC_V2 feedback stage=%s kind=light constant=%d",
                    stage == nullptr ? "unknown" : stage, kReadyHapticConstant);
            return true;
        }
    }

    original(&storage, kCommitHapticConstant);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=%s constant=%d",
            stage == nullptr ? "unknown" : stage, light ? "light-fallback" : "commit",
            kCommitHapticConstant);
    return true;
}''',
'''    Dl_info providerInfo{};
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

    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    const uintptr_t rawArc = gCapturedHapticArc.load(std::memory_order_acquire);
    const uintptr_t addressBits = rawArc & kPointerAddressMask;
    const int64_t capturedAtMs = gCapturedHapticArcAtMs.load(std::memory_order_acquire);
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
    }

    // The provider ABI expects x0 to point to storage containing the Arc pointer.
    // Replay the exact tagged pointer observed from Xiaomi and only use the verified ext API.
    void *storage = reinterpret_cast<void *>(rawArc);
    original(&storage, kHapticConstant);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=ext constant=%d arcAgeMs=%lld",
            stage == nullptr ? "unknown" : stage, kHapticConstant,
            static_cast<long long>(now - capturedAtMs));
    return true;
}'''
    ),
    (
'''        performNativeHaptic(previousHapticSegment == 2 ? "return-first" : "first", true);''',
'''        performNativeHaptic(previousHapticSegment == 2 ? "return-first" : "first");'''
    ),
    (
'''    if (isLauncherProcess()
            && (std::strstr(name, "haptic") != nullptr
                    || std::strstr(name, "background_tasks") != nullptr)) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 loader candidate name=%s handle=%p",
                name, handle);
    }
    if (std::strstr(name, kHapticLibraryNeedle) != nullptr && isLauncherProcess()) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 loader matched name=%s handle=%p",
                name, handle);
        installHapticCaptureHookFromHandle(handle);
    }

    if (std::strstr(name, kTargetLibrary) != nullptr) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            ensureHook(library, "loader-callback");
            if (isLauncherProcess()) {
                installHapticCaptureHookFromLauncherImport(library, "launcher-loader-callback");
                ensureWorkerStarted();
            }
        }
    }''',
'''    // Keep linker callbacks lightweight. Haptic symbol resolution and hook installation are
    // worker-only to avoid linker-lock re-entry/deadlock hazards.
    if (isLauncherProcess()
            && (std::strstr(name, "haptic") != nullptr
                    || std::strstr(name, "background_tasks") != nullptr)) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 loader observed name=%s handle=%p workerOnly=1",
                name, handle);
        ensureWorkerStarted();
    }

    if (std::strstr(name, kTargetLibrary) != nullptr) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            ensureHook(library, "loader-callback");
            if (isLauncherProcess()) ensureWorkerStarted();
        }
    }'''
    ),
    (
'''            "HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-primary provider-loader-fallback first-segment-only no-module-second no-module-commit no-apk-open no-haptic-dlopen no-hook-mutex");''',
'''            "HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved arc-max-age-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex",
            static_cast<long long>(kHapticArcMaxAgeMs));'''
    ),
    (
'''        ensureHook(library, "native-init-backfill");
        if (launcherProcess) installHapticCaptureHookFromLauncherImport(library, "native-init-feature-probe");''',
'''        ensureHook(library, "native-init-backfill");'''
    ),
]

for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"replacement mismatch: expected 1, found {count}: {old[:100]!r}")
    text = text.replace(old, new, 1)

# Ensure haptic installation can only originate from the watchdog worker.
for forbidden in ["installHapticCaptureHookFromHandle", "kStandardHapticSymbol", "gStandardHapticFeedback", "kReadyHapticConstant"]:
    if forbidden in text:
        raise SystemExit(f"forbidden legacy haptic path remains: {forbidden}")

main.write_text(text)

gradle = root / "app/build.gradle.kts"
g = gradle.read_text()
g = g.replace('versionCode = 42\n        versionName = "0.8.1"',
              'versionCode = 43\n        versionName = "0.8.1-dev.1"')
if 'versionCode = 43' not in g or 'versionName = "0.8.1-dev.1"' not in g:
    raise SystemExit("version bump failed")
gradle.write_text(g)

print("Applied haptic hardening and bumped to 0.8.1-dev.1 (43)")
