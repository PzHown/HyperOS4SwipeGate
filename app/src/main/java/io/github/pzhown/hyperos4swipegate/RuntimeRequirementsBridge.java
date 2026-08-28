package io.github.pzhown.hyperos4swipegate;

import android.content.Context;

import java.io.File;
import java.util.List;

import io.github.libxposed.service.HookedTarget;
import io.github.libxposed.service.XposedService;

/**
 * Rootless runtime requirement detection for the HyperOS 4 native launcher path.
 *
 * XposedServiceHelper only supports one listener per app process. This bridge therefore
 * reuses the single service connection owned by XposedServiceBridge instead of registering
 * a second listener.
 *
 * Zygisk Next keeps its exact installed version below /data/adb, which a normal app cannot
 * read without root. Prefer direct HYOS target evidence from LSPosed API 102; when a HYOS-only
 * child is not exposed through getRunningTargets(), fall back to the complete capability chain:
 * supported LSPosed + Launcher scope + Xiaomi hyos_spawner. This avoids reporting a false
 * negative on working HYOS Runtime installations while keeping the App rootless.
 */
public final class RuntimeRequirementsBridge {
    public static final long MIN_LSPOSED_VERSION_CODE = 7846L;
    public static final String MIN_ZYGISK_NEXT_VERSION = "1.5.0";

    private static final String TARGET_PACKAGE = "com.miui.home";
    private static final String HYOS_SPAWNER = "/system_ext/bin/hyos_spawner";

    private RuntimeRequirementsBridge() {}

    public record Snapshot(
            boolean serviceConnected,
            int apiVersion,
            String frameworkName,
            String frameworkVersion,
            long frameworkVersionCode,
            boolean lsposedSupported,
            boolean hyosSpawnerPresent,
            boolean hyosRuntimeDetected,
            boolean zygiskNextSupported,
            boolean launcherInScope,
            String evidence,
            String error
    ) {}

    public static Snapshot snapshot(Context context) {
        XposedService current = XposedServiceBridge.currentService(context);
        if (current == null) {
            return new Snapshot(false, 0, "", "", 0L,
                    false, new File(HYOS_SPAWNER).exists(), false, false,
                    false, "serviceConnected=false", XposedServiceBridge.currentServiceError());
        }

        try {
            final int api = current.getApiVersion();
            final String frameworkName = current.getFrameworkName();
            final String frameworkVersion = current.getFrameworkVersion();
            final long frameworkVersionCode = current.getFrameworkVersionCode();
            final boolean lsposedSupported = frameworkVersionCode >= MIN_LSPOSED_VERSION_CODE;
            final boolean hyosSpawnerPresent = new File(HYOS_SPAWNER).exists();
            final boolean launcherInScope = current.getScope().contains(TARGET_PACKAGE);
            final int launcherUid = resolveLauncherUid(context);

            boolean directHyosRuntimeDetected = false;
            StringBuilder targetDump = new StringBuilder();

            if (api >= XposedService.API_102) {
                List<HookedTarget> targets = current.getRunningTargets();
                for (HookedTarget target : targets) {
                    if (targetDump.length() > 0) targetDump.append(';');
                    String processName = target.getProcessName();
                    targetDump.append(processName)
                            .append(" uid=").append(target.getUid())
                            .append(" pid=").append(target.getPid())
                            .append(" state=").append(target.getState().name());

                    if (processName != null && processName.contains("hyos_spawner")) {
                        directHyosRuntimeDetected = true;
                    } else if (hyosSpawnerPresent && launcherUid >= 0 && target.getUid() == launcherUid) {
                        directHyosRuntimeDetected = true;
                    }
                }
            }

            // HYOS native children are not guaranteed to appear in getRunningTargets().
            // If the framework is new enough, Launcher is in scope and the device exposes
            // Xiaomi's HYOS spawner, treat the runtime capability as available instead of
            // producing a false negative solely because the native child was omitted.
            final boolean capabilityFallback = !directHyosRuntimeDetected
                    && api >= XposedService.API_102
                    && lsposedSupported
                    && launcherInScope
                    && hyosSpawnerPresent;
            final boolean hyosRuntimeDetected = directHyosRuntimeDetected || capabilityFallback;
            final boolean zygiskNextSupported = hyosRuntimeDetected;
            final String hyosEvidenceMode = directHyosRuntimeDetected
                    ? "running-target"
                    : (capabilityFallback ? "capability-fallback" : "none");

            final String evidence = "frameworkVersionCode=" + frameworkVersionCode
                    + " api=" + api
                    + " launcherInScope=" + launcherInScope
                    + " hyosSpawnerPresent=" + hyosSpawnerPresent
                    + " directHyosRuntimeDetected=" + directHyosRuntimeDetected
                    + " hyosRuntimeDetected=" + hyosRuntimeDetected
                    + " hyosEvidenceMode=" + hyosEvidenceMode
                    + " targets=[" + targetDump + "]";

            return new Snapshot(true, api, frameworkName, frameworkVersion, frameworkVersionCode,
                    lsposedSupported, hyosSpawnerPresent, hyosRuntimeDetected,
                    zygiskNextSupported, launcherInScope, evidence, "");
        } catch (Throwable t) {
            String message = t.getMessage();
            if (message == null || message.isBlank()) message = t.getClass().getSimpleName();
            return new Snapshot(true, 0, "", "", 0L,
                    false, new File(HYOS_SPAWNER).exists(), false, false,
                    false, "status check failed", message);
        }
    }

    private static int resolveLauncherUid(Context context) {
        try {
            return context.getPackageManager().getApplicationInfo(TARGET_PACKAGE, 0).uid;
        } catch (Throwable ignored) {
            return -1;
        }
    }
}
