from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


main = "native/src/main.cpp"

replace_once(
    main,
    "constexpr int64_t kPatternRescanIntervalMs = 5000;\nconstexpr size_t kHookProbeSize = 16;\n",
    "constexpr int64_t kPatternRescanIntervalMs = 5000;\n"
    "constexpr size_t kBackProgressCallerScanSize = 0x900;\n"
    "constexpr size_t kBackProgressBodyProbeSize = 0x90;\n"
    "constexpr uint32_t kFmovS1Twenty = 0x1e269001u;\n"
    "constexpr uint32_t kFmovS8S0 = 0x1e204008u;\n"
    "constexpr uint32_t kFcmpS8Zero = 0x1e202108u;\n"
    "constexpr uint32_t kMovW8Float110 = 0x52a85b88u;\n"
    "constexpr size_t kHookProbeSize = 16;\n",
    "progress constants",
)

replace_once(
    main,
    "std::atomic<uintptr_t> gStockBackReleaseHapticCallsite{0};\n"
    "std::atomic<bool> gStockBackReleaseHapticCallsiteResolved{false};\n",
    "std::atomic<uintptr_t> gStockBackReleaseHapticCallsite{0};\n"
    "std::atomic<bool> gStockBackReleaseHapticCallsiteResolved{false};\n"
    "// Launcher 8.x uses BackGestureUtils::convert_offset as the shared progress coordinate\n"
    "// for on_swipe_process, on_vsync and release. Runtime hook scales its distance input so\n"
    "// Xiaomi's native 0.8 READY point moves from 88dp to the configured threshold without\n"
    "// freezing the animation or changing unrelated raw gesture/velocity calculations.\n"
    "std::atomic<void *> gOriginalBackProgressConvertOffset{nullptr};\n"
    "std::atomic<uintptr_t> gBackProgressConvertOffsetTarget{0};\n"
    "std::atomic<bool> gBackProgressHookInstalled{false};\n"
    "std::atomic<bool> gBackProgressResolveFailureLogged{false};\n",
    "progress state",
)

old_decode = """bool decodeBlTarget(uintptr_t pc, uint32_t instruction, uintptr_t *target) {
    if (target == nullptr || (instruction & 0xfc000000u) != 0x94000000u) return false;
    int64_t imm26 = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((imm26 & 0x02000000LL) != 0) imm26 |= ~0x03ffffffLL;
    const int64_t delta = imm26 * 4LL;
    *target = static_cast<uintptr_t>(static_cast<int64_t>(pc) + delta);
    return true;
}

"""
new_decode = old_decode + r'''bool validateBackProgressConvertOffsetBody(const LibraryInfo &library, uintptr_t candidate) {
    if (candidate == 0 || !libraryContainsRange(library, candidate, kBackProgressBodyProbeSize)) {
        return false;
    }

    bool sawInputMove = false;
    bool sawNegativeGuard = false;
    bool sawStockDistance110 = false;
    for (size_t offset = 0; offset < kBackProgressBodyProbeSize; offset += 4) {
        uint32_t insn = 0;
        std::memcpy(&insn, reinterpret_cast<const void *>(candidate + offset), sizeof(insn));
        sawInputMove |= insn == kFmovS8S0;
        sawNegativeGuard |= insn == kFcmpS8Zero;
        sawStockDistance110 |= insn == kMovW8Float110;
    }
    return sawInputMove && sawNegativeGuard && sawStockDistance110;
}

uintptr_t resolveBackProgressConvertOffsetTarget(
        const LibraryInfo &library, uintptr_t onSwipeProcessTarget,
        size_t *corroboratedCallsites) {
    if (corroboratedCallsites != nullptr) *corroboratedCallsites = 0;
    if (library.base == 0 || onSwipeProcessTarget == 0) return 0;

    struct Candidate {
        uintptr_t target = 0;
        size_t calls = 0;
    };
    std::array<Candidate, 16> candidates{};
    size_t candidateCount = 0;

    const uintptr_t end = onSwipeProcessTarget + kBackProgressCallerScanSize;
    for (uintptr_t pc = onSwipeProcessTarget; pc + 8u <= end; pc += 4u) {
        if (!libraryContainsRange(library, pc, 8)) break;
        uint32_t callInsn = 0;
        uint32_t nextInsn = 0;
        std::memcpy(&callInsn, reinterpret_cast<const void *>(pc), sizeof(callInsn));
        std::memcpy(&nextInsn, reinterpret_cast<const void *>(pc + 4u), sizeof(nextInsn));
        if (nextInsn != kFmovS1Twenty) continue;

        uintptr_t target = 0;
        if (!decodeBlTarget(pc, callInsn, &target)
                || !validateBackProgressConvertOffsetBody(library, target)) {
            continue;
        }

        size_t index = 0;
        for (; index < candidateCount; ++index) {
            if (candidates[index].target == target) break;
        }
        if (index == candidateCount) {
            if (candidateCount >= candidates.size()) return 0;
            candidates[candidateCount++].target = target;
        }
        ++candidates[index].calls;
    }

    uintptr_t found = 0;
    size_t foundCalls = 0;
    size_t qualified = 0;
    for (size_t i = 0; i < candidateCount; ++i) {
        // Direct APK validation: Launcher 8.01.02.5459 has 3 corroborating callsites and
        // 6174 has 4 in on_swipe_process. Requiring >=3 rejects incidental float helpers.
        if (candidates[i].calls < 3) continue;
        found = candidates[i].target;
        foundCalls = candidates[i].calls;
        ++qualified;
    }
    if (qualified != 1) return 0;
    if (corroboratedCallsites != nullptr) *corroboratedCallsites = foundCalls;
    return found;
}

using BackProgressConvertOffsetFn = float (*)(float);

float backProgressConvertOffsetHook(float distancePx) {
    const auto original = reinterpret_cast<BackProgressConvertOffsetFn>(
            gOriginalBackProgressConvertOffset.load(std::memory_order_acquire));
    if (original == nullptr) return distancePx;

    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0
            ? kStockBoundaryDp : std::max(configuredDp, kStockBoundaryDp);
    if (effectiveDp <= kStockBoundaryDp) return original(distancePx);

    // convert_offset's stock coordinate is 110dp with READY at progress 0.8, i.e. 88dp.
    // Scaling its pixel input by 88/customThreshold is density-independent and preserves
    // Xiaomi's own nonlinear easing after READY. At rawDx == customThreshold the original
    // function receives exactly the stock-equivalent 88dp distance.
    const float scale = static_cast<float>(kStockBoundaryDp) / static_cast<float>(effectiveDp);
    return original(distancePx * scale);
}

bool installBackProgressHook(
        const LibraryInfo &library, uintptr_t onSwipeProcessTarget, const char *source) {
    if (library.base == 0 || onSwipeProcessTarget == 0 || gHookFunction == nullptr) return false;
    if (gBackProgressHookInstalled.load(std::memory_order_acquire)) return true;

    size_t corroboratedCallsites = 0;
    const uintptr_t target = resolveBackProgressConvertOffsetTarget(
            library, onSwipeProcessTarget, &corroboratedCallsites);
    if (target == 0) {
        bool expected = false;
        if (gBackProgressResolveFailureLogged.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            logLine(ANDROID_LOG_WARN,
                    "PROGRESS_V1 convert_offset unresolved source=%s onSwipe=%p profile=%s; keeping legacy clamp fallback",
                    source == nullptr ? "unknown" : source,
                    reinterpret_cast<void *>(onSwipeProcessTarget), gActivePatternName);
        }
        return false;
    }

    void *backup = nullptr;
    const int rc = swipegate_install_protected_inline_hook(
            gHookFunction, reinterpret_cast<void *>(target),
            reinterpret_cast<void *>(backProgressConvertOffsetHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "PROGRESS_V1 convert_offset hook failed source=%s rc=%d target=%p backup=%p",
                source == nullptr ? "unknown" : source, rc,
                reinterpret_cast<void *>(target), backup);
        return false;
    }

    gOriginalBackProgressConvertOffset.store(backup, std::memory_order_release);
    gBackProgressConvertOffsetTarget.store(target, std::memory_order_release);
    gBackProgressHookInstalled.store(true, std::memory_order_release);
    gBackProgressResolveFailureLogged.store(false, std::memory_order_release);
    const uintptr_t rva = target >= library.base ? target - library.base : 0u;
    logLine(ANDROID_LOG_INFO,
            "PROGRESS_V1 convert_offset hook ready source=%s target=%p targetRva=0x%zx corroboratedCalls=%zu mapping=88dp/customThreshold refs=5459:0x773814,6174:0x60bb80",
            source == nullptr ? "unknown" : source,
            reinterpret_cast<void *>(target), static_cast<size_t>(rva), corroboratedCallsites);
    return true;
}

'''
replace_once(main, old_decode, new_decode, "insert progress resolver/hook")

old_gate = r'''float gateHorizontalDistance(bool readyFinish, uint32_t side, float horizontalDistancePx) {
    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0 ? kStockBoundaryDp : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const float stockBoundaryPx = densityDpi > 0 ? dpToPx(kStockBoundaryDp, densityDpi) : 0.0f;
    const float stockGuardPx = stockBoundaryPx > 1.0f ? stockBoundaryPx - 1.0f : 0.0f;
    const float userGatePx = densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);

    const bool delayBeyondStock = effectiveDp > kStockBoundaryDp && densityDpi > 0
            && stockGuardPx > 0.0f && userGatePx > stockBoundaryPx;
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

    // The module only fills the missing first-segment feedback. Xiaomi owns the
    // second-segment/commit feedback, so crossing into >= userGatePx must not replay
    // another vibration here. readyFinish identifies that the Back (first) segment is
    // actually armed; userGateReached separates the custom second segment.
    int hapticSegment = 0;
    if (delayBeyondStock && userGateReached) {
        hapticSegment = 2;
    } else if (readyFinish) {
        hapticSegment = 1;
    }
    if (hapticSegment == 2) {
        // Threshold / Three-hold remains 100% Xiaomi-owned. It invalidates Ready->Release
        // dedup until the gesture explicitly re-enters Ready, so threshold release is never
        // suppressed by the release-callsite policy.
        gReadyReleaseDedupEligible.store(false, std::memory_order_release);
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

    const int64_t now = monotonicMs();
    int64_t last = gLastSwipeLogMs.load(std::memory_order_relaxed);
    if (now - last >= 1000 && gLastSwipeLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE rawDx=%.2f effectiveDx=%.2f configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f stockBoundaryPx=%.2f guardPx=%.2f delayBeyondStock=%d gateReached=%d clamped=%d readyFinish=%d side=%u repairs=%llu",
                horizontalDistancePx, effectiveDistancePx, configuredDp, effectiveDp, densityDpi,
                userGatePx, stockBoundaryPx, stockGuardPx, delayBeyondStock ? 1 : 0,
                userGateReached ? 1 : 0, clamped ? 1 : 0, readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
    }
    return effectiveDistancePx;
}
'''
new_gate = r'''float gateHorizontalDistance(bool readyFinish, uint32_t side, float horizontalDistancePx) {
    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0 ? kStockBoundaryDp : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const float stockBoundaryPx = densityDpi > 0 ? dpToPx(kStockBoundaryDp, densityDpi) : 0.0f;
    const float stockGuardPx = stockBoundaryPx > 1.0f ? stockBoundaryPx - 1.0f : 0.0f;
    const float userGatePx = densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);

    const bool delayBeyondStock = effectiveDp > kStockBoundaryDp && densityDpi > 0
            && stockGuardPx > 0.0f && userGatePx > stockBoundaryPx;
    const bool userGateReached = !delayBeyondStock || absDx >= userGatePx;
    const bool progressHookReady = gBackProgressHookInstalled.load(std::memory_order_acquire);
    const float progressScale = effectiveDp > kStockBoundaryDp
            ? static_cast<float>(kStockBoundaryDp) / static_cast<float>(effectiveDp) : 1.0f;

    // Preferred path: keep Xiaomi's raw gesture distance untouched and move the shared
    // convert_offset progress coordinate instead. This preserves raw velocity/history while
    // stretching the Back animation continuously so progress 0.8 lands on customThreshold.
    // If the semantic progress resolver is unavailable on an unknown build, retain the old
    // 87.xdp clamp as a fail-safe rather than changing gesture semantics without proof.
    float effectiveDistancePx = horizontalDistancePx;
    bool clamped = false;
    const bool legacyClampFallback = delayBeyondStock && !progressHookReady;
    if (legacyClampFallback && !userGateReached && absDx > stockGuardPx) {
        effectiveDistancePx = std::copysign(stockGuardPx, horizontalDistancePx);
        clamped = true;
        gClampedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        gPassthroughCount.fetch_add(1, std::memory_order_relaxed);
    }

    // The module only fills the missing first-segment feedback. Xiaomi owns the
    // second-segment/commit feedback, so crossing into >= userGatePx must not replay
    // another vibration here. readyFinish identifies that the Back (first) segment is
    // actually armed; userGateReached separates the custom second segment.
    int hapticSegment = 0;
    if (delayBeyondStock && userGateReached) {
        hapticSegment = 2;
    } else if (readyFinish) {
        hapticSegment = 1;
    }
    if (hapticSegment == 2) {
        // Threshold / Three-hold remains 100% Xiaomi-owned. It invalidates Ready->Release
        // dedup until the gesture explicitly re-enters Ready, so threshold release is never
        // suppressed by the release-callsite policy.
        gReadyReleaseDedupEligible.store(false, std::memory_order_release);
    }
    const int previousHapticSegment = gHapticGestureSegment.exchange(
            hapticSegment, std::memory_order_acq_rel);
    if (hapticSegment == 1 && previousHapticSegment != 1) {
        const char *stage = previousHapticSegment == 2 ? "return-first" : "first";
        if (!performNativeHaptic(stage)) {
            // Do not consume the segment transition when the feedback could not actually be
            // emitted. Restoring the previous state lets a later frame retry as soon as the
            // HyperRT bridge is available instead of losing the first haptic forever.
            int expectedSegment = 1;
            if (gHapticGestureSegment.compare_exchange_strong(
                    expectedSegment, previousHapticSegment, std::memory_order_acq_rel)) {
                logLine(ANDROID_LOG_INFO,
                        "HAPTIC_V2 retry armed stage=%s previousSegment=%d reason=feedback-not-emitted",
                        stage, previousHapticSegment);
            }
        }
    }

    const int64_t now = monotonicMs();
    int64_t last = gLastSwipeLogMs.load(std::memory_order_relaxed);
    if (now - last >= 1000 && gLastSwipeLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE rawDx=%.2f effectiveDx=%.2f configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f stockBoundaryPx=%.2f delayBeyondStock=%d gateReached=%d progressHook=%d progressScale=%.4f legacyClamp=%d clamped=%d readyFinish=%d side=%u repairs=%llu",
                horizontalDistancePx, effectiveDistancePx, configuredDp, effectiveDp, densityDpi,
                userGatePx, stockBoundaryPx, delayBeyondStock ? 1 : 0,
                userGateReached ? 1 : 0, progressHookReady ? 1 : 0, progressScale,
                legacyClampFallback ? 1 : 0, clamped ? 1 : 0, readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
    }
    return effectiveDistancePx;
}
'''
replace_once(main, old_gate, new_gate, "replace clamp gate with progress-coordinate gate")

replace_once(
    main,
    "    // Runtime tracing on 6179 identified the real stock hand-up HyperRT callsite. Resolve\n"
    "    // its structural fingerprint now; no secondary Launcher function hook is required.\n"
    "    resolveAndPublishStockBackReleaseHapticCallsite(library, \"primary-hook-install\");\n"
    "    return true;\n",
    "    // Stretch Xiaomi's shared Back progress coordinate before relying on raw-distance\n"
    "    // passthrough. Unknown builds fall back to the legacy clamp inside gateHorizontalDistance.\n"
    "    installBackProgressHook(library, target, \"primary-hook-install\");\n"
    "    // Runtime tracing on 6179 identified the real stock hand-up HyperRT callsite. Resolve\n"
    "    // its structural fingerprint now; no secondary Launcher function hook is required.\n"
    "    resolveAndPublishStockBackReleaseHapticCallsite(library, \"primary-hook-install\");\n"
    "    return true;\n",
    "install progress hook with primary hook",
)

replace_once(
    main,
    "    gStockBackReleaseHapticCallsiteResolved.store(false, std::memory_order_release);\n"
    "    gStockBackReleaseHapticCallsite.store(0, std::memory_order_release);\n",
    "    gStockBackReleaseHapticCallsiteResolved.store(false, std::memory_order_release);\n"
    "    gStockBackReleaseHapticCallsite.store(0, std::memory_order_release);\n"
    "    gBackProgressHookInstalled.store(false, std::memory_order_release);\n"
    "    gBackProgressConvertOffsetTarget.store(0, std::memory_order_release);\n"
    "    gOriginalBackProgressConvertOffset.store(nullptr, std::memory_order_release);\n"
    "    gBackProgressResolveFailureLogged.store(false, std::memory_order_release);\n",
    "reset progress hook on remap",
)

old_health = r'''                const uintptr_t releaseCallsite = gStockBackReleaseHapticCallsite.load(
                        std::memory_order_acquire);
                logLine(ANDROID_LOG_INFO,
                        "HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu hapticCapture=%d releaseCallsiteReady=%d releaseCallsite=%p releaseCallsiteRva=0x%zx",
                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),
                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),
                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)),
                        gHapticCaptureHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
                        gStockBackReleaseHapticCallsiteResolved.load(std::memory_order_acquire) ? 1 : 0,
                        reinterpret_cast<void *>(releaseCallsite),
                        library.base != 0 && releaseCallsite >= library.base
                                ? static_cast<size_t>(releaseCallsite - library.base) : 0u);
'''
new_health = r'''                const uintptr_t releaseCallsite = gStockBackReleaseHapticCallsite.load(
                        std::memory_order_acquire);
                const uintptr_t progressTarget = gBackProgressConvertOffsetTarget.load(
                        std::memory_order_acquire);
                logLine(ANDROID_LOG_INFO,
                        "HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu hapticCapture=%d releaseCallsiteReady=%d releaseCallsite=%p releaseCallsiteRva=0x%zx progressHook=%d progressTarget=%p progressTargetRva=0x%zx",
                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),
                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),
                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)),
                        gHapticCaptureHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
                        gStockBackReleaseHapticCallsiteResolved.load(std::memory_order_acquire) ? 1 : 0,
                        reinterpret_cast<void *>(releaseCallsite),
                        library.base != 0 && releaseCallsite >= library.base
                                ? static_cast<size_t>(releaseCallsite - library.base) : 0u,
                        gBackProgressHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
                        reinterpret_cast<void *>(progressTarget),
                        library.base != 0 && progressTarget >= library.base
                                ? static_cast<size_t>(progressTarget - library.base) : 0u);
'''
replace_once(main, old_health, new_health, "health progress state")

replace_once(
    main,
    "            ensureHook(library, \"watchdog\");\n"
    "            if (!gStockBackReleaseHapticCallsiteResolved.load(std::memory_order_acquire)) {\n",
    "            ensureHook(library, \"watchdog\");\n"
    "            if (!gBackProgressHookInstalled.load(std::memory_order_acquire)) {\n"
    "                const uintptr_t onSwipeTarget = gHookedTarget.load(std::memory_order_acquire);\n"
    "                if (onSwipeTarget != 0) {\n"
    "                    installBackProgressHook(library, onSwipeTarget, \"watchdog\");\n"
    "                }\n"
    "            }\n"
    "            if (!gStockBackReleaseHapticCallsiteResolved.load(std::memory_order_acquire)) {\n",
    "watchdog progress retry",
)

replace_once(
    "app/build.gradle.kts",
    "        // 0.8.2-beta1: runtime-proven Ready/Release dedup; temporary tracing and legacy Arc state removed.\n"
    "        versionCode = 62\n"
    "        versionName = \"0.8.2-beta1\"\n",
    "        // 0.8.2-beta2: scale Launcher Back progress coordinate so animation READY matches custom threshold.\n"
    "        versionCode = 63\n"
    "        versionName = \"0.8.2-beta2\"\n",
    "version bump",
)

text = Path(main).read_text()
required = [
    "PROGRESS_V1 convert_offset hook ready",
    "mapping=88dp/customThreshold",
    "gBackProgressHookInstalled",
    "progressScale=%.4f",
    "legacyClampFallback",
    "validateBackProgressConvertOffsetBody",
    "candidates[i].calls < 3",
    "kMovW8Float110",
    "kReadyReleaseDedupMs = 750",
    "ready-release-callsite",
]
for needle in required:
    if needle not in text:
        raise SystemExit(f"missing invariant: {needle}")

# One-shot staging files remove themselves after the patch is committed.
Path("scripts/apply_back_progress_scale.py").unlink()
Path(".github/workflows/run-back-progress-scale.yml").unlink()
