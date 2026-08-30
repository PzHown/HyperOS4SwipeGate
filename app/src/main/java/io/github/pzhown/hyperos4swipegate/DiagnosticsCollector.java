package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;

public final class DiagnosticsCollector {
    private DiagnosticsCollector() {}

    public static String collect(Context context) {
        XposedServiceBridge.Snapshot service = XposedServiceBridge.snapshot(context);
        RuntimeRequirementsBridge.Snapshot runtime = RuntimeRequirementsBridge.snapshot(context);
        DiagnosticsStreamBridge.NativeHookStatus nativeHook =
                DiagnosticsStreamBridge.nativeHookStatus();

        StringBuilder out = new StringBuilder();
        out.append("=== SwipeGate diagnostics ===\n");
        out.append("version=").append(BuildConfig.VERSION_NAME)
                .append(" (").append(BuildConfig.VERSION_CODE).append(")\n");
        out.append("device=").append(Build.MANUFACTURER).append(' ')
                .append(Build.MODEL).append('\n');
        out.append("android=").append(Build.VERSION.RELEASE)
                .append(" sdk=").append(Build.VERSION.SDK_INT).append('\n');
        out.append("launcher=").append(readLauncherVersion(context)).append('\n');

        out.append("\n[framework]\n");
        out.append("connected=").append(service.serviceConnected()).append('\n');
        if (service.serviceConnected()) {
            out.append("api=").append(service.apiVersion()).append('\n');
            if (!service.frameworkName().isBlank()) {
                out.append("framework=").append(service.frameworkName());
                if (!service.frameworkVersion().isBlank()) {
                    out.append(' ').append(service.frameworkVersion());
                }
                out.append('\n');
            }
        }
        out.append("launcherInScope=").append(runtime.launcherInScope()).append('\n');
        out.append("systemUiInScope=").append(runtime.systemUiInScope()).append('\n');
        out.append("requiredScopesReady=").append(runtime.requiredScopesReady()).append('\n');
        out.append("hyosRuntime=").append(runtime.hyosRuntimeDetected()).append('\n');
        if (!service.error().isBlank()) {
            out.append("error=").append(service.error()).append('\n');
        }

        out.append("\n[target]\n");
        out.append("loaded=").append(service.launcherLoaded()).append('\n');
        if (!service.targetState().isBlank()) {
            out.append("state=").append(service.targetState()).append('\n');
        }

        out.append("\n[config]\n");
        out.append("thresholdDp=").append(service.thresholdDp()).append('\n');

        out.append("\n[channel]\n");
        out.append("stage=").append(NativeControlBridge.channelStage()).append('\n');
        out.append("detail=").append(NativeControlBridge.channelDetail()).append('\n');
        out.append("pending=").append(NativeControlBridge.hasPendingQuery()).append('\n');
        out.append("stageAgeMs=").append(NativeControlBridge.channelAgeMs()).append('\n');

        out.append("\n[hook]\n");
        out.append("state=").append(nativeHook.state()).append('\n');
        out.append("fresh=").append(nativeHook.fresh()).append('\n');
        if (!nativeHook.pattern().isBlank()) {
            out.append("profile=").append(nativeHook.pattern()).append('\n');
        }
        if (!nativeHook.detail().isBlank()) {
            out.append("detail=").append(nativeHook.detail()).append('\n');
        }
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
