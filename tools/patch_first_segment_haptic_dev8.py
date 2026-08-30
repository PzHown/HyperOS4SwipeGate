from pathlib import Path

main = Path('native/src/main.cpp')
text = main.read_text()

old_state = 'std::atomic<bool> gReadyHapticLatched{false};\n'
new_state = '''// Haptic segment state is independent from the Launcher hook health state.\n// 0 = outside/idle, 1 = first segment (Back, below custom threshold),\n// 2 = second segment (custom threshold reached). Only entering segment 1 is replayed.\nstd::atomic<int> gHapticGestureSegment{0};\n'''
assert text.count(old_state) == 1
text = text.replace(old_state, new_state, 1)

old_gate = '''    // Custom thresholds use the custom boundary. Stock mode follows Launcher\n    // readyFinish. Trigger only on the false -> true boundary and re-arm after\n    // the gesture leaves ready state. No mutex or symbol lookup occurs here.\n    const bool hapticReady = delayBeyondStock ? userGateReached : readyFinish;\n    if (hapticReady) {\n        bool expected = false;\n        if (gReadyHapticLatched.compare_exchange_strong(\n                expected, true, std::memory_order_acq_rel)) {\n            performNativeHaptic("ready", true);\n        }\n    } else {\n        gReadyHapticLatched.store(false, std::memory_order_release);\n    }\n'''
new_gate = '''    // The module only fills the missing first-segment feedback. Xiaomi owns the\n    // second-segment/commit feedback, so crossing into >= userGatePx must not replay\n    // another vibration here. readyFinish identifies that the Back (first) segment is\n    // actually armed; userGateReached separates the custom second segment.\n    int hapticSegment = 0;\n    if (delayBeyondStock && userGateReached) {\n        hapticSegment = 2;\n    } else if (readyFinish) {\n        hapticSegment = 1;\n    }\n    const int previousHapticSegment = gHapticGestureSegment.exchange(\n            hapticSegment, std::memory_order_acq_rel);\n    if (hapticSegment == 1 && previousHapticSegment != 1) {\n        performNativeHaptic(previousHapticSegment == 2 ? "return-first" : "first", true);\n    }\n'''
assert text.count(old_gate) == 1
text = text.replace(old_gate, new_gate, 1)

# Do not install the old on_back_invoke replay hook. The stock Launcher owns second-stage/commit haptics.
old_install = '    installBackInvokeHapticHook(library);\n    return true;\n'
new_install = '    // No module commit hook: preserve Xiaomi\'s native second-segment/commit haptic exactly once.\n    return true;\n'
assert text.count(old_install) == 1
text = text.replace(old_install, new_install, 1)

old_back = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {\n    const bool wasReady = gReadyHapticLatched.exchange(false, std::memory_order_acq_rel);\n    if (wasReady) performNativeHaptic("commit", false);\n}\n'''
new_back = '''__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {\n    // Kept ABI-compatible with the assembly shim, but dev.8 no longer installs this hook.\n    // If a stale in-process hook ever reaches it, only reset segment state; never replay commit.\n    gHapticGestureSegment.store(0, std::memory_order_release);\n}\n'''
assert text.count(old_back) == 1
text = text.replace(old_back, new_back, 1)

old_policy = 'HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-primary provider-loader-fallback no-apk-open no-haptic-dlopen no-hook-mutex ready=ext-fallback commit=ext-0'
new_policy = 'HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-primary provider-loader-fallback first-segment-only no-module-second no-module-commit no-apk-open no-haptic-dlopen no-hook-mutex'
assert text.count(old_policy) == 1
text = text.replace(old_policy, new_policy, 1)

main.write_text(text)

gradle = Path('app/build.gradle.kts')
g = gradle.read_text()
assert g.count('versionCode = 39') == 1
assert g.count('versionName = "0.8.0-dev.7"') == 1
g = g.replace('versionCode = 39', 'versionCode = 40', 1)
g = g.replace('versionName = "0.8.0-dev.7"', 'versionName = "0.8.0-dev.8"', 1)
gradle.write_text(g)
