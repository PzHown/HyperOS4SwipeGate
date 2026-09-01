from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


main = Path("native/src/main.cpp")
text = main.read_text()

# Remove dev.17 caller-trace-only dependencies/state.
text = text.replace("#include <sys/syscall.h>\n", "")
text = text.replace("std::atomic<uint64_t> gHapticTraceSequence{0};\n", "")

trace_start = text.find("const char *traceModuleBasename(const char *path) {")
trace_end = text.find("__attribute__((noinline)) void hapticFeedbackCaptureHook", trace_start)
if trace_start < 0 or trace_end < 0:
    raise SystemExit("caller trace block not found")
text = text[:trace_start] + text[trace_end:]

old_hook_prefix = '''    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n    traceStockHapticCall(constant, now, callerReturnAddress);\n'''
new_hook_prefix = '''    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n'''
if text.count(old_hook_prefix) != 1:
    raise SystemExit(f"trace hook call: expected 1 match, got {text.count(old_hook_prefix)}")
text = text.replace(old_hook_prefix, new_hook_prefix, 1)

# Synthetic Ready is production behavior now; don't emit one success line per gesture.
old_ready_success = '''    gHapticUnavailableLogged.store(false, std::memory_order_release);\n    logLine(ANDROID_LOG_INFO,\n            "HAPTIC_V2 feedback stage=%s kind=hyperrt-stock constant=%d readyReleaseDedupMs=%lld",\n            stage == nullptr ? "unknown" : stage, kHapticConstant,\n            static_cast<long long>(kReadyReleaseDedupMs));\n    return true;\n'''
new_ready_success = '''    gHapticUnavailableLogged.store(false, std::memory_order_release);\n    return true;\n'''
if text.count(old_ready_success) != 1:
    raise SystemExit(f"ready success log: expected 1 match, got {text.count(old_ready_success)}")
text = text.replace(old_ready_success, new_ready_success, 1)

text = text.replace(
    "    // Synthetic Ready uses the original HyperRT trampoline, outside the stock\n"
    "    // release-helper scope, so it can never suppress itself.\n",
    "    // Synthetic Ready uses the original HyperRT trampoline, bypassing the provider\n"
    "    // capture hook, so it can never be mistaken for Xiaomi's stock release callsite.\n",
    1,
)
text = text.replace(
    "        // suppressed by the release-helper scope.\n",
    "        // suppressed by the release-callsite policy.\n",
    1,
)

# Make the production intent explicit at init; runtime caller tracing is gone.
old_init = (
    '            "HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 '
    'release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 '
    'release-source=GestureStubViewWindow::handle_back_gesture callsite-scoped=1 constant=0",\n'
)
new_init = (
    '            "HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 '
    'release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 '
    'release-source=GestureStubViewWindow::handle_back_gesture callsite-scoped=1 '
    'runtime-trace=0 constant=0",\n'
)
if text.count(old_init) != 1:
    raise SystemExit(f"native init haptic policy: expected 1 match, got {text.count(old_init)}")
text = text.replace(old_init, new_init, 1)

main.write_text(text)

replace_once(
    "app/build.gradle.kts",
    "        // dev.18: dedup the runtime-proven Launcher 8.x stock Back release HyperRT callsite (6174/6179).\n"
    "        versionCode = 60\n"
    "        versionName = \"0.8.1-dev.18\"\n",
    "        // dev.19: production cleanup after runtime-proven release-callsite dedup.\n"
    "        versionCode = 61\n"
    "        versionName = \"0.8.1-dev.19\"\n",
    "version bump",
)

# Production invariants: no dev caller trace, exact callsite dedup still intact, and only
# actionable release/resolver logs remain for the haptic hand-up path.
text = main.read_text()
for forbidden in [
    "HAPTIC_TRACE",
    "traceStockHapticCall",
    "traceModuleBasename",
    "gHapticTraceSequence",
    "SYS_gettid",
    "feedback stage=%s kind=hyperrt-stock",
]:
    if forbidden in text:
        raise SystemExit(f"production trace residue remains: {forbidden}")

for required in [
    "stock-back-release-haptic-v1",
    "HAPTIC_V2 release callsite ready",
    "HAPTIC_V2 release callsite unresolved",
    "HAPTIC_V2 release suppressed reason=ready-release-callsite",
    "HAPTIC_V2 release preserved reason=callsite-window",
    "callsite == releaseCallsite",
    "kReadyReleaseDedupMs = 750",
    "gHapticGestureSegment.store(0, std::memory_order_release)",
    "runtime-trace=0",
]:
    if required not in text:
        raise SystemExit(f"production invariant missing: {required}")

# One-shot staging files must not survive the cleanup commit.
Path("scripts/apply_haptic_production_cleanup.py").unlink()
Path(".github/workflows/run-haptic-production-cleanup.yml").unlink()
