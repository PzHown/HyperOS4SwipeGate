from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


# 1) Make one versionCode source drive both APK and native code.
p = Path("app/build.gradle.kts")
text = p.read_text()
if "val swipeGateVersionCode" not in text:
    marker = 'plugins {\n    id("com.android.application")\n    id("org.jetbrains.kotlin.plugin.compose")\n}\n\n'
    if marker not in text:
        raise SystemExit("gradle plugins marker missing")
    text = text.replace(marker, marker + "val swipeGateVersionCode = 57\n\n", 1)
text = text.replace(
    "// dev.14: cross-thread one-shot Ready/Release dedup token + MADV_DONTNEED page guard.",
    "// dev.15: end-to-end loaded-version handshake for reliable restart-required detection.",
    1,
)
text = text.replace("versionCode = 56", "versionCode = swipeGateVersionCode", 1)
text = text.replace('versionName = "0.8.1-dev.14"', 'versionName = "0.8.1-dev.15"', 1)
needle = 'arguments += "-DANDROID_STL=c++_static"'
if '-DSWIPEGATE_VERSION_CODE=' not in text:
    if text.count(needle) != 1:
        raise SystemExit("gradle native arguments marker missing")
    text = text.replace(
        needle,
        needle + '\n                arguments += "-DSWIPEGATE_VERSION_CODE=$swipeGateVersionCode"',
        1,
    )
p.write_text(text)


# 2) Require and expose the same versionCode to native code.
replace_once(
    "native/CMakeLists.txt",
    "project(hyperos4swipegate LANGUAGES C CXX ASM)\n\n",
    "project(hyperos4swipegate LANGUAGES C CXX ASM)\n\n"
    "if(NOT DEFINED SWIPEGATE_VERSION_CODE)\n"
    "    message(FATAL_ERROR \"SWIPEGATE_VERSION_CODE must be supplied by Gradle\")\n"
    "endif()\n\n",
    "cmake version requirement",
)
replace_once(
    "native/CMakeLists.txt",
    "target_compile_features(hyperos4swipegate PRIVATE cxx_std_20)\n",
    "target_compile_features(hyperos4swipegate PRIVATE cxx_std_20)\n"
    "target_compile_definitions(hyperos4swipegate PRIVATE\n"
    "        SWIPEGATE_VERSION_CODE=${SWIPEGATE_VERSION_CODE}\n"
    ")\n",
    "cmake compile definition",
)


# 3) Native runtime replies with the version actually loaded in com.miui.home.
replace_once(
    "native/src/control_channel.cpp",
    'constexpr const char *kHookStateExtra = "swipegate_hook_state";\n',
    'constexpr const char *kHookStateExtra = "swipegate_hook_state";\n'
    'constexpr const char *kNativeModuleVersionExtra = "swipegate_native_module_version";\n',
    "native version extra",
)
replace_once(
    "native/src/control_channel.cpp",
    "            || !addBundleI32(extras, kHookStateExtra, static_cast<int32_t>(state))\n"
    "            || !addBundleI32(extras, kThresholdExtra, threshold)\n",
    "            || !addBundleI32(extras, kHookStateExtra, static_cast<int32_t>(state))\n"
    "            || !addBundleI32(extras, kNativeModuleVersionExtra,\n"
    "                    static_cast<int32_t>(SWIPEGATE_VERSION_CODE))\n"
    "            || !addBundleI32(extras, kThresholdExtra, threshold)\n",
    "native reply version",
)
replace_once(
    "native/src/control_channel.cpp",
    '                  "CONTROL_CARRIER accepted nonce=%lld threshold=%d logLevel=%d haptic=%d breakOpen=%d senderUidRead=%d",\n'
    "                  static_cast<long long>(nonce), thresholdDp, logLevel, hapticEnabled ? 1 : 0,\n"
    "                  breakOpenEnabled ? 1 : 0, senderUidRead ? 1 : 0);\n",
    '                  "CONTROL_CARRIER accepted nonce=%lld threshold=%d logLevel=%d haptic=%d breakOpen=%d senderUidRead=%d nativeVersion=%d",\n'
    "                  static_cast<long long>(nonce), thresholdDp, logLevel, hapticEnabled ? 1 : 0,\n"
    "                  breakOpenEnabled ? 1 : 0, senderUidRead ? 1 : 0,\n"
    "                  static_cast<int>(SWIPEGATE_VERSION_CODE));\n",
    "native accepted log version",
)


# 4) SystemUI reports its own loaded module version and relays the native one.
replace_once(
    "app/src/main/java/io/github/pzhown/hyperos4swipegate/SystemUiBridgeModule.java",
    '    static final String EXTRA_HOOK_STATE = "swipegate_hook_state";\n',
    '    static final String EXTRA_HOOK_STATE = "swipegate_hook_state";\n'
    '    static final String EXTRA_SYSTEMUI_MODULE_VERSION = "swipegate_systemui_module_version";\n'
    '    static final String EXTRA_NATIVE_MODULE_VERSION = "swipegate_native_module_version";\n',
    "systemui version extras",
)
replace_once(
    "app/src/main/java/io/github/pzhown/hyperos4swipegate/SystemUiBridgeModule.java",
    '                "SystemUI runtime bridge ready uid=" + Process.myUid());\n',
    '                "SystemUI runtime bridge ready uid=" + Process.myUid()\n'
    '                        + " loadedVersion=" + BuildConfig.VERSION_CODE);\n',
    "systemui ready log",
)
replace_once(
    "app/src/main/java/io/github/pzhown/hyperos4swipegate/SystemUiBridgeModule.java",
    "                        .putExtra(EXTRA_HOOK_STATE,\n"
    "                                intent.getIntExtra(EXTRA_HOOK_STATE, 0))\n"
    "                        .putExtra(EXTRA_THRESHOLD_DP,\n",
    "                        .putExtra(EXTRA_HOOK_STATE,\n"
    "                                intent.getIntExtra(EXTRA_HOOK_STATE, 0))\n"
    "                        .putExtra(EXTRA_SYSTEMUI_MODULE_VERSION, BuildConfig.VERSION_CODE)\n"
    "                        .putExtra(EXTRA_NATIVE_MODULE_VERSION,\n"
    "                                intent.getIntExtra(EXTRA_NATIVE_MODULE_VERSION, 0))\n"
    "                        .putExtra(EXTRA_THRESHOLD_DP,\n",
    "systemui relay versions",
)
replace_once(
    "app/src/main/java/io/github/pzhown/hyperos4swipegate/SystemUiBridgeModule.java",
    "                    .putExtra(EXTRA_HOOK_STATE, 0)\n"
    "                    .putExtra(EXTRA_CHANNEL_STAGE, stage)\n",
    "                    .putExtra(EXTRA_HOOK_STATE, 0)\n"
    "                    .putExtra(EXTRA_SYSTEMUI_MODULE_VERSION, BuildConfig.VERSION_CODE)\n"
    "                    .putExtra(EXTRA_CHANNEL_STAGE, stage)\n",
    "systemui stage version",
)


# 5) App retains both loaded versions from the authenticated SystemUI/native reply.
p = Path("app/src/main/java/io/github/pzhown/hyperos4swipegate/NativeControlBridge.java")
text = p.read_text()
old = """    public record Snapshot(\n            String state,\n            String pattern,\n            String detail,\n            long receivedAtElapsedMs\n    ) {\n        static Snapshot unknown() {\n            return new Snapshot(\"UNKNOWN\", \"\", \"等待 HyOS Runtime Hook 状态\", 0L);\n        }\n"""
new = """    public record Snapshot(\n            String state,\n            String pattern,\n            String detail,\n            long systemUiLoadedVersionCode,\n            long nativeLoadedVersionCode,\n            long receivedAtElapsedMs\n    ) {\n        static Snapshot unknown() {\n            return new Snapshot(\"UNKNOWN\", \"\", \"等待 HyOS Runtime Hook 状态\", 0L, 0L, 0L);\n        }\n"""
if text.count(old) != 1:
    raise SystemExit(f"native snapshot record: expected 1 match, got {text.count(old)}")
text = text.replace(old, new, 1)
old = """            return new Snapshot(\"CHANNEL_ERROR\", current.pattern(), detail,\n                    current.receivedAtElapsedMs());\n"""
new = """            return new Snapshot(\"CHANNEL_ERROR\", current.pattern(), detail,\n                    current.systemUiLoadedVersionCode(), current.nativeLoadedVersionCode(),\n                    current.receivedAtElapsedMs());\n"""
if text.count(old) != 1:
    raise SystemExit("channel error snapshot marker missing")
text = text.replace(old, new, 1)
old = """            String detail = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_DETAIL));\n            String log = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_NATIVE_LOG));\n\n            boolean nativeReply = \"NATIVE_REPLY_RELAYED\".equals(stage)\n"""
new = """            String detail = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_DETAIL));\n            String log = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_NATIVE_LOG));\n            long systemUiLoadedVersion = intent.getIntExtra(\n                    SystemUiBridgeModule.EXTRA_SYSTEMUI_MODULE_VERSION, 0);\n            long nativeLoadedVersion = intent.getIntExtra(\n                    SystemUiBridgeModule.EXTRA_NATIVE_MODULE_VERSION, 0);\n\n            boolean nativeReply = \"NATIVE_REPLY_RELAYED\".equals(stage)\n"""
if text.count(old) != 1:
    raise SystemExit("reply version read marker missing")
text = text.replace(old, new, 1)
old = """            latestSnapshot = new Snapshot(\n                    stateName(state), pattern, detail, SystemClock.elapsedRealtime());\n"""
new = """            latestSnapshot = new Snapshot(\n                    stateName(state), pattern, detail,\n                    systemUiLoadedVersion, nativeLoadedVersion, SystemClock.elapsedRealtime());\n"""
if text.count(old) != 1:
    raise SystemExit("reply snapshot marker missing")
text = text.replace(old, new, 1)
p.write_text(text)


# 6) Use all visible relevant targets, framework STALE semantics, loadedVersionCode,
#    and the end-to-end runtime handshake. Never synthesize UP_TO_DATE from scope alone.
p = Path("app/src/main/java/io/github/pzhown/hyperos4swipegate/XposedServiceBridge.java")
text = p.read_text()
start = text.index("            HookedTarget matchedTarget = null;\n")
end_marker = "            int targetPid = directRuntimeTarget ? matchedTarget.getPid() : 0;\n"
end = text.index(end_marker, start) + len(end_marker)
replacement = """            HookedTarget matchedTarget = null;\n            HookedTarget systemUiTarget = null;\n            String matchMode = \"none\";\n            StringBuilder targetDump = new StringBuilder();\n            long installedVersionCode = resolveInstalledVersionCode(context);\n\n            if (api >= XposedService.API_102) {\n                List<HookedTarget> targets = current.getRunningTargets();\n\n                for (HookedTarget target : targets) {\n                    if (targetDump.length() > 0) targetDump.append(';');\n                    targetDump.append(target.getProcessName())\n                            .append(\" uid=\").append(target.getUid())\n                            .append(\" pid=\").append(target.getPid())\n                            .append(\" state=\").append(target.getState().name())\n                            .append(\" loadedVersion=\").append(target.getLoadedVersionCode());\n\n                    if (SYSTEM_UI_PACKAGE.equals(target.getProcessName())) {\n                        systemUiTarget = target;\n                    }\n                    if (TARGET_PACKAGE.equals(target.getProcessName())) {\n                        matchedTarget = target;\n                        matchMode = \"process-name\";\n                    }\n                }\n\n                if (matchedTarget == null && launcherUid >= 0) {\n                    for (HookedTarget target : targets) {\n                        if (target.getUid() == launcherUid) {\n                            matchedTarget = target;\n                            matchMode = \"launcher-uid\";\n                            break;\n                        }\n                    }\n                }\n\n                if (matchedTarget == null) {\n                    for (HookedTarget target : targets) {\n                        String processName = target.getProcessName();\n                        if (processName != null && processName.contains(\"hyos_spawner\")) {\n                            matchedTarget = target;\n                            matchMode = \"hyos-process\";\n                            break;\n                        }\n                    }\n                }\n            }\n\n            boolean directRuntimeTarget = matchedTarget != null;\n\n            // Some HYOS-enabled LSPosed builds do not expose the native-only launcher child as a\n            // normal running target. Scope presence is activation evidence only; it must never be\n            // promoted to UP_TO_DATE because that masks STALE SystemUI/native code after an APK update.\n            boolean hyosActivationFallback = !directRuntimeTarget\n                    && api >= XposedService.API_102\n                    && launcherInScope\n                    && systemUiInScope\n                    && hyosSpawnerPresent;\n\n            boolean launcherLoaded = directRuntimeTarget || systemUiTarget != null || hyosActivationFallback;\n            String frameworkTargetState = aggregateTargetState(\n                    matchedTarget, systemUiTarget, installedVersionCode);\n            int targetPid = directRuntimeTarget ? matchedTarget.getPid() : 0;\n"""
text = text[:start] + replacement + text[end:]

old = """            DiagnosticsStreamBridge.NativeHookStatus nativeHookStatus =\n                    DiagnosticsStreamBridge.nativeHookStatus();\n            String nativeState = nativeHookStatus.state();\n            String targetState = frameworkTargetState;\n            if (launcherLoaded) {\n                // Failure wins over freshness. A stale FAILED state must stay failed until the\n                // native side reports a newer state; otherwise the UI lies by falling back to\n                // \"正在更新\" after a known Hook failure.\n                if (\"FAILED\".equals(frameworkTargetState) || \"FAILED\".equals(nativeState)) {\n                    targetState = \"FAILED\";\n                } else if (\"RELOADING\".equals(frameworkTargetState)) {\n                    targetState = \"RELOADING\";\n                } else if (!nativeHookStatus.fresh()) {\n                    // Keep the existing short loading window while the app waits for the first\n                    // Native reply. NativeControlBridge turns a prolonged no-reply condition into\n                    // FAILED after its fail-closed timeout, so this can no longer loop forever.\n                    targetState = \"RELOADING\";\n                } else {\n                    targetState = switch (nativeState) {\n                        case \"HEALTHY\" -> frameworkTargetState;\n                        case \"FAILED\" -> \"FAILED\";\n                        case \"REPAIRING\", \"WAITING\", \"UNKNOWN\" -> \"RELOADING\";\n                        default -> \"RELOADING\";\n                    };\n                }\n            }\n"""
new = """            DiagnosticsStreamBridge.NativeHookStatus nativeHookStatus =\n                    DiagnosticsStreamBridge.nativeHookStatus();\n            NativeControlBridge.Snapshot nativePeer = NativeControlBridge.snapshot();\n            String nativeState = nativeHookStatus.state();\n            boolean runtimeVersionStale = nativePeer.fresh()\n                    && (nativePeer.systemUiLoadedVersionCode() <= 0L\n                    || nativePeer.nativeLoadedVersionCode() <= 0L\n                    || nativePeer.systemUiLoadedVersionCode() != installedVersionCode\n                    || nativePeer.nativeLoadedVersionCode() != installedVersionCode);\n            String targetState = frameworkTargetState;\n            if (launcherLoaded) {\n                // Framework state is authoritative when available. The explicit version handshake\n                // covers HYOS builds that hide the launcher child from getRunningTargets().\n                if (\"FAILED\".equals(frameworkTargetState) || \"FAILED\".equals(nativeState)) {\n                    targetState = \"FAILED\";\n                } else if (\"STALE\".equals(frameworkTargetState) || runtimeVersionStale) {\n                    targetState = \"STALE\";\n                } else if (\"RELOADING\".equals(frameworkTargetState)) {\n                    targetState = \"RELOADING\";\n                } else if (!nativeHookStatus.fresh()) {\n                    targetState = \"RELOADING\";\n                } else {\n                    targetState = switch (nativeState) {\n                        case \"HEALTHY\" -> frameworkTargetState.isBlank()\n                                ? \"UP_TO_DATE\" : frameworkTargetState;\n                        case \"FAILED\" -> \"FAILED\";\n                        case \"REPAIRING\", \"WAITING\", \"UNKNOWN\" -> \"RELOADING\";\n                        default -> \"RELOADING\";\n                    };\n                }\n            }\n"""
if text.count(old) != 1:
    raise SystemExit(f"target state block: expected 1 match, got {text.count(old)}")
text = text.replace(old, new, 1)

old = """                    + \" nativeHookDetail=\" + nativeHookStatus.detail()\n                    + \" targets=[\" + targetDump + \"]\";\n"""
new = """                    + \" nativeHookDetail=\" + nativeHookStatus.detail()\n                    + \" installedVersion=\" + installedVersionCode\n                    + \" systemUiLoadedVersion=\" + nativePeer.systemUiLoadedVersionCode()\n                    + \" nativeLoadedVersion=\" + nativePeer.nativeLoadedVersionCode()\n                    + \" runtimeVersionStale=\" + runtimeVersionStale\n                    + \" targets=[\" + targetDump + \"]\";\n"""
if text.count(old) != 1:
    raise SystemExit("runtime evidence marker missing")
text = text.replace(old, new, 1)

insert_before = """    private static int resolveLauncherUid(Context context) {\n"""
helpers = """    private static long resolveInstalledVersionCode(Context context) {\n        try {\n            return context.getPackageManager()\n                    .getPackageInfo(context.getPackageName(), 0)\n                    .getLongVersionCode();\n        } catch (Throwable ignored) {\n            return BuildConfig.VERSION_CODE;\n        }\n    }\n\n    private static String aggregateTargetState(\n            HookedTarget launcherTarget, HookedTarget systemUiTarget, long installedVersionCode) {\n        boolean stale = false;\n        boolean reloading = false;\n        for (HookedTarget target : new HookedTarget[]{launcherTarget, systemUiTarget}) {\n            if (target == null) continue;\n            if (target.getState() == HookedTarget.State.FAILED) return \"FAILED\";\n            if (target.getState() == HookedTarget.State.STALE) stale = true;\n            if (target.getState() == HookedTarget.State.RELOADING) reloading = true;\n            long loadedVersionCode = target.getLoadedVersionCode();\n            if (loadedVersionCode > 0L && installedVersionCode > 0L\n                    && loadedVersionCode != installedVersionCode) {\n                stale = true;\n            }\n        }\n        if (stale) return \"STALE\";\n        if (reloading) return \"RELOADING\";\n        if (launcherTarget != null || systemUiTarget != null) return \"UP_TO_DATE\";\n        return \"\";\n    }\n\n"""
if text.count(insert_before) != 1:
    raise SystemExit("helper insertion marker missing")
text = text.replace(insert_before, helpers + insert_before, 1)
p.write_text(text)


# Remove temporary/stale patch workflows and this patch machinery from the final commit.
for path in [
    ".github/workflows/apply-cross-thread-release-dedup.yml",
    ".github/workflows/apply-madvise-page-guard.yml",
    ".github/workflows/run-cross-thread-release-dedup.yml",
    ".github/workflows/run-loaded-version-handshake.yml",
    "scripts/apply_cross_thread_release_dedup.py",
    "scripts/apply_loaded_version_handshake.py",
]:
    candidate = Path(path)
    if candidate.exists():
        candidate.unlink()
