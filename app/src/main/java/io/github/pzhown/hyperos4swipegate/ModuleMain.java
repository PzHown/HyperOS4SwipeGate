package io.github.pzhown.hyperos4swipegate;

import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;

import androidx.annotation.NonNull;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

/**
 * Modern libxposed Java entry used only as a configuration bridge.
 *
 * The actual gesture hook remains in native_init. The module app writes
 * libxposed RemotePreferences without root; this entry receives those changes
 * inside com.miui.home and mirrors the current threshold into launcher-owned
 * cache files that the native hook can read directly.
 */
public final class ModuleMain extends XposedModule {
    private static final String TARGET_PACKAGE = "com.miui.home";

    private SharedPreferences remotePreferences;
    private SharedPreferences.OnSharedPreferenceChangeListener preferenceListener;
    private final List<File> configFiles = new ArrayList<>();

    @Override
    public void onPackageLoaded(@NonNull XposedModuleInterface.PackageLoadedParam param) {
        if (!TARGET_PACKAGE.equals(param.getPackageName()) || !param.isFirstPackage()) return;

        ApplicationInfo appInfo = param.getApplicationInfo();
        addConfigFile(appInfo.deviceProtectedDataDir);
        addConfigFile(appInfo.dataDir);

        try {
            remotePreferences = getRemotePreferences(ConfigBridge.REMOTE_PREF_GROUP);
            preferenceListener = (preferences, key) -> {
                if (ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP.equals(key)) {
                    publishThreshold(preferences);
                }
            };
            remotePreferences.registerOnSharedPreferenceChangeListener(preferenceListener);
            if (remotePreferences.contains(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP)) {
                publishThreshold(remotePreferences);
            }
            log(android.util.Log.INFO, "HyperOS4SwipeGateJava",
                    "RemotePreferences bridge ready configFiles=" + configFiles.size());
        } catch (Throwable t) {
            log(android.util.Log.ERROR, "HyperOS4SwipeGateJava",
                    "RemotePreferences bridge failed", t);
        }
    }

    private void addConfigFile(String dataDir) {
        if (dataDir == null || dataDir.isBlank()) return;
        File file = new File(new File(dataDir, "cache"), ConfigBridge.NATIVE_CONFIG_FILE);
        for (File existing : configFiles) {
            if (existing.equals(file)) return;
        }
        configFiles.add(file);
    }

    private void publishThreshold(SharedPreferences preferences) {
        int thresholdDp = preferences.getInt(
                ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP,
                ConfigBridge.DEFAULT_THRESHOLD_DP);
        thresholdDp = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, thresholdDp));
        byte[] bytes = (Integer.toString(thresholdDp) + "\n").getBytes(StandardCharsets.US_ASCII);

        int successes = 0;
        for (File file : configFiles) {
            File parent = file.getParentFile();
            if (parent == null || (!parent.exists() && !parent.mkdirs())) continue;
            File temporary = new File(parent, file.getName() + ".tmp");
            try (FileOutputStream output = new FileOutputStream(temporary, false)) {
                output.write(bytes);
                output.flush();
                try {
                    output.getFD().sync();
                } catch (Throwable ignored) {
                }
            } catch (Throwable ignored) {
                continue;
            }

            if (!temporary.renameTo(file)) {
                try (FileOutputStream output = new FileOutputStream(file, false)) {
                    output.write(bytes);
                    output.flush();
                } catch (Throwable ignored) {
                    temporary.delete();
                    continue;
                }
                temporary.delete();
            }
            successes++;
        }

        log(android.util.Log.INFO, "HyperOS4SwipeGateJava",
                "Published threshold=" + thresholdDp + "dp files=" + successes);
    }
}
