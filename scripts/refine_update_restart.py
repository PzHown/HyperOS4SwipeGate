from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    return text.replace(old, new, 1)


home = Path("app/src/main/java/io/github/pzhown/hyperos4swipegate/HomeScreen.kt")
text = home.read_text()
text = replace_once(
    text,
    'private const val PREF_LAST_APP_VERSION_CODE = "last_app_version_code"',
    'private const val PREF_LAST_SEEN_UPDATE_TIME = "last_seen_app_update_time"',
    "pref key",
)
old = '''private fun shouldShowUpdateRestartNotice(context: Context): Boolean {
    val prefs = ConfigBridge.localPreferences(context)
    val currentVersionCode = BuildConfig.VERSION_CODE
    val previousVersionCode = prefs.getInt(PREF_LAST_APP_VERSION_CODE, 0)
    if (previousVersionCode == 0) {
        prefs.edit().putInt(PREF_LAST_APP_VERSION_CODE, currentVersionCode).apply()
        return false
    }
    if (previousVersionCode == currentVersionCode) return false

    prefs.edit().putInt(PREF_LAST_APP_VERSION_CODE, currentVersionCode).apply()
    return true
}
'''
new = '''private fun shouldShowUpdateRestartNotice(context: Context): Boolean {
    return try {
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        val firstInstallTime = info.firstInstallTime
        val lastUpdateTime = info.lastUpdateTime
        val prefs = ConfigBridge.localPreferences(context)
        val lastSeenUpdateTime = prefs.getLong(PREF_LAST_SEEN_UPDATE_TIME, 0L)

        // Fresh install: firstInstallTime == lastUpdateTime, so do not nag.
        // Upgrade install: lastUpdateTime advances while firstInstallTime stays unchanged.
        // Persist the update timestamp so each APK update prompts only once.
        val unseenUpgrade = lastUpdateTime > firstInstallTime && lastSeenUpdateTime != lastUpdateTime
        if (lastSeenUpdateTime != lastUpdateTime) {
            prefs.edit().putLong(PREF_LAST_SEEN_UPDATE_TIME, lastUpdateTime).apply()
        }
        unseenUpgrade
    } catch (_: Throwable) {
        false
    }
}
'''
text = replace_once(text, old, new, "update detector")
home.write_text(text)

gradle = Path("app/build.gradle.kts")
g = gradle.read_text()
g = replace_once(
    g,
    "        // dev.6: update-restart notice + 400 ms Ready/Release dedup.\n        versionCode = 48\n        versionName = \"0.8.1-dev.6\"",
    "        // dev.7: robust upgrade detection + 400 ms Ready/Release dedup.\n        versionCode = 49\n        versionName = \"0.8.1-dev.7\"",
    "version",
)
gradle.write_text(g)

Path("scripts/refine_update_restart.py").unlink()
Path(".github/workflows/refine-update-restart-detection.yml").unlink()
