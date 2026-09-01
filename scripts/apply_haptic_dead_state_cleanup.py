from pathlib import Path

p = Path("native/src/main.cpp")
text = p.read_text()

singletons = [
    "std::atomic<uintptr_t> gCapturedHapticArc{0};\n",
    "std::atomic<int64_t> gCapturedHapticArcAtMs{0};\n",
    "std::atomic<int64_t> gNativeHapticSuppressUntilMs{0};\n",
    "std::atomic<int64_t> gLastInjectedHapticAtMs{0};\n",
]
for item in singletons:
    count = text.count(item)
    if count != 1:
        raise SystemExit(f"expected exactly one legacy declaration {item.strip()!r}, got {count}")
    text = text.replace(item, "", 1)

old_block = '''    if (storage != nullptr) {\n        uintptr_t rawArc = 0;\n        std::memcpy(&rawArc, storage, sizeof(rawArc));\n        const uintptr_t addressBits = rawArc & kPointerAddressMask;\n        if (addressBits >= 0x10000u) {\n            // Preserve the original tagged pointer exactly as Xiaomi supplied it.  The\n            // untagged value is used only for sanity checking, never for replay.\n            gCapturedHapticArc.store(rawArc, std::memory_order_release);\n            gCapturedHapticArcAtMs.store(now, std::memory_order_release);\n            gHapticUnavailableLogged.store(false, std::memory_order_release);\n        }\n    }\n\n'''
if text.count(old_block) != 1:
    raise SystemExit(f"expected one obsolete Arc capture block, got {text.count(old_block)}")
text = text.replace(old_block, "", 1)

old_comment = '''    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points\n    // at the instruction immediately after the real callsite in libapp_launcher.so. Capture\n    // it before doing any other work. Synthetic Ready feedback uses the original trampoline\n    // and therefore never enters this function.\n'''
new_comment = '''    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points\n    // at the instruction immediately after the real callsite in libapp_launcher.so. Read it\n    // once for callsite-scoped dedup. Synthetic Ready uses the original trampoline and never\n    // enters this function.\n'''
if text.count(old_comment) != 1:
    raise SystemExit(f"capture comment mismatch: {text.count(old_comment)}")
text = text.replace(old_comment, new_comment, 1)

for forbidden in [
    "gCapturedHapticArc",
    "gCapturedHapticArcAtMs",
    "gNativeHapticSuppressUntilMs",
    "gLastInjectedHapticAtMs",
    "rawArc",
    "addressBits",
]:
    if forbidden in text:
        raise SystemExit(f"legacy haptic state still present: {forbidden}")
for required in [
    "__builtin_return_address(0)",
    "gStockBackReleaseHapticCallsite",
    "HAPTIC_V2 release suppressed reason=ready-release-callsite",
    "HAPTIC_V2 release preserved reason=callsite-window",
    "kReadyReleaseDedupMs = 750",
]:
    if required not in text:
        raise SystemExit(f"production invariant missing: {required}")

p.write_text(text)

# One-shot staging files do not belong in production.
Path("scripts/apply_haptic_dead_state_cleanup.py").unlink()
Path(".github/workflows/run-haptic-dead-state-cleanup.yml").unlink()
