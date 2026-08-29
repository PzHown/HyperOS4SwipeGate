package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ConfigBridge {
    public static final String PREF_KEY_THRESHOLD_DP = "trigger_threshold_dp";
    public static final String LEGACY_PREF_KEY_THRESHOLD_PX = "trigger_threshold_px";
    public static final String LEGACY_PREF_KEY_EXTRA_DP = "trigger_extra_dp";
    public static final String PREF_KEY_LOG_LEVEL = "native_log_level";

    public static final int DEFAULT_THRESHOLD_DP = 0; // Legacy alias: 0 = Xiaomi stock/default (88dp).
    public static final int STOCK_THRESHOLD_DP = 88;
    public static final int MAX_THRESHOLD_DP = 300;

    public static final int LOG_LEVEL_OFF = 0;
    public static final int LOG_LEVEL_COMPACT = 1;
    public static final int LOG_LEVEL_DETAILED = 2;
    public static final int DEFAULT_LOG_LEVEL = LOG_LEVEL_OFF;
    // Native logging levels are user-selectable. Hook-health transport remains available when off.
    public static final boolean LOG_RECORDING_OPTIONS_ENABLED = true;

    public static final String REMOTE_PREF_GROUP = "swipegate";
    public static final String REMOTE_PREF_KEY_THRESHOLD_DP = "threshold_dp";
    public static final String REMOTE_PREF_KEY_LOG_LEVEL = "log_level";
    public static final String REMOTE_PREF_KEY_DIAGNOSTICS_PORT = "diagnostics_port";
    public static final String REMOTE_PREF_KEY_DIAGNOSTICS_TOKEN = "diagnostics_token";
    public static final String NATIVE_CONFIG_FILE = "hyperos4swipegate_config";
    public static final String NATIVE_LOG_LEVEL_FILE = "hyperos4swipegate_log_level";

    // Legacy migration fallback only. New builds use libxposed RemotePreferences.
    public static final String SYSTEM_PROPERTY = "persist.hyperos4swipegate.threshold_dp";
    public static final String LEGACY_SYSTEM_PROPERTY_PX = "persist.hyperos4swipegate.threshold_px";
    public static final String LEGACY_SYSTEM_PROPERTY_EXTRA_DP = "persist.hyperos4swipegate.extra_dp";

    private static final ExecutorService EXECUTOR = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private ConfigBridge() {}

    public interface Callback {
        void onResult(Result result);
    }

    public record Result(boolean success, int value, String message) {}

    public static SharedPreferences localPreferences(Context context) {
        Context app = context.getApplicationContext();
        return app.getSharedPreferences(app.getPackageName() + "_preferences", Context.MODE_PRIVATE);
    }

    public static int sanitizeLogLevel(int logLevel) {
        if (!LOG_RECORDING_OPTIONS_ENABLED) return LOG_LEVEL_OFF;
        return Math.max(LOG_LEVEL_OFF, Math.min(LOG_LEVEL_DETAILED, logLevel));
    }

    public static void applyThresholdDpAsync(Context context, int thresholdDp, Callback callback) {
        Context app = context.getApplicationContext();
        int safeValue = Math.max(STOCK_THRESHOLD_DP, Math.min(MAX_THRESHOLD_DP, thresholdDp));
        EXECUTOR.execute(() -> {
            Result result = XposedServiceBridge.applyThresholdDp(app, safeValue);
            MAIN.post(() -> callback.onResult(result));
        });
    }

    public static void applyLogLevelAsync(Context context, int logLevel, Callback callback) {
        Context app = context.getApplicationContext();
        int safeValue = sanitizeLogLevel(logLevel);
        EXECUTOR.execute(() -> {
            Result result = XposedServiceBridge.applyLogLevel(app, safeValue);
            MAIN.post(() -> callback.onResult(result));
        });
    }
}
