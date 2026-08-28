package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.ParcelFileDescriptor;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import io.github.libxposed.service.HookedTarget;
import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.XposedServiceHelper;

public final class XposedServiceBridge {
    private static final String TARGET_PACKAGE = "com.miui.home";
    private static final String HYOS_SPAWNER = "/system_ext/bin/hyos_spawner";
    private static final int MAX_NATIVE_LOG_READ_BYTES = 128 * 1024;

    private static final AtomicBoolean INITIALIZED = new AtomicBoolean(false);
    private static volatile XposedService service;
    private static volatile Context appContext;
    private static volatile String serviceError = "";
    private static volatile String runtimeEvidence = "not checked";

    private XposedServiceBridge() {}

    public record Snapshot(
            boolean serviceConnected,
            int apiVersion,
            String frameworkName,
            String frameworkVersion,
            boolean launcherLoaded,
            String targetState,
            int targetPid,
            int thresholdDp,
            boolean remoteThresholdPresent,
            String error
    ) {}

    public static void initialize(Context context) {
        appContext = context.getApplicationContext();
        if (!INITIALIZED.compareAndSet(false, true)) return;
        XposedServiceHelper.registerListener(new XposedServiceHelper.OnServiceListener() {
            @Override
            public void onServiceBind(XposedService boundService) {
                service = boundService;
                serviceError = "";
                migrateLocalThresholdIfNeeded(boundService);
            }

            @Override
            public void onServiceDied(XposedService deadService) {
                if (service == deadService) {
                    service = null;
                    serviceError = "LSPosed 服务已断开";
                    runtimeEvidence = "service disconnected";
                }
            }
        });
    }

    private static void migrateLocalThresholdIfNeeded(XposedService current) {
        Context context = appContext;
        if (context == null) return;
        try {
            if (current.getApiVersion() < XposedService.API_102) return;
            if ((current.getFrameworkProperties() & XposedService.PROP_CAP_REMOTE) == 0) return;
            SharedPreferences remote = current.getRemotePreferences(ConfigBridge.REMOTE_PREF_GROUP);
            if (remote.contains(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP)) return;
            int localValue = ConfigBridge.localPreferences(context)
                    .getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP);
            localValue = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, localValue));
            remote.edit()
                    .putInt(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP, localValue)
                    .commit();
        } catch (Throwable t) {
            String message = t.getMessage();
            serviceError = message == null || message.isBlank()
                    ? t.getClass().getSimpleName()
                    : message;
        }
    }

    public static ConfigBridge.Result applyThresholdDp(Context context, int thresholdDp) {
        initialize(context);
        int safeValue = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, thresholdDp));
        ConfigBridge.localPreferences(context)
                .edit()
                .putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, safeValue)
                .apply();

        XposedService current = service;
        if (current == null) {
            return new ConfigBridge.Result(false, safeValue, "LSPosed 服务未连接");
        }

        try {
            if (current.getApiVersion() < XposedService.API_102) {
                return new ConfigBridge.Result(false, safeValue, "需要 LSPosed API 102");
            }
            if ((current.getFrameworkProperties() & XposedService.PROP_CAP_REMOTE) == 0) {
                return new ConfigBridge.Result(false, safeValue, "当前框架不支持 RemotePreferences");
            }
            SharedPreferences remote = current.getRemotePreferences(ConfigBridge.REMOTE_PREF_GROUP);
            boolean committed = remote.edit()
                    .putInt(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP, safeValue)
                    .commit();
            if (!committed) {
                return new ConfigBridge.Result(false, safeValue, "远程配置写入失败");
            }
            serviceError = "";
            return new ConfigBridge.Result(true, safeValue, "ok");
        } catch (Throwable t) {
            String message = t.getMessage();
            if (message == null || message.isBlank()) message = t.getClass().getSimpleName();
            serviceError = message;
            return new ConfigBridge.Result(false, safeValue, message);
        }
    }

    public static String readNativeRuntimeLog(Context context) {
        initialize(context);
        XposedService current = service;
        if (current == null) {
            return "LSPosed 服务未连接，暂时无法读取 Native 日志。";
        }

        try {
            if ((current.getFrameworkProperties() & XposedService.PROP_CAP_REMOTE) == 0) {
                return "当前 LSPosed 不支持 Remote Files，无法读取 Native 日志。";
            }
            try (ParcelFileDescriptor descriptor =
                         current.openRemoteFile(ConfigBridge.REMOTE_NATIVE_LOG_FILE);
                 ParcelFileDescriptor.AutoCloseInputStream input =
                         new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[4096];
                int total = 0;
                while (total < MAX_NATIVE_LOG_READ_BYTES) {
                    int allowed = Math.min(buffer.length, MAX_NATIVE_LOG_READ_BYTES - total);
                    int read = input.read(buffer, 0, allowed);
                    if (read <= 0) break;
                    output.write(buffer, 0, read);
                    total += read;
                }
                String text = new String(output.toByteArray(), StandardCharsets.UTF_8).trim();
                if (text.isBlank()) {
                    return "等待 Native 日志镜像…\n请返回系统桌面执行一次侧滑后点刷新。";
                }
                return text;
            }
        } catch (Throwable t) {
            String message = t.getMessage();
            if (message == null || message.isBlank()) message = t.getClass().getSimpleName();
            return "Native 日志读取失败：" + message;
        }
    }

    public static Snapshot snapshot(Context context) {
        initialize(context);
        int localThreshold = ConfigBridge.localPreferences(context)
                .getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP);
        localThreshold = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, localThreshold));

        XposedService current = service;
        if (current == null) {
            runtimeEvidence = "serviceConnected=false";
            return new Snapshot(false, 0, "", "", false, "", 0,
                    localThreshold, false, serviceError);
        }

        try {
            int api = current.getApiVersion();
            String frameworkName = current.getFrameworkName();
            String frameworkVersion = current.getFrameworkVersion();
            int threshold = localThreshold;
            boolean remotePresent = false;

            if ((current.getFrameworkProperties() & XposedService.PROP_CAP_REMOTE) != 0) {
                SharedPreferences remote = current.getRemotePreferences(ConfigBridge.REMOTE_PREF_GROUP);
                remotePresent = remote.contains(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP);
                if (remotePresent) {
                    threshold = remote.getInt(
                            ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP,
                            ConfigBridge.DEFAULT_THRESHOLD_DP);
                    threshold = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, threshold));
                }
            }

            final int launcherUid = resolveLauncherUid(context);
            final boolean hyosSpawnerPresent = new File(HYOS_SPAWNER).exists();
            final boolean launcherInScope = current.getScope().contains(TARGET_PACKAGE);

            HookedTarget matchedTarget = null;
            String matchMode = "none";
            StringBuilder targetDump = new StringBuilder();

            if (api >= XposedService.API_102) {
                List<HookedTarget> targets = current.getRunningTargets();

                for (HookedTarget target : targets) {
                    if (targetDump.length() > 0) targetDump.append(';');
                    targetDump.append(target.getProcessName())
                            .append(" uid=").append(target.getUid())
                            .append(" pid=").append(target.getPid())
                            .append(" state=").append(target.getState().name());

                    if (TARGET_PACKAGE.equals(target.getProcessName())) {
                        matchedTarget = target;
                        matchMode = "process-name";
                        break;
                    }
                }

                if (matchedTarget == null && launcherUid >= 0) {
                    for (HookedTarget target : targets) {
                        if (target.getUid() == launcherUid) {
                            matchedTarget = target;
                            matchMode = "launcher-uid";
                            break;
                        }
                    }
                }

                if (matchedTarget == null) {
                    for (HookedTarget target : targets) {
                        String processName = target.getProcessName();
                        if (processName != null && processName.contains("hyos_spawner")) {
                            matchedTarget = target;
                            matchMode = "hyos-process";
                            break;
                        }
                    }
                }
            }

            boolean directRuntimeTarget = matchedTarget != null;

            // Some HYOS-enabled LSPosed builds do not expose a native-only child as a Java-style
            // running target. In that case do not report a hard false negative merely because
            // processName != com.miui.home. Scope + the Xiaomi HYOS runtime is treated as module
            // activation evidence, while native Pattern/HOOK_HEALTH remains a separate health check.
            boolean hyosActivationFallback = !directRuntimeTarget
                    && api >= XposedService.API_102
                    && launcherInScope
                    && hyosSpawnerPresent;

            boolean launcherLoaded = directRuntimeTarget || hyosActivationFallback;
            String targetState = directRuntimeTarget
                    ? matchedTarget.getState().name()
                    : (hyosActivationFallback ? "UP_TO_DATE" : "");
            int targetPid = directRuntimeTarget ? matchedTarget.getPid() : 0;

            runtimeEvidence = "hyosSpawnerPresent=" + hyosSpawnerPresent
                    + " launcherUid=" + launcherUid
                    + " launcherInScope=" + launcherInScope
                    + " matchMode=" + matchMode
                    + " activation=" + (directRuntimeTarget
                    ? "running-target"
                    : (hyosActivationFallback ? "scope+hyos-fallback" : "none"))
                    + " targets=[" + targetDump + "]";

            serviceError = "";
            return new Snapshot(true, api, frameworkName, frameworkVersion,
                    launcherLoaded, targetState, targetPid, threshold, remotePresent, "");
        } catch (Throwable t) {
            String message = t.getMessage();
            if (message == null || message.isBlank()) message = t.getClass().getSimpleName();
            serviceError = message;
            runtimeEvidence = "status check failed: " + message;
            return new Snapshot(true, 0, "", "", false, "", 0,
                    localThreshold, false, message);
        }
    }

    public static String runtimeEvidence() {
        return runtimeEvidence;
    }

    static XposedService currentService(Context context) {
        initialize(context);
        return service;
    }

    static String currentServiceError() {
        return serviceError;
    }

    private static int resolveLauncherUid(Context context) {
        try {
            return context.getPackageManager().getApplicationInfo(TARGET_PACKAGE, 0).uid;
        } catch (Throwable ignored) {
            return -1;
        }
    }
}
