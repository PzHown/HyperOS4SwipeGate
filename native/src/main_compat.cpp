// Compatibility entry point for HyperOS 4 Launcher generations with different
// Gesture on_swipe_process ABIs. Keep the validated legacy implementation in
// main.cpp untouched and add the 6174 GestureBackArrowView ABI alongside it.
#define native_init native_init_legacy_internal
#include "main.cpp"
#undef native_init

namespace {

// RELEASE-8.01.02.6174-260818-08281208-R
// GestureBackArrowView::on_swipe_process entry at libapp_launcher.so + 0x657080.
// The first 16 bytes also occur at another function in this build, so use 20
// exact bytes. The resulting executable-segment match count is exactly one.
constexpr uint8_t kGestureBackArrowPattern6174[] = {
        0xff, 0x83, 0x04, 0xd1, 0xeb, 0x2b, 0x0a, 0x6d,
        0xe9, 0x23, 0x0b, 0x6d, 0xfd, 0x7b, 0x0c, 0xa9,
        0xfc, 0x6b, 0x00, 0xf9,
};
constexpr uint8_t kGestureBackArrowMask6174[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
};
constexpr PatternSpec kGestureBackArrowSpec6174 = {
        "8.01.02.6174-v2",
        kGestureBackArrowPattern6174,
        kGestureBackArrowMask6174,
        sizeof(kGestureBackArrowPattern6174),
};
constexpr uintptr_t kReferenceGestureBackArrowOffset6174 = 0x657080;

static_assert(sizeof(kGestureBackArrowPattern6174) == sizeof(kGestureBackArrowMask6174));
static_assert(sizeof(kGestureBackArrowPattern6174) >= kHookProbeSize);

// AArch64 call site in 6174:
//   x0 = GestureBackArrowView*, w1 = ready/finish flag, w2 = side,
//   s0 = absolute horizontal swipe distance, s1/s2 = current point coordinates.
using GestureBackArrowProcess6174Fn =
        void (*)(void *, bool, uint32_t, float, float, float);

std::atomic<GestureBackArrowProcess6174Fn> gOriginalGestureBackArrow6174{nullptr};
std::atomic<bool> gCompatWorkerStarted{false};
std::atomic<bool> gUsing6174Abi{false};
std::atomic<uintptr_t> g6174Base{0};
std::atomic<uintptr_t> g6174Target{0};
std::array<uint8_t, kHookProbeSize> g6174OriginalHead{};
std::array<uint8_t, kHookProbeSize> g6174PatchHead{};
bool g6174OriginalHeadReady = false;
bool g6174PatchHeadReady = false;

float gatedHorizontalDistance(float horizontalDistancePx, bool readyFinish, uint32_t side) {
    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0
            ? kStockBoundaryDp
            : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const float stockBoundaryPx = densityDpi > 0
            ? dpToPx(kStockBoundaryDp, densityDpi)
            : 0.0f;
    const float stockGuardPx = stockBoundaryPx > 1.0f
            ? stockBoundaryPx - 1.0f
            : 0.0f;
    const float userGatePx = densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);

    const bool delayBeyondStock = effectiveDp > kStockBoundaryDp
            && densityDpi > 0
            && stockGuardPx > 0.0f
            && userGatePx > stockBoundaryPx;
    const bool userGateReached = !delayBeyondStock || absDx >= userGatePx;

    float effectiveDistancePx = horizontalDistancePx;
    bool clamped = false;
    if (delayBeyondStock && !userGateReached && absDx > stockGuardPx) {
        effectiveDistancePx = std::copysign(stockGuardPx, horizontalDistancePx);
        clamped = true;
        gClampedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        gPassthroughCount.fetch_add(1, std::memory_order_relaxed);
    }

    const int64_t now = monotonicMs();
    int64_t last = gLastSwipeLogMs.load(std::memory_order_relaxed);
    if (now - last >= 1000 && gLastSwipeLogMs.compare_exchange_strong(
            last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE_V2 rawDx=%.2f effectiveDx=%.2f configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f stockBoundaryPx=%.2f guardPx=%.2f gateReached=%d clamped=%d readyFinish=%d side=%u",
                horizontalDistancePx, effectiveDistancePx, configuredDp, effectiveDp,
                densityDpi, userGatePx, stockBoundaryPx, stockGuardPx,
                userGateReached ? 1 : 0, clamped ? 1 : 0,
                readyFinish ? 1 : 0, side);
    }
    return effectiveDistancePx;
}

void onGestureBackArrowProcess6174Hook(
        void *self,
        bool readyFinish,
        uint32_t side,
        float horizontalDistancePx,
        float pointX,
        float pointY) {
    ActiveHookCallGuard activeGuard;
    const float effectiveDistancePx = gatedHorizontalDistance(
            horizontalDistancePx, readyFinish, side);
    const GestureBackArrowProcess6174Fn original =
            gOriginalGestureBackArrow6174.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(self, readyFinish, side, effectiveDistancePx, pointX, pointY);
    }
}

PatternMatch resolve6174Target(const LibraryInfo &library, size_t *candidateCount) {
    PatternMatch match{};
    size_t count = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < kGestureBackArrowSpec6174.size) continue;

        const uintptr_t alignedStart = (range.start + 3U) & ~static_cast<uintptr_t>(3U);
        const uintptr_t last = range.start + range.size - kGestureBackArrowSpec6174.size;
        for (uintptr_t cursor = alignedStart; cursor <= last; cursor += 4U) {
            if (!patternMatchesAt(cursor, kGestureBackArrowSpec6174)) continue;
            ++count;
            if (count == 1) {
                match = {cursor, &kGestureBackArrowSpec6174};
            } else {
                match = {};
                if (candidateCount != nullptr) *candidateCount = count;
                return match;
            }
        }
    }
    if (candidateCount != nullptr) *candidateCount = count;
    return count == 1 ? match : PatternMatch{};
}

void reset6174Tracking(uintptr_t newBase) {
    const uintptr_t oldBase = g6174Base.exchange(0, std::memory_order_acq_rel);
    const uintptr_t oldTarget = g6174Target.exchange(0, std::memory_order_acq_rel);
    if (oldTarget != 0) {
        logLine(ANDROID_LOG_WARN,
                "HOOK_HEALTH_V2 launcher mapping changed oldBase=%p oldTarget=%p newBase=%p",
                reinterpret_cast<void *>(oldBase), reinterpret_cast<void *>(oldTarget),
                reinterpret_cast<void *>(newBase));
    }
    gOriginalGestureBackArrow6174.store(nullptr, std::memory_order_release);
    g6174OriginalHeadReady = false;
    g6174PatchHeadReady = false;
}

bool install6174Hook(const LibraryInfo &library, uintptr_t target, const char *source) {
    if (gHookFunction == nullptr || library.base == 0 || target == 0) return false;
    if (!patternMatchesAt(target, kGestureBackArrowSpec6174)) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_SCAN_V2 pattern changed source=%s target=%p; refusing",
                source, reinterpret_cast<void *>(target));
        return false;
    }

    std::array<uint8_t, kHookProbeSize> before{};
    if (!readProbeHead(target, before)) return false;

    void *backup = nullptr;
    const int rc = gHookFunction(
            reinterpret_cast<void *>(target),
            reinterpret_cast<void *>(onGestureBackArrowProcess6174Hook),
            &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE_V2 hook_func failed source=%s rc=%d backup=%p target=%p",
                source, rc, backup, reinterpret_cast<void *>(target));
        return false;
    }

    std::array<uint8_t, kHookProbeSize> patched{};
    if (!readProbeHead(target, patched) || probeEquals(before, patched)) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH_V2 hook returned success but entry is unchanged target=%p",
                reinterpret_cast<void *>(target));
        return false;
    }

    g6174OriginalHead = before;
    g6174OriginalHeadReady = true;
    g6174PatchHead = patched;
    g6174PatchHeadReady = true;
    gOriginalGestureBackArrow6174.store(
            reinterpret_cast<GestureBackArrowProcess6174Fn>(backup),
            std::memory_order_release);
    g6174Base.store(library.base, std::memory_order_release);
    g6174Target.store(target, std::memory_order_release);
    gUsing6174Abi.store(true, std::memory_order_release);
    gHookInstalled.store(true, std::memory_order_release);

    const uintptr_t resolvedOffset = target - library.base;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE_V2 hook installed source=%s pattern=%s base=%p target=%p resolvedOffset=0x%zx referenceOffset=0x%zx patchHead=%s",
            source, kGestureBackArrowSpec6174.name,
            reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(target),
            static_cast<size_t>(resolvedOffset),
            static_cast<size_t>(kReferenceGestureBackArrowOffset6174),
            probeHex(patched).c_str());
    return true;
}

bool repair6174Hook(const LibraryInfo &library, uintptr_t target, const char *source) {
    if (gUnhookFunction == nullptr || !g6174OriginalHeadReady) return false;
    const int64_t now = monotonicMs();
    const int64_t lastAttempt = gLastRepairAttemptMs.load(std::memory_order_relaxed);
    if (now - lastAttempt < kRepairCooldownMs) return false;
    gLastRepairAttemptMs.store(now, std::memory_order_relaxed);

    if (!waitForHookIdle()) return false;
    gOriginalGestureBackArrow6174.store(nullptr, std::memory_order_release);
    const int rc = gUnhookFunction(reinterpret_cast<void *>(target));

    std::array<uint8_t, kHookProbeSize> after{};
    if (!readProbeHead(target, after)) return false;
    if (!probeEquals(after, g6174OriginalHead)) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH_V2 repair refused rc=%d target=%p head=%s expected=%s",
                rc, reinterpret_cast<void *>(target), probeHex(after).c_str(),
                probeHex(g6174OriginalHead).c_str());
        return false;
    }

    g6174PatchHeadReady = false;
    if (!install6174Hook(library, target, "repair-v2")) return false;
    const uint64_t repairs = gRepairCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    logLine(ANDROID_LOG_INFO,
            "HOOK_HEALTH_V2 repaired successfully target=%p repairCount=%llu",
            reinterpret_cast<void *>(target), static_cast<unsigned long long>(repairs));
    return true;
}

bool ensure6174Hook(const LibraryInfo &library, const char *source) {
    const uintptr_t trackedBase = g6174Base.load(std::memory_order_acquire);
    const uintptr_t trackedTarget = g6174Target.load(std::memory_order_acquire);

    if (trackedBase == library.base && trackedTarget != 0) {
        std::array<uint8_t, kHookProbeSize> current{};
        if (!readProbeHead(trackedTarget, current)) return false;
        if (g6174PatchHeadReady && probeEquals(current, g6174PatchHead)) {
            gHookInstalled.store(true, std::memory_order_release);
            return true;
        }
        if (g6174OriginalHeadReady && probeEquals(current, g6174OriginalHead)) {
            gHookInstalled.store(false, std::memory_order_release);
            return repair6174Hook(library, trackedTarget, source);
        }
        gHookInstalled.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH_V2 foreign patch detected source=%s target=%p head=%s; refusing",
                source, reinterpret_cast<void *>(trackedTarget), probeHex(current).c_str());
        return false;
    }

    if (trackedBase != 0 || trackedTarget != 0) reset6174Tracking(library.base);

    size_t candidates = 0;
    const PatternMatch match = resolve6174Target(library, &candidates);
    if (candidates != 1 || match.address == 0) {
        return false;
    }

    logLine(ANDROID_LOG_INFO,
            "HOOK_SCAN_V2 resolved source=%s pattern=%s target=%p resolvedOffset=0x%zx candidates=%zu",
            source, kGestureBackArrowSpec6174.name,
            reinterpret_cast<void *>(match.address),
            static_cast<size_t>(match.address - library.base), candidates);
    return install6174Hook(library, match.address, source);
}

void compatibilityWatchdogWorker() {
    while (isTargetProcess()) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            if (gUsing6174Abi.load(std::memory_order_acquire)) {
                ensure6174Hook(library, "watchdog-v2");
            } else if (!ensureHook(library, "watchdog")) {
                ensure6174Hook(library, "watchdog-v2");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kHookHealthIntervalMs));
    }
}

void ensureCompatibilityWorkerStarted() {
    if (!isTargetProcess()) return;
    bool expected = false;
    if (!gCompatWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread(compatibilityWatchdogWorker).detach();
}

void onCompatibilityLibraryLoaded(const char *name, void *) {
    if (!isTargetProcess() || name == nullptr || std::strstr(name, kTargetLibrary) == nullptr) {
        return;
    }
    const LibraryInfo library = findLauncherLibrary();
    if (library.base == 0) return;

    if (gUsing6174Abi.load(std::memory_order_acquire)) {
        ensure6174Hook(library, "loader-callback-v2");
    } else if (!ensureHook(library, "loader-callback")) {
        ensure6174Hook(library, "loader-callback-v2");
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr || entries->hook_func == nullptr || !isTargetProcess()) {
        return nullptr;
    }
    gHookFunction = entries->hook_func;
    gUnhookFunction = entries->unhook_func;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE native_init accepted api=%u exe=%s process=%s hook_func=%p unhook_func=%p resolver=legacy+6174-v2",
            entries->version, readExecutable().c_str(), readProcessName().c_str(),
            reinterpret_cast<void *>(entries->hook_func),
            reinterpret_cast<void *>(entries->unhook_func));
    ensureCompatibilityWorkerStarted();
    return onCompatibilityLibraryLoaded;
}
