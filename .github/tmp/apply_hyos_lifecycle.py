from pathlib import Path

path = Path('native/src/main.cpp')
text = path.read_text()

replacements = []

replacements.append((
'''bool isTargetProcess() {\n    return readExecutable() == kSpawnerPath && readProcessName() == kTargetPackage;\n}\n''',
'''// LSPosed initializes HyperOS native modules in the root hyos_spawner before\n// the launcher child is specialized. At that stage /proc/self/cmdline is commonly\n// `usap64`, not com.miui.home. Keep executable identity as the hard injection\n// boundary and only require the launcher cmdline for child-only work such as the\n// watchdog thread.\nbool isLauncherProcess() {\n    return readProcessName() == kTargetPackage;\n}\n\nbool isHyosSpawnerProcessFamily() {\n    return readExecutable() == kSpawnerPath;\n}\n\nbool isTargetProcess() {\n    return isHyosSpawnerProcessFamily() && isLauncherProcess();\n}\n'''))

replacements.append((
'''    return effectiveDistancePx;\n}\n\nvoid onSwipeProcessHookV1(void *self, bool readyFinish, uint32_t side, const void *point, float horizontalDistancePx) {\n    ActiveHookCallGuard activeGuard;\n''',
'''    return effectiveDistancePx;\n}\n\nvoid ensureWorkerStarted();\n\nvoid onSwipeProcessHookV1(void *self, bool readyFinish, uint32_t side, const void *point, float horizontalDistancePx) {\n    ActiveHookCallGuard activeGuard;\n    // The inline hook can be inherited from the root spawner. Start the child-only\n    // watchdog lazily on the first real launcher invocation if no loader callback\n    // was delivered after specialization.\n    if (isLauncherProcess()) ensureWorkerStarted();\n'''))

replacements.append((
'''void onSwipeProcessHookV2(void *self, bool readyFinish, uint32_t side, float horizontalDistancePx, float pointX, float pointY) {\n    ActiveHookCallGuard activeGuard;\n''',
'''void onSwipeProcessHookV2(void *self, bool readyFinish, uint32_t side, float horizontalDistancePx, float pointX, float pointY) {\n    ActiveHookCallGuard activeGuard;\n    if (isLauncherProcess()) ensureWorkerStarted();\n'''))

replacements.append((
'''    const bool forceScan = std::strcmp(source, "loader-callback") == 0;\n''',
'''    const bool forceScan = std::strcmp(source, "loader-callback") == 0\n            || std::strcmp(source, "native-init-backfill") == 0;\n'''))

replacements.append((
'''void onLibraryLoaded(const char *name, void *) {\n    if (!isTargetProcess() || name == nullptr) return;\n    if (std::strstr(name, kTargetLibrary) != nullptr) {\n        const LibraryInfo library = findLauncherLibrary();\n        if (library.base != 0) {\n            ensureHook(library, "loader-callback");\n        }\n    }\n}\n''',
'''void onLibraryLoaded(const char *name, void *) {\n    if (!isHyosSpawnerProcessFamily() || name == nullptr) return;\n    if (std::strstr(name, kTargetLibrary) != nullptr) {\n        const LibraryInfo library = findLauncherLibrary();\n        if (library.base != 0) {\n            ensureHook(library, "loader-callback");\n            if (isLauncherProcess()) ensureWorkerStarted();\n        }\n    }\n}\n'''))

replacements.append((
'''extern "C" __attribute__((visibility("default"), used))\nNativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {\n    if (entries == nullptr || entries->hook_func == nullptr || !isTargetProcess()) {\n        return nullptr;\n    }\n    gHookFunction = entries->hook_func;\n    gUnhookFunction = entries->unhook_func;\n    logLine(ANDROID_LOG_INFO,\n            "DP_GATE native_init accepted api=%u exe=%s process=%s hook_func=%p unhook_func=%p watchdog=%lldms resolver=masked-pattern-scan repair=unhook+rehook",\n            entries->version, readExecutable().c_str(), readProcessName().c_str(),\n            reinterpret_cast<void *>(entries->hook_func),\n            reinterpret_cast<void *>(entries->unhook_func),\n            static_cast<long long>(kHookHealthIntervalMs));\n    ensureWorkerStarted();\n    return onLibraryLoaded;\n}\n''',
'''extern "C" __attribute__((visibility("default"), used))\nNativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {\n    const std::string executable = readExecutable();\n    const std::string processName = readProcessName();\n    const bool entriesReady = entries != nullptr;\n    const bool hookReady = entriesReady && entries->hook_func != nullptr;\n    const bool hyosProcess = executable == kSpawnerPath;\n    const bool launcherProcess = processName == kTargetPackage;\n\n    // Always emit the preflight result before any rejection. This is intentionally\n    // independent from the app-side log recording level so LSPosed exports can\n    // distinguish lifecycle rejection from Pattern/ABI failures.\n    __android_log_print(\n            ANDROID_LOG_INFO, kTag,\n            "DP_GATE native_init checks entries=%d hook=%d unhook=%d hyosExe=%d launcherCmdline=%d api=%u exe=%s process=%s",\n            entriesReady ? 1 : 0, hookReady ? 1 : 0,\n            entriesReady && entries->unhook_func != nullptr ? 1 : 0,\n            hyosProcess ? 1 : 0, launcherProcess ? 1 : 0,\n            entriesReady ? entries->version : 0, executable.c_str(), processName.c_str());\n\n    if (!entriesReady || !hookReady) {\n        logLine(ANDROID_LOG_ERROR,\n                "DP_GATE native_init rejected: LSPosed native hook backend unavailable entries=%p hook_func=%p",\n                static_cast<const void *>(entries),\n                entriesReady ? reinterpret_cast<void *>(entries->hook_func) : nullptr);\n        return nullptr;\n    }\n    if (!hyosProcess) {\n        logLine(ANDROID_LOG_WARN,\n                "DP_GATE native_init rejected non-HYOS process exe=%s process=%s",\n                executable.c_str(), processName.c_str());\n        return nullptr;\n    }\n\n    gHookFunction = entries->hook_func;\n    gUnhookFunction = entries->unhook_func;\n    logLine(ANDROID_LOG_INFO,\n            "DP_GATE native_init accepted api=%u exe=%s process=%s launcherCmdline=%d hook_func=%p unhook_func=%p watchdog=%lldms resolver=masked-pattern-scan repair=unhook+rehook",\n            entries->version, executable.c_str(), processName.c_str(), launcherProcess ? 1 : 0,\n            reinterpret_cast<void *>(entries->hook_func),\n            reinterpret_cast<void *>(entries->unhook_func),\n            static_cast<long long>(kHookHealthIntervalMs));\n\n    // HyperOS may map libapp_launcher.so before LSPosed calls native_init. Backfill\n    // the already-loaded image so the first hook can be installed in the spawner\n    // and inherited by the final launcher child instead of waiting for a callback\n    // that may never arrive.\n    const LibraryInfo library = findLauncherLibrary();\n    if (library.base != 0) {\n        logLine(ANDROID_LOG_INFO,\n                "DP_GATE native_init backfill found %s base=%p process=%s",\n                kTargetLibrary, reinterpret_cast<void *>(library.base), processName.c_str());\n        ensureHook(library, "native-init-backfill");\n    } else {\n        logLine(ANDROID_LOG_INFO,\n                "DP_GATE native_init waiting for %s process=%s",\n                kTargetLibrary, processName.c_str());\n    }\n\n    // Never start a worker thread in the root spawner: it would disappear across\n    // fork while the atomic started flag remained inherited by the child.\n    if (launcherProcess) ensureWorkerStarted();\n    return onLibraryLoaded;\n}\n'''))

for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'expected exactly one match, got {count}: {old[:100]!r}')
    text = text.replace(old, new, 1)

path.write_text(text)
print(f'patched {path} with {len(replacements)} lifecycle replacements')
