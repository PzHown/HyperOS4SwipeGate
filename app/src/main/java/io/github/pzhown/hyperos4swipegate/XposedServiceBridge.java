package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import io.github.libxposed.service.HookedTarget;
import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.XposedServiceHelper;

public final class XposedServiceBridge {
    private static final AtomicBoolean INITIALIZED = new AtomicBoolean(false);
    private static volatile XposedService service;
    private static volatile String serviceError = "";

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
        if (!INITIALIZED.compareAndSet(false, true)) return;
        XposedServiceHelper.registerListener(new XposedServiceHelper.OnServiceListener() {
            @Override
            public void onServiceBind(XposedService boundService) {
                service = boundService;
                serviceError = "";
            }

            @Override
            public void onServiceDied(XposedService deadService) {
                if (service == deadService) {
                    service = null;
                    serviceError = "LSPosed 服务已断开";
                }
            }
        });
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

    public static Snapshot snapshot(Context context) {
        initialize(context);
        int localThreshold = ConfigBridge.localPreferences(context)
                .getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP);
        localThreshold = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, localThreshold));

        XposedService current = service;
        if (current == null) {
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

            boolean launcherLoaded = false;
            String targetState = "";
            int targetPid = 0;
            if (api >= XposedService.API_102) {
                List<HookedTarget> targets = current.getRunningTargets();
                for (HookedTarget target : targets) {
                    if ("com.miui.home".equals(target.getProcessName())) {
                        launcherLoaded = true;
                        targetState = target.getState().name();
                        targetPid = target.getPid();
                        break;
                    }
                }
            }

            serviceError = "";
            return new Snapshot(true, api, frameworkName, frameworkVersion,
                    launcherLoaded, targetState, targetPid, threshold, remotePresent, "");
        } catch (Throwable t) {
            String message = t.getMessage();
            if (message == null || message.isBlank()) message = t.getClass().getSimpleName();
            serviceError = message;
            return new Snapshot(true, 0, "", "", false, "", 0,
                    localThreshold, false, message);
        }
    }
}
