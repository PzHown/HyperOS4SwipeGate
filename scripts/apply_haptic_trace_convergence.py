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

# dev.17's provider-wide caller tracing served its purpose. Keep only the LR/callsite
# classification required by the production dedup path; remove high-frequency trace logs
# and their trace-only state/dependencies.
text = text.replace("#include <sys/syscall.h>\n", "", 1)
text = text.replace("std::atomic<uint64_t> gHapticTraceSequence{0};\n", "", 1)

start = text.find("const char *traceModuleBasename(const char *path) {")
end = text.find("__attribute__((noinline)) void hapticFeedbackCaptureHook", start)
if start < 0 or end < 0:
    raise SystemExit("trace helper block not found")
text = text[:start] + text[end:]

old = """    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n    traceStockHapticCall(constant, now, callerReturnAddress);\n"""
new = """    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n"""
if text.count(old) != 1:
    raise SystemExit(f"trace invocation block: expected 1 match, got {text.count(old)}")
text = text.replace(old, new, 1)

text = text.replace(
    "    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points\n"
    "    // at the instruction immediately after the real callsite in libapp_launcher.so. Capture\n"
    "    // it before doing any other work. Synthetic Ready feedback uses the original trampoline\n"
    "    // and therefore never enters this function.\n",
    "    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points\n"
    "    // at the instruction immediately after the real Launcher callsite. Capture it first and\n"
    "    // classify only against the semantically resolved stock Back-release instruction.\n"
    "    // Synthetic Ready uses the original trampoline and never enters this function.\n",
    1,
)
text = text.replace(
    "    // Synthetic Ready uses the original HyperRT trampoline, outside the stock\n"
    "    // release-helper scope, so it can never suppress itself.\n",
    "    // Synthetic Ready uses the original HyperRT trampoline and bypasses the provider hook,\n"
    "    // so callsite-scoped Release dedup can never suppress the synthetic feedback itself.\n",
    1,
)

# Production source must not retain dev.17's high-frequency trace machinery.
for forbidden in [
    "HAPTIC_TRACE stock-call",
    "traceStockHapticCall(",
    "traceModuleBasename(",
    "gHapticTraceSequence",
    "SYS_gettid",
    "syscall(",
]:
    if forbidden in text:
        raise SystemExit(f"trace-only token still present: {forbidden}")

# Keep the evidence-backed resolver and the two low-rate outcome logs.
for required in [
    "stock-back-release-haptic-v1",
    "HAPTIC_V2 release callsite ready",
    "HAPTIC_V2 release suppressed reason=ready-release-callsite",
    "HAPTIC_V2 release preserved reason=callsite-window",
    "kReadyReleaseDedupMs = 750",
    "releaseMatch",  # no longer logged; should not exist after convergence
]:
    if required == "releaseMatch":
        if required in text:
            raise SystemExit("trace-only releaseMatch state still present")
    elif required not in text:
        raise SystemExit(f"production invariant missing: {required}")

main.write_text(text)

replace_once(
    "app/build.gradle.kts",
    "        // dev.18: dedup the runtime-proven Launcher 8.x stock Back release HyperRT callsite.\n"
    "        versionCode = 60\n"
    "        versionName = \"0.8.1-dev.18\"\n",
    "        // dev.19: converge the proven release-callsite dedup and remove provider-wide trace noise.\n"
    "        versionCode = 61\n"
    "        versionName = \"0.8.1-dev.19\"\n",
    "version bump",
)

# One-shot staging only.
Path("scripts/apply_haptic_trace_convergence.py").unlink()
Path(".github/workflows/run-haptic-trace-convergence.yml").unlink()
