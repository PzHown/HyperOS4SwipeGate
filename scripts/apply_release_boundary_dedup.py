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


main = Path("native/src/main.cpp")
text = main.read_text()

# The old design armed dedup at GestureInputBackHelper::on_back_invoke. 6174 reverse
# analysis proves the stock hand-up vibration occurs earlier, synchronously inside
# GestureBackArrowView::check_and_perform_haptic_feedback from on_swipe_stop. Remove the
# late commit wrapper and resolve/hook the true release-feedback boundary instead.
old_extern = """__attribute__((visibility(\"hidden\"))) extern void *gSwipeGateOriginalOnBackInvoke;\n__attribute__((visibility(\"hidden\"))) void swipegate_on_back_invoke_hook();\n__attribute__((visibility(\"hidden\"))) void swipegate_haptic_on_back_invoke();\n__attribute__((visibility(\"hidden\"))) void swipegate_haptic_on_back_invoke_exit();\n"""
if text.count(old_extern) != 1:
    raise SystemExit("back-invoke extern block missing")
text = text.replace(old_extern, "", 1)
main.write_text(text)

release_pattern = """// Launcher 8.0.x GestureBackArrowView::check_and_perform_haptic_feedback entry.\n// 6174 disassembly starts with:\n//   ldr w8,[x0,#0x18] / ldrb w9,[x0,#0x20] / cbnz feedback_done /\n//   cmp w8,#4 / b.hi / and w8,w8,#0xff / cmp w8,#3 / b.ne.\n// Branch immediates are masked; register/opcode/state constants remain exact. A candidate\n// is accepted only when the same function also contains the imported HyperRT haptic call\n// followed by feedback_done=true, so this is a semantic 8.x feature fingerprint rather\n// than a hard-coded RVA.\nconstexpr uint8_t kReleaseFeedbackPatternV1[] = {\n        0x08,0x18,0x40,0xb9, 0x09,0x80,0x40,0x39, 0x09,0x00,0x00,0x35,\n        0x1f,0x11,0x00,0x71, 0x08,0x00,0x00,0x54, 0x08,0x1d,0x00,0x12,\n        0x1f,0x0d,0x00,0x71, 0x01,0x00,0x00,0x54,\n};\nconstexpr uint8_t kReleaseFeedbackMaskV1[] = {\n        0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0x1f,0x00,0x00,0xff,\n        0xff,0xff,0xff,0xff, 0x1f,0x00,0x00,0xff, 0xff,0xff,0xff,0xff,\n        0xff,0xff,0xff,0xff, 0x1f,0x00,0x00,0xff,\n};\nconstexpr PatternSpec kReleaseFeedbackPattern = {\n        \"release-ready-state-back-v1\", kReleaseFeedbackPatternV1,\n        kReleaseFeedbackMaskV1, sizeof(kReleaseFeedbackPatternV1),\n};\n\n"""
replace_span(
    "native/src/main.cpp",
    "// Known-good GestureInputBackHelper::on_back_invoke entry patterns.",
    "static_assert(sizeof(kOnSwipeProcessPatternV1)",
    release_pattern,
    "replace back-invoke patterns",
)
replace_once(
    "native/src/main.cpp",
    "static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));\n",
    "static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));\n"
    "static_assert(sizeof(kReleaseFeedbackPatternV1) == sizeof(kReleaseFeedbackMaskV1));\n",
    "release pattern static assert",
)

state_block = """std::atomic<int64_t> gLastReadyHapticAtMs{0};\nstd::atomic<bool> gReadyReleaseDedupEligible{false};\n// Launcher 8.0.x emits the stock hand-up vibration synchronously from\n// GestureBackArrowView::check_and_perform_haptic_feedback during on_swipe_stop. The\n// boundary hook keeps Xiaomi's state bookkeeping intact and only scopes the nested\n// constant=0 HyperRT call for one-shot suppression.\nstd::atomic<void *> gOriginalReleaseFeedbackHelper{nullptr};\nstd::atomic<uintptr_t> gReleaseFeedbackTarget{0};\nstd::atomic<bool> gReleaseFeedbackHookInstalled{false};\nthread_local bool gReleaseFeedbackScopeArmed = false;\nthread_local int64_t gReleaseFeedbackScopeReadyAtMs = 0;\n// Haptic segment state is independent from the Launcher hook health state.\n// 0 = outside/idle, 1 = first segment (Back, below custom threshold),\n// 2 = second segment (custom threshold reached). Only entering segment 1 is replayed.\nstd::atomic<int> gHapticGestureSegment{0};\n\n"""
replace_span(
    "native/src/main.cpp",
    "std::atomic<int64_t> gLastReadyHapticAtMs{0};",
    "std::mutex gHookMutex;",
    state_block,
    "replace release dedup state",
)

capture_replacement = """    // The stock 8.0.x hand-up effect is synchronous inside the resolved\n    // GestureBackArrowView release helper. Suppress only the nested constant=0 call while\n    // that exact helper is active. The helper itself still runs and sets feedback_done,\n    // preserving Xiaomi's release state machine.\n    if (constant == kHapticConstant && gReleaseFeedbackScopeArmed) {\n        gReleaseFeedbackScopeArmed = false;\n        const int64_t readyAt = gReleaseFeedbackScopeReadyAtMs;\n        gReleaseFeedbackScopeReadyAtMs = 0;\n        logLine(ANDROID_LOG_INFO,\n                \"HAPTIC_V2 release suppressed reason=ready-release-boundary constant=%d deltaMs=%lld windowMs=%lld source=GestureBackArrowView::check_and_perform_haptic_feedback\",\n                constant,\n                readyAt > 0 && now >= readyAt\n                        ? static_cast<long long>(now - readyAt) : -1LL,\n                static_cast<long long>(kReadyReleaseDedupMs));\n        return;\n    }\n\n"""
replace_span(
    "native/src/main.cpp",
    "    // A Release haptic may arrive after on_back_invoke has returned or on another\n",
    "    original(storage, constant);\n",
    capture_replacement,
    "replace capture token suppression",
)

replace_once(
    "native/src/main.cpp",
    "    // A new Ready segment supersedes any delayed Release token left by the\n"
    "    // previous gesture. The synthetic Ready call uses the original trampoline, so it\n"
    "    // cannot consume this token itself.\n"
    "    gReleaseDedupTokenDeadlineMs.store(0, std::memory_order_release);\n"
    "    gReleaseDedupTokenReadyAtMs.store(0, std::memory_order_release);\n\n",
    "    // Synthetic Ready uses the original HyperRT trampoline, outside the stock\n"
    "    // release-helper scope, so it can never suppress itself.\n\n",
    "remove stale release token clear",
)

release_resolver = r'''using ReleaseFeedbackHelperFn = void (*)(void *);

bool releaseFeedbackCandidateHasHapticCall(
        const LibraryInfo &library, uintptr_t candidate, uintptr_t hapticSlot) {
    if (candidate == 0 || hapticSlot == 0 || !libraryContainsRange(library, candidate, 0xc0)) {
        return false;
    }
    for (uintptr_t pc = candidate + 0x20; pc <= candidate + 0xb0; pc += 4) {
        uint32_t callInsn = 0;
        std::memcpy(&callInsn, reinterpret_cast<const void *>(pc), sizeof(callInsn));
        uintptr_t callTarget = 0;
        if (!decodeBlTarget(pc, callInsn, &callTarget)
                || !pltReferencesSlot(library, callTarget, hapticSlot)) {
            continue;
        }
        if (!libraryContainsRange(library, pc + 16, 4)) return false;
        uint32_t movFeedbackDone = 0;
        uint32_t storeFeedbackDone = 0;
        std::memcpy(&movFeedbackDone,
                    reinterpret_cast<const void *>(pc + 12), sizeof(movFeedbackDone));
        std::memcpy(&storeFeedbackDone,
                    reinterpret_cast<const void *>(pc + 16), sizeof(storeFeedbackDone));
        // 6174: mov w8,#1 ; strb w8,[x19,#0x20]. This corroborates that the call is
        // check_and_perform_haptic_feedback rather than an unrelated constant=0 site.
        if (movFeedbackDone == 0x52800028u && storeFeedbackDone == 0x39008268u) {
            return true;
        }
    }
    return false;
}

uintptr_t resolveReleaseFeedbackTarget(const LibraryInfo &library, const char **featureName) {
    if (featureName != nullptr) *featureName = nullptr;
    const ImportedFunctionResolution haptic = resolveImportedFunction(library, kHapticSymbol);
    if (haptic.matches != 1 || haptic.slot == 0) {
        if (featureName != nullptr) *featureName = "haptic-import-unresolved";
        return 0;
    }

    uintptr_t found = 0;
    size_t matches = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < kReleaseFeedbackPattern.size) continue;
        const uintptr_t first = (range.start + 3u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - kReleaseFeedbackPattern.size;
        for (uintptr_t cursor = first; cursor <= last; cursor += 4u) {
            if (!patternMatchesAt(cursor, kReleaseFeedbackPattern)) continue;
            if (!releaseFeedbackCandidateHasHapticCall(library, cursor, haptic.slot)) continue;
            found = cursor;
            if (++matches > 1) {
                if (featureName != nullptr) *featureName = "ambiguous";
                return 0;
            }
        }
    }
    if (matches == 1 && featureName != nullptr) {
        *featureName = kReleaseFeedbackPattern.name;
    }
    return matches == 1 ? found : 0;
}

void releaseFeedbackHelperHook(void *arrowState) {
    const auto original = reinterpret_cast<ReleaseFeedbackHelperFn>(
            gOriginalReleaseFeedbackHelper.load(std::memory_order_acquire));
    if (original == nullptr) return;

    const int64_t now = monotonicMs();
    const bool eligible = gReadyReleaseDedupEligible.exchange(false, std::memory_order_acq_rel);
    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
    const int64_t delta = readyAt > 0 && now >= readyAt ? now - readyAt : -1;
    const bool suppress = eligible && delta >= 0 && delta < kReadyReleaseDedupMs
            && gHapticCaptureHookInstalled.load(std::memory_order_acquire);

    gReleaseFeedbackScopeReadyAtMs = suppress ? readyAt : 0;
    gReleaseFeedbackScopeArmed = suppress;
    if (suppress) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release boundary armed deltaMs=%lld windowMs=%lld semantic=READY_STATE_BACK",
                static_cast<long long>(delta), static_cast<long long>(kReadyReleaseDedupMs));
    }

    // Do not skip Xiaomi's helper. The nested HyperRT call is intercepted instead, so
    // feedback_done and all release-state bookkeeping remain stock.
    original(arrowState);

    gReleaseFeedbackScopeArmed = false;
    gReleaseFeedbackScopeReadyAtMs = 0;
    gHapticGestureSegment.store(0, std::memory_order_release);
}

bool installReleaseFeedbackHapticHook(const LibraryInfo &library) {
    if (library.base == 0 || gHookFunction == nullptr) return false;
    const char *featureName = nullptr;
    const uintptr_t target = resolveReleaseFeedbackTarget(library, &featureName);
    if (target == 0) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 release boundary unresolved feature=%s activeProfile=%s; stock release remains untouched",
                featureName == nullptr ? "none" : featureName, gActivePatternName);
        return false;
    }
    if (gReleaseFeedbackHookInstalled.load(std::memory_order_acquire)
            && gReleaseFeedbackTarget.load(std::memory_order_acquire) == target) {
        return true;
    }

    void *backup = nullptr;
    const int rc = swipegate_install_protected_inline_hook(
            gHookFunction, reinterpret_cast<void *>(target),
            reinterpret_cast<void *>(releaseFeedbackHelperHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC_V2 release boundary hook failed rc=%d target=%p backup=%p",
                rc, reinterpret_cast<void *>(target), backup);
        return false;
    }

    gOriginalReleaseFeedbackHelper.store(backup, std::memory_order_release);
    gReleaseFeedbackTarget.store(target, std::memory_order_release);
    gReleaseFeedbackHookInstalled.store(true, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 release boundary hook ready feature=%s activeProfile=%s target=%p semantic=READY_STATE_BACK synchronous=1",
            featureName == nullptr ? "unknown" : featureName, gActivePatternName,
            reinterpret_cast<void *>(target));
    return true;
}

'''
replace_span(
    "native/src/main.cpp",
    "uintptr_t resolveUniqueAuxPattern(\n",
    "float gateHorizontalDistance(",
    release_resolver,
    "replace back-invoke resolver with release boundary",
)

replace_once(
    "native/src/main.cpp",
    "    if (hapticSegment == 2) {\n"
    "        // Threshold / Three-hold remains 100% Xiaomi-owned and cancels Ready->Release dedup\n"
    "        // until the gesture explicitly re-enters Ready. Also invalidate a not-yet-consumed\n"
    "        // Release token defensively so Threshold feedback can never be swallowed.\n"
    "        gReadyReleaseDedupEligible.store(false, std::memory_order_release);\n"
    "        gReleaseDedupTokenDeadlineMs.store(0, std::memory_order_release);\n"
    "        gReleaseDedupTokenReadyAtMs.store(0, std::memory_order_release);\n"
    "    }\n",
    "    if (hapticSegment == 2) {\n"
    "        // Threshold / Three-hold remains 100% Xiaomi-owned. It invalidates Ready->Release\n"
    "        // dedup until the gesture explicitly re-enters Ready, so threshold release is never\n"
    "        // suppressed by the release-helper scope.\n"
    "        gReadyReleaseDedupEligible.store(false, std::memory_order_release);\n"
    "    }\n",
    "threshold invalidation",
)

replace_once(
    "native/src/main.cpp",
    "    // on_back_invoke is only a scope marker for Ready->Release dedup; it does not create haptics.\n"
    "    installBackInvokeHapticHook(library);\n",
    "    // 8.0.x stock hand-up lives in GestureBackArrowView::on_swipe_stop ->\n"
    "    // check_and_perform_haptic_feedback, before on_back_invoke. Hook that exact helper.\n"
    "    installReleaseFeedbackHapticHook(library);\n",
    "install release boundary",
)

replace_once(
    "native/src/main.cpp",
    "    gBackInvokeHookInstalled.store(false, std::memory_order_release);\n"
    "    gBackInvokeTarget.store(0, std::memory_order_release);\n"
    "    __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);\n",
    "    gReleaseFeedbackHookInstalled.store(false, std::memory_order_release);\n"
    "    gReleaseFeedbackTarget.store(0, std::memory_order_release);\n"
    "    gOriginalReleaseFeedbackHelper.store(nullptr, std::memory_order_release);\n",
    "reset release boundary mapping",
)

replace_once(
    "native/src/main.cpp",
    "            ensureHook(library, \"watchdog\");\n"
    "            swipegate_back_break_maintain();\n",
    "            ensureHook(library, \"watchdog\");\n"
    "            if (!gReleaseFeedbackHookInstalled.load(std::memory_order_acquire)) {\n"
    "                installReleaseFeedbackHapticHook(library);\n"
    "            }\n"
    "            swipegate_back_break_maintain();\n",
    "watchdog release boundary retry",
)

replace_once(
    "native/src/main.cpp",
    "__attribute__((visibility(\"hidden\"))) void *gSwipeGateOriginalOnSwipeProcess = nullptr;\n"
    "__attribute__((visibility(\"hidden\"))) void *gSwipeGateOriginalOnBackInvoke = nullptr;\n",
    "__attribute__((visibility(\"hidden\"))) void *gSwipeGateOriginalOnSwipeProcess = nullptr;\n",
    "remove back invoke original",
)

replace_span(
    "native/src/main.cpp",
    "__attribute__((visibility(\"hidden\"))) void swipegate_haptic_on_back_invoke() {\n",
    "}\n\nextern \"C\" __attribute__((visibility(\"default\"), used))\nNativeOnModuleLoaded native_init",
    "",
    "remove back invoke haptic callbacks",
)

replace_once(
    "native/src/main.cpp",
    "            \"HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 constant=0\",\n",
    "            \"HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 release-boundary=GestureBackArrowView constant=0\",\n",
    "native init haptic policy log",
)

# Remove the obsolete ABI wrapper completely. The new release helper is a verified one-argument
# native function and is hooked from C++; the original helper still executes in full.
asm = Path("native/src/swipe_hook_entry.S")
asm_text = asm.read_text()
marker = "// ABI-transparent commit lifecycle hook."
if asm_text.count(marker) != 1:
    raise SystemExit("assembly back-invoke marker missing")
asm.write_text(asm_text[:asm_text.index(marker)].rstrip() + "\n")

# Version bump. Keep literal versionCode because build.yml intentionally parses that form.
replace_once(
    "app/build.gradle.kts",
    "        // dev.15: end-to-end loaded-version handshake for reliable restart-required detection.\n"
    "        versionCode = 57\n"
    "        versionName = \"0.8.1-dev.15\"\n",
    "        // dev.16: Launcher 8.x release-boundary haptic dedup at GestureBackArrowView.\n"
    "        versionCode = 58\n"
    "        versionName = \"0.8.1-dev.16\"\n",
    "version bump",
)

# Final source-level invariants.
main_text = Path("native/src/main.cpp").read_text()
asm_text = Path("native/src/swipe_hook_entry.S").read_text()
for forbidden in [
    "gReleaseDedupTokenDeadlineMs",
    "gReleaseDedupTokenReadyAtMs",
    "gSwipeGateOriginalOnBackInvoke",
    "installBackInvokeHapticHook",
    "swipegate_haptic_on_back_invoke",
    "swipegate_on_back_invoke_hook",
]:
    if forbidden in main_text or forbidden in asm_text:
        raise SystemExit(f"stale back-invoke dedup symbol remains: {forbidden}")
for required in [
    "release-ready-state-back-v1",
    "releaseFeedbackCandidateHasHapticCall",
    "releaseFeedbackHelperHook",
    "reason=ready-release-boundary",
    "semantic=READY_STATE_BACK",
    "kReadyReleaseDedupMs = 750",
]:
    if required not in main_text:
        raise SystemExit(f"required release-boundary marker missing: {required}")
