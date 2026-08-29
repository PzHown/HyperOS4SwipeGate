package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import io.github.libxposed.service.HookedTarget;
import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.XposedServiceHelper;

public final class XposedServiceBridge {
    private static final String TARGET_PACKAGE = "com.miui.home";
    private static final String HYOS_SPAWNER = "/system_ext/bin/hyos_spawner";

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
        DiagnosticsStreamBridge.initialize(context);
        appContext = context.getApplicationContext();
        if (!INITIALIZED.compareAndSet(false, true)) return;
        XposedServiceHelper.registerListener(new XposedServiceHelper.OnServiceListener() {
            @Override
            public void onServiceBind(XposedService boundService) {
                service = boundService;
                serviceError = "";
                syncRemoteState(boundService);
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

    private static void syncRemoteState(XposedService current) {
        Context context = appContext;
        if (context == null) return;
        try {
            if (current.getApiVersion() < XposedService.API_102) return;
            if ((current.getFrameworkProperties() & XposedService.PROP_CAP_REMOTE) == 0) return;
            SharedPreferences local = ConfigBridge.localPreferences(context);
            SharedPreferences remote = current.getRemotePreferences(ConfigBridge.REMOTE_PREF_GROUP);
            SharedPreferences.Editor editor = remote.edit();
            boolean changed = false;

            if (!remote.contains(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP)) {
                int localValue = local.getInt(
                        ConfigBridge.PREF_KEY_THRESHOLD_DP,
                        ConfigBridge.DEFAULT_THRESHOLD_DP);
                localValue = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, localValue));
                editor.putInt(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP, localValue);
                changed = true;
            }

            int storedLogLevel = local.getInt(
                    ConfigBridge.PREF_KEY_LOG_LEVEL,
                    ConfigBridge.DEFAULT_LOG_LEVEL);
            int localLogLevel = ConfigBridge.sanitizeLogLevel(storedLogLevel);
            if (storedLogLevel != localLogLevel) {
                local.edit()
                        .putInt(ConfigBridge.PREF_KEY_LOG_LEVEL, localLogLevel)
                        .apply();
            }
            if (remote.getInt(
                    ConfigBridge.REMOTE_PREF_KEY_LOG_LEVEL,
                    ConfigBridge.DEFAULT_LOG_LEVEL) != localLogLevel) {
                editor.putInt(ConfigBridge.REMOTE_PREF_KEY_LOG_LEVEL, localLogLevel);
                changed = true;
            }

            int diagnosticsPort = DiagnosticsStreamBridge.port();
            String diagnosticsToken = DiagnosticsStreamBridge.token();
            if (diagnosticsPort > 0 && !diagnosticsToken.isBlank()) {
                if (remote.getInt(ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_PORT, 0)
                        != diagnosticsPort) {
                    editor.putInt(ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_PORT, diagnosticsPort);
                    changed = true;
                }
                String remoteToken = remote.getString(
                        ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_TOKEN, "");
                if (!diagnosticsToken.equals(remoteToken)) {
                    editor.putString(
                            ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_TOKEN,
                            diagnosticsToken);
                    changed = true;
                }
            }

            if (changed && !editor.commit()) {
                serviceError = "远程状态同步失败";
            }
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

    public static ConfigBridge.Result applyLogLevel(Context context, int logLevel) {
        initialize(context);
        int safeValue = ConfigBridge.sanitizeLogLevel(logLevel);
        ConfigBridge.localPreferences(context)
                .edit()
                .putInt(ConfigBridge.PREF_KEY_LOG_LEVEL, safeValue)
                .apply();
        DiagnosticsStreamBridge.clearLog();

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
                    .putInt(ConfigBridge.REMOTE_PREF_KEY_LOG_LEVEL, safeValue)
                    .commit();
            if (!committed) {
                return new ConfigBridge.Result(false, safeValue, "日志设置写入失败");
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
        int logLevel = ConfigBridge.sanitizeLogLevel(
                ConfigBridge.localPreferences(context).getInt(
                        ConfigBridge.PREF_KEY_LOG_LEVEL,
                        ConfigBridge.DEFAULT_LOG_LEVEL));
        if (logLevel <= ConfigBridge.LOG_LEVEL_OFF) {
            return "日志记录已关闭。";
        }
        return DiagnosticsStreamBridge.currentLog();
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
            // running target. Scope + the Xiaomi HYOS runtime is still valid activation evidence,
            // but it is no longer sufficient for a green/active UI state. Native Hook health must
            // independently arrive through DiagnosticsStreamBridge.
            boolean hyosActivationFallback = !directRuntimeTarget
                    && api >= XposedService.API_102
                    && launcherInScope
                    && hyosSpawnerPresent;

            boolean launcherLoaded = directRuntimeTarget || hyosActivationFallback;
            String frameworkTargetState = directRuntimeTarget
                    ? matchedTarget.getState().name()
                    : (hyosActivationFallback ? "UP_TO_DATE" : "");
            int targetPid = directRuntimeTarget ? matchedTarget.getPid() : 0;

            DiagnosticsStreamBridge.NativeHookStatus nativeHookStatus =
                    DiagnosticsStreamBridge.nativeHookStatus();
            String targetState = frameworkTargetState;
            if (launcherLoaded) {
                if ("FAILED".equals(frameworkTargetState)) {
                    targetState = "FAILED";
                } else if ("RELOADING".equals(frameworkTargetState)) {
                    targetState = "RELOADING";
                } else if (!nativeHookStatus.fresh()) {
                    // Strict readiness: activation/scope alone must never become Active.
                    targetState = "RELOADING";
                } else {
                    targetState = switch (nativeHookStatus.state()) {
                        case "HEALTHY" -> frameworkTargetState;
                        case "FAILED" -> "FAILED";
                        case "REPAIRING", "WAITING", "UNKNOWN" -> "RELOADING";
                        default -> "RELOADING";
                    };
                }
            }

            runtimeEvidence = "hyosSpawnerPresent=" + hyosSpawnerPresent
                    + " launcherUid=" + launcherUid
                    + " launcherInScope=" + launcherInScope
                    + " matchMode=" + matchMode
                    + " activation=" + (directRuntimeTarget
                    ? "running-target"
                    : (hyosActivationFallback ? "scope+hyos-fallback" : "none"))
                    + " nativeHookState=" + nativeHookStatus.state()
                    + " nativeHookFresh=" + nativeHookStatus.fresh()
                    + " nativeHookPattern=" + nativeHookStatus.pattern()
                    + " nativeHookDetail=" + nativeHookStatus.detail()
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
