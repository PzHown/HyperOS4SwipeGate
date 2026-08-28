package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;

public final class DiagnosticsCollector {
    private DiagnosticsCollector() {}

    public static String collect(Context context) {
        StringBuilder out = new StringBuilder();
        out.append("=== HyperOS4 SwipeGate diagnostics ===\n");
        out.append("appVersion=").append(BuildConfig.VERSION_NAME)
                .append(" (").append(BuildConfig.VERSION_CODE).append(")\n");
        out.append("device=").append(Build.MANUFACTURER).append(' ')
                .append(Build.MODEL).append('\n');
        out.append("android=").append(Build.VERSION.RELEASE)
                .append(" sdk=").append(Build.VERSION.SDK_INT).append('\n');
        out.append("launcherVersion=").append(readLauncherVersion(context)).append('\n');
        out.append("targetCompatibility=Launcher 8.0+\n");
        out.append("testedLauncher=RELEASE-8.01.02.5459-260807-08242024-R\n");
        out.append("stockSidebarBoundary=88dp\n\n");

        XposedServiceBridge.Snapshot snapshot = XposedServiceBridge.snapshot(context);
        out.append("[LSPosed service]\n");
        out.append("connected=").append(snapshot.serviceConnected()).append('\n');
        if (snapshot.serviceConnected()) {
            out.append("api=").append(snapshot.apiVersion()).append('\n');
            if (!snapshot.frameworkName().isBlank()) {
                out.append("framework=").append(snapshot.frameworkName());
                if (!snapshot.frameworkVersion().isBlank()) {
                    out.append(' ').append(snapshot.frameworkVersion());
                }
                out.append('\n');
            }
        }
        if (!snapshot.error().isBlank()) {
            out.append("serviceError=").append(snapshot.error()).append('\n');
        }

        out.append("\n[target]\n");
        out.append("process=com.miui.home\n");
        out.append("moduleLoaded=").append(snapshot.launcherLoaded()).append('\n');
        if (snapshot.launcherLoaded()) {
            out.append("pid=").append(snapshot.targetPid()).append('\n');
            out.append("state=").append(snapshot.targetState()).append('\n');
        }

        out.append("\n[configuration]\n");
        out.append("thresholdDp=").append(snapshot.thresholdDp()).append('\n');
        out.append("remotePreferencePresent=").append(snapshot.remoteThresholdPresent()).append('\n');
        out.append("transport=libxposed RemotePreferences\n");
        out.append("rootRequired=false\n");

        out.append("\n[native hook]\n");
        if (snapshot.launcherLoaded()) {
            out.append("The module generation is loaded in com.miui.home.\n");
            out.append("Native Pattern/health details are recorded by LSPosed/native logs.\n");
        } else {
            out.append("The module is not currently reported as loaded in com.miui.home.\n");
        }
        out.append("This app no longer requests su only to read process/logcat diagnostics.\n");
        return out.toString();
    }

    public static String readLauncherVersion(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo("com.miui.home", 0);
            return info.versionName == null || info.versionName.isBlank()
                    ? "<unknown>"
                    : info.versionName;
        } catch (Throwable t) {
            return "<unavailable>";
        }
    }
}
