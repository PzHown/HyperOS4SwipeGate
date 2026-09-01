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

text = text.replace("#include <sys/syscall.h>\n", "", 1)
text = text.replace("std::atomic<uint64_t> gHapticTraceSequence{0};\n", "", 1)

start = text.find("const char *traceModuleBasename(const char *path) {")
end = text.find("__attribute__((noinline)) void hapticFeedbackCaptureHook", start)
if start < 0 or end < 0 or end <= start:
    raise SystemExit("trace helper block not found")
text = text[:start] + text[end:]

old_hook = """__attribute__((noinline)) void hapticFeedbackCaptureHook(void *storage, int32_t constant) {\n    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points\n    // at the instruction immediately after the real callsite in libapp_launcher.so. Capture\n    // it before doing any other work. Synthetic Ready feedback uses the original trampoline\n    // and therefore never enters this function.\n    void *rawReturnAddress = __builtin_return_address(0);\n    const uintptr_t callerReturnAddress = reinterpret_cast<uintptr_t>(\n            __builtin_extract_return_addr(rawReturnAddress)) & kPointerAddressMask;\n    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n    traceStockHapticCall(constant, now, callerReturnAddress);\n"""
new_hook = """__attribute__((noinline)) void hapticFeedbackCaptureHook(void *storage, int32_t constant) {\n    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points\n    // at the instruction immediately after the real callsite in libapp_launcher.so. Read LR\n    // once for callsite-scoped dedup; do not run dladdr/log tracing on the hot haptic path.\n    // Synthetic Ready feedback uses the original trampoline and never enters this function.\n    void *rawReturnAddress = __builtin_return_address(0);\n    const uintptr_t callerReturnAddress = reinterpret_cast<uintptr_t>(\n            __builtin_extract_return_addr(rawReturnAddress)) & kPointerAddressMask;\n    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n"""
if text.count(old_hook) != 1:
    raise SystemExit(f"capture hook prologue: expected 1 match, got {text.count(old_hook)}")
text = text.replace(old_hook, new_hook, 1)

# Production convergence invariants: retain semantic callsite resolution and only the
# actionable release outcome logs. The temporary stock-call trace must be completely gone.
for forbidden in [
    "HAPTIC_TRACE stock-call",
    "traceStockHapticCall",
    "traceModuleBasename",
    "gHapticTraceSequence",
    "SYS_gettid",
]:
    if forbidden in text:
        raise SystemExit(f"temporary trace residue remains: {forbidden}")
for required in [
    "HAPTIC_V2 release suppressed reason=ready-release-callsite",
    "HAPTIC_V2 release preserved reason=callsite-window",
    "HAPTIC_V2 release callsite ready source=%s feature=%s",
    "stock-back-release-haptic-v1",
    "kReadyReleaseDedupMs = 750",
    "gStockBackReleaseHapticCallsite",
]:
    if required not in text:
        raise SystemExit(f"required production invariant missing: {required}")
main.write_text(text)

replace_once(
    "app/build.gradle.kts",
    "        // dev.18: dedup the runtime-proven Launcher 8.x stock Back release HyperRT callsite (6174/6179).\n"
    "        versionCode = 60\n"
    "        versionName = \"0.8.1-dev.18\"\n",
    "        // dev.19: converge runtime-proven release dedup; remove temporary hot-path tracing.\n"
    "        versionCode = 61\n"
    "        versionName = \"0.8.1-dev.19\"\n",
    "version bump",
)

# One-shot staging files remove themselves in the resulting source commit.
Path("scripts/apply_haptic_log_convergence.py").unlink()
Path(".github/workflows/run-haptic-log-convergence.yml").unlink()
