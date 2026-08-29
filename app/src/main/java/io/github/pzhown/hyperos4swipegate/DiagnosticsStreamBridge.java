package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.os.SystemClock;

/**
 * Compatibility facade for diagnostics consumers.
 *
 * HyperOS Runtime does not execute the Launcher-side Java module entry reliably, so the previous
 * random-port ModuleMain -> App relay could never become healthy. NativeControlBridge now owns the
 * actual transport and this class keeps the existing status/log API used by the UI and diagnostics.
 */
public final class DiagnosticsStreamBridge {
    static final String HOOK_STATUS_PREFIX = "SWIPEGATE_STATUS_V1";

    private static final long HOOK_STATUS_STALE_AFTER_MS = 5_000L;

    private DiagnosticsStreamBridge() {}

    public record NativeHookStatus(
            String state,
            String pattern,
            String detail,
            long receivedAtElapsedMs
    ) {
        static NativeHookStatus unknown() {
            return new NativeHookStatus("UNKNOWN", "", "等待 Native Hook 状态", 0L);
        }

        public boolean fresh() {
            if (receivedAtElapsedMs <= 0L) return false;
            long age = SystemClock.elapsedRealtime() - receivedAtElapsedMs;
            return age >= 0L && age <= HOOK_STATUS_STALE_AFTER_MS;
        }

        public boolean healthy() {
            return fresh() && "HEALTHY".equals(state);
        }
    }

    public static void initialize(Context context) {
        NativeControlBridge.initialize(context);
    }

    // Retained for ModuleMain source compatibility. The direct Native channel no longer publishes
    // a random App endpoint through RemotePreferences.
    public static int port() {
        return 0;
    }

    public static String token() {
        return "";
    }

    public static void clearLog() {
        NativeControlBridge.clearLog();
    }

    public static NativeHookStatus nativeHookStatus() {
        NativeControlBridge.Snapshot snapshot = NativeControlBridge.snapshot();
        if (snapshot == null) return NativeHookStatus.unknown();
        return new NativeHookStatus(
                snapshot.state(),
                snapshot.pattern(),
                snapshot.detail(),
                snapshot.receivedAtElapsedMs());
    }

    public static String currentLog() {
        return NativeControlBridge.currentLog();
    }
}
