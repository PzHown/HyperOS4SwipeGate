from pathlib import Path


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    path.write_text(text.replace(old, new, 1))


# Native log output: Off = no normal module runtime logs; Compact = state/config/errors;
# Detailed = adds recurring transport, gesture, haptic and health traces.
path = Path("native/src/log_filter.cpp")
replace_once(
    path,
    '''bool isDetailedOnlyLine(const char *message) {
    if (message == nullptr) return false;
    return std::strncmp(message, "DP_GATE rawDx=", 14) == 0
            || std::strncmp(message, "HOOK_HEALTH healthy ", 20) == 0;
}

bool shouldPersistForApp(const char *message) {
    const int level = readNativeLogLevel();
    if (level <= kLogLevelOff) return false;
    if (level == kLogLevelCompact && isDetailedOnlyLine(message)) return false;
    return true;
}
''',
    '''bool startsWith(const char *message, const char *prefix) {
    if (message == nullptr || prefix == nullptr) return false;
    return std::strncmp(message, prefix, std::strlen(prefix)) == 0;
}

bool isDetailedOnlyLine(const char *message) {
    if (message == nullptr) return false;
    return startsWith(message, "DP_GATE rawDx=")
            || startsWith(message, "HOOK_HEALTH healthy ")
            || startsWith(message, "CONTROL_CARRIER accepted ")
            || startsWith(message, "CONTROL_CARRIER duplicate ")
            || startsWith(message, "CONTROL_CARRIER sender_uid ")
            || startsWith(message, "CONTROL_CARRIER haptic field missing")
            || startsWith(message, "NATIVE_REPLY waiting ")
            || startsWith(message, "Runtime carrier accepted but native reply is not ready")
            || startsWith(message, "HAPTIC_V2 feedback ");
}

bool shouldEmitAtLevel(int level, const char *message) {
    if (level <= kLogLevelOff) return false;
    if (level == kLogLevelCompact && isDetailedOnlyLine(message)) return false;
    return true;
}

bool shouldPersistForApp(const char *message) {
    return shouldEmitAtLevel(readNativeLogLevel(), message);
}
''',
    "log_filter classifier",
)
replace_once(
    path,
    '''    // Error reporting is intentionally independent from the SwipeGate App log setting. Keep the
    // normal Android log emission and additionally retain ERROR+ lines for LSPosed-side diagnosis.
    if (priority >= ANDROID_LOG_ERROR) appendLsposedError(text);
    return __real___android_log_write(priority, tag, text);
''',
    '''    // ERROR+ retention remains available for diagnostics, while normal Android/LSPosed output
    // strictly follows Off / Compact / Detailed.
    if (priority >= ANDROID_LOG_ERROR) appendLsposedError(text);
    if (!shouldEmitAtLevel(readNativeLogLevel(), text)) return 0;
    return __real___android_log_write(priority, tag, text);
''',
    "android log wrapper",
)

# App-visible native log payload must use the same compact/detailed split.
path = Path("native/src/control_channel.cpp")
replace_once(
    path,
    '''bool isDetailedOnlyLine(const char *text) {
    if (text == nullptr) return false;
    return std::strncmp(text, "DP_GATE rawDx=", 14) == 0
            || std::strncmp(text, "HOOK_HEALTH healthy ", 20) == 0;
}
''',
    '''bool startsWith(const char *text, const char *prefix) {
    if (text == nullptr || prefix == nullptr) return false;
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool isDetailedOnlyLine(const char *text) {
    if (text == nullptr) return false;
    return startsWith(text, "DP_GATE rawDx=")
            || startsWith(text, "HOOK_HEALTH healthy ")
            || startsWith(text, "CONTROL_CARRIER accepted ")
            || startsWith(text, "CONTROL_CARRIER duplicate ")
            || startsWith(text, "CONTROL_CARRIER sender_uid ")
            || startsWith(text, "CONTROL_CARRIER haptic field missing")
            || startsWith(text, "NATIVE_REPLY waiting ")
            || startsWith(text, "Runtime carrier accepted but native reply is not ready")
            || startsWith(text, "HAPTIC_V2 feedback ");
}
''',
    "control channel classifier",
)

# Ensure native_init's direct Android print also goes through the level-aware wrapped log path.
path = Path("native/src/main.cpp")
replace_once(
    path,
    '''    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "DP_GATE native_init checks entries=%d hook=%d unhook=%d hyosExe=%d launcherCmdline=%d api=%u exe=%s process=%s",
''',
    '''    logLine(
            ANDROID_LOG_INFO,
            "DP_GATE native_init checks entries=%d hook=%d unhook=%d hyosExe=%d launcherCmdline=%d api=%u exe=%s process=%s",
''',
    "native_init log routing",
)

# UI copy. Existing explicit user preference is preserved; only never-configured installs use the
# already-existing DEFAULT_HAPTIC_ENABLED=false default.
replace_once(
    Path("app/src/main/java/io/github/pzhown/hyperos4swipegate/HomeScreen.kt"),
    'title = "丰富侧滑震动反馈",',
    'title = "丰富侧滑震动反馈 · Beta",',
    "haptic beta label",
)
replace_once(
    Path("app/src/main/java/io/github/pzhown/hyperos4swipegate/SettingsScreen.kt"),
    'summary = "额外记录侧滑过程与周期健康状态",',
    'summary = "额外记录轮询、触感反馈、侧滑过程与周期健康状态",',
    "detailed log summary",
)

# New dev build identity.
path = Path("app/build.gradle.kts")
replace_once(path, "versionCode = 40", "versionCode = 41", "versionCode")
replace_once(path, 'versionName = "0.8.0-dev.8"', 'versionName = "0.8.0-dev.9"', "versionName")
