package io.github.pzhown.hyperos4swipegate;

import android.app.Activity;
import android.app.Application;
import android.app.BroadcastOptions;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;

/** Settings/status client. Gestures and persisted configuration live in Launcher. */
public final class NativeControlBridge {
    private static final long PEER_FRESH_MS = 5_000L;
    private static final long STATUS_REPLY_TIMEOUT_MS = 6_000L;
    private static final long PULSE_INTERVAL_MS = 1_500L;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static long lastIssuedNonce;

    private static volatile Context appContext;
    private static volatile Query pending;
    private static volatile String lastError = "";
    private static volatile String latestLog = "";
    private static volatile Snapshot latestSnapshot = Snapshot.unknown();
    private static volatile String channelStage = "APP_READY";
    private static volatile long channelEventElapsedMs = SystemClock.elapsedRealtime();
    private static volatile boolean timedOut;
    private static volatile boolean configPersisted;
    private static boolean receiverRegistered;
    private static boolean lifecycleRegistered;
    private static int startedActivities;

    private NativeControlBridge() {}

    public record Snapshot(String state, String pattern, String detail,
                           long systemUiLoadedVersionCode, long nativeLoadedVersionCode,
                           long receivedAtElapsedMs) {
        static Snapshot unknown() {
            return new Snapshot("UNKNOWN", "", "等待 HyOS Runtime Hook 状态", 0L, 0L, 0L);
        }
        public boolean fresh() {
            long age = SystemClock.elapsedRealtime() - receivedAtElapsedMs;
            return receivedAtElapsedMs > 0L && age >= 0L && age <= PEER_FRESH_MS;
        }
    }

    private record Query(long nonce, long createdAtMs, int threshold, int logLevel,
                         boolean haptic, boolean breakOpen) {
        boolean expired(long now) {
            return now < createdAtMs || now - createdAtMs >= STATUS_REPLY_TIMEOUT_MS;
        }
        boolean matches(Intent reply) {
            return reply.getIntExtra(SystemUiBridgeModule.EXTRA_THRESHOLD_DP, -1) == threshold
                    && reply.getIntExtra(SystemUiBridgeModule.EXTRA_LOG_LEVEL, -1) == logLevel
                    && reply.hasExtra(SystemUiBridgeModule.EXTRA_HAPTIC_ENABLED)
                    && reply.hasExtra(SystemUiBridgeModule.EXTRA_BREAK_OPEN_ENABLED)
                    && reply.getBooleanExtra(SystemUiBridgeModule.EXTRA_HAPTIC_ENABLED, false) == haptic
                    && reply.getBooleanExtra(SystemUiBridgeModule.EXTRA_BREAK_OPEN_ENABLED, false) == breakOpen;
        }
    }

    public static void initialize(Context context) {
        if (context == null) return;
        appContext = context.getApplicationContext();
        runOnMain(() -> {
            registerReplyReceiverIfNeeded();
            if (!lifecycleRegistered && appContext instanceof Application application) {
                application.registerActivityLifecycleCallbacks(LIFECYCLE);
                lifecycleRegistered = true;
            }
            sendQuery();
            schedulePulse();
        });
    }

    public static void requestConfigRefresh() {
        runOnMain(() -> {
            pending = null;
            configPersisted = false;
            timedOut = false;
            sendQuery();
            schedulePulse();
        });
    }

    public static void requestSync() {
        runOnMain(() -> {
            sendQuery();
            schedulePulse();
        });
    }

    private static void sendQuery() {
        Context context = appContext;
        if (context == null) return;
        registerReplyReceiverIfNeeded();
        long now = SystemClock.elapsedRealtime();
        if (pending != null && pending.expired(now)) expireQuery();
        if (pending == null) {
            if (lastIssuedNonce == Long.MAX_VALUE) {
                lastError = "请求序号已耗尽，请重启 App";
                return;
            }
            long nonce = Math.max(Math.max(1L, SystemClock.elapsedRealtimeNanos()), lastIssuedNonce + 1L);
            lastIssuedNonce = nonce;
            var preferences = ConfigBridge.localPreferences(context);
            int threshold = Math.max(ConfigBridge.STOCK_THRESHOLD_DP,
                    Math.min(ConfigBridge.MAX_THRESHOLD_DP, preferences.getInt(
                            ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP)));
            pending = new Query(nonce, now, threshold,
                    ConfigBridge.sanitizeLogLevel(preferences.getInt(
                            ConfigBridge.PREF_KEY_LOG_LEVEL, ConfigBridge.DEFAULT_LOG_LEVEL)),
                    preferences.getBoolean(ConfigBridge.PREF_KEY_HAPTIC_ENABLED,
                            ConfigBridge.DEFAULT_HAPTIC_ENABLED),
                    preferences.getBoolean(ConfigBridge.PREF_KEY_BREAK_OPEN_ENABLED,
                            ConfigBridge.DEFAULT_BREAK_OPEN_ENABLED));
            timedOut = false;
            setChannelStage("APP_QUERY_SENT");
        }
        Query query = pending;
        try {
            Intent intent = new Intent(SystemUiBridgeModule.ACTION_APP_QUERY)
                    .setPackage(SystemUiBridgeModule.SYSTEM_UI_PACKAGE)
                    .putExtra(SystemUiBridgeModule.EXTRA_NONCE, query.nonce())
                    .putExtra(SystemUiBridgeModule.EXTRA_THRESHOLD_DP, query.threshold())
                    .putExtra(SystemUiBridgeModule.EXTRA_LOG_LEVEL, query.logLevel())
                    .putExtra(SystemUiBridgeModule.EXTRA_HAPTIC_ENABLED, query.haptic())
                    .putExtra(SystemUiBridgeModule.EXTRA_BREAK_OPEN_ENABLED, query.breakOpen())
                    .putExtra(SystemUiBridgeModule.EXTRA_SENDER_UID, Process.myUid());
            context.sendBroadcast(intent, null,
                    BroadcastOptions.makeBasic().setShareIdentityEnabled(true).toBundle());
        } catch (Throwable t) {
            lastError = errorMessage(t);
            setChannelStage("APP_SEND_ERROR");
        }
    }

    private static final Runnable PULSE = () -> {
        if (pending != null && pending.expired(SystemClock.elapsedRealtime())) expireQuery();
        if (startedActivities == 0 && pending == null) return;
        sendQuery();
        schedulePulse();
    };

    private static void schedulePulse() {
        MAIN.removeCallbacks(PULSE);
        if (startedActivities == 0 && pending == null) return;
        long delay = PULSE_INTERVAL_MS;
        if (pending != null) {
            delay = Math.min(delay, Math.max(0L,
                    pending.createdAtMs() + STATUS_REPLY_TIMEOUT_MS - SystemClock.elapsedRealtime()));
        }
        MAIN.postDelayed(PULSE, delay);
    }

    private static void expireQuery() {
        pending = null;
        timedOut = true;
        setChannelStage("APP_QUERY_TIMEOUT");
    }

    private static final Application.ActivityLifecycleCallbacks LIFECYCLE =
            new Application.ActivityLifecycleCallbacks() {
        @Override public void onActivityStarted(Activity activity) {
            startedActivities++;
            requestSync();
        }
        @Override public void onActivityStopped(Activity activity) {
            startedActivities = Math.max(0, startedActivities - 1);
            schedulePulse();
        }
        @Override public void onActivityCreated(Activity activity, Bundle state) {}
        @Override public void onActivityResumed(Activity activity) {}
        @Override public void onActivityPaused(Activity activity) {}
        @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
        @Override public void onActivityDestroyed(Activity activity) {}
    };

    public static Snapshot snapshot() {
        Snapshot current = latestSnapshot;
        if ("FAILED".equals(current.state())) return current;
        if (timedOut) {
            String detail = lastError.isBlank()
                    ? "Native 状态通道超时；Hook 本身尚未确认失败。"
                    : "Native 配置/状态通道异常：" + lastError;
            return new Snapshot("CHANNEL_ERROR", current.pattern(), detail,
                    current.systemUiLoadedVersionCode(), current.nativeLoadedVersionCode(),
                    current.receivedAtElapsedMs());
        }
        return current;
    }

    public static boolean hasFreshPeer() { return latestSnapshot.fresh(); }
    public static boolean isConfigurationPersisted() { return configPersisted; }
    public static String channelStage() { return channelStage; }
    public static boolean hasPendingQuery() { return pending != null; }
    public static void clearLog() { latestLog = ""; }

    public static String channelDetail() {
        return switch (channelStage) {
            case "APP_READY" -> "App 状态通道已初始化";
            case "APP_QUERY_SENT" -> "App 已向 SystemUI 发送状态查询";
            case "SYSTEMUI_QUERY_RECEIVED" -> "SystemUI 已收到 App 状态查询";
            case "CARRIER_SENT" -> "SystemUI 已向 Launcher 发送 carrier，等待 Native 回包";
            case "CARRIER_SEND_FAILED" -> "SystemUI 无法发送 carrier，正在限时重试";
            case "NATIVE_REPLY_REJECTED" -> "Native 回包认证或 nonce 校验失败";
            case "NATIVE_REPLY_RELAYED" -> "SystemUI 已转发 Native 回包";
            case "APP_NATIVE_REPLY_RECEIVED" -> "Native 已确认配置应用并持久化，无需 App 常驻";
            case "CONFIG_NOT_PERSISTED" -> "Native 已应用配置，但尚未成功写入持久目录";
            case "APP_QUERY_TIMEOUT" -> "本次同步已超时；重新进入页面或修改设置可重试";
            case "APP_SEND_ERROR" -> "App 无法发送状态查询";
            case "APP_RECEIVER_ERROR" -> "App 无法注册状态回包接收器";
            default -> channelStage;
        };
    }

    public static long channelAgeMs() {
        return Math.max(0L, SystemClock.elapsedRealtime() - channelEventElapsedMs);
    }

    public static String currentLog() {
        Context context = appContext;
        int level = context == null ? ConfigBridge.DEFAULT_LOG_LEVEL
                : ConfigBridge.sanitizeLogLevel(ConfigBridge.localPreferences(context).getInt(
                        ConfigBridge.PREF_KEY_LOG_LEVEL, ConfigBridge.DEFAULT_LOG_LEVEL));
        if (level <= ConfigBridge.LOG_LEVEL_OFF) return "日志记录已关闭。";
        if (!lastError.isBlank()) return "HyOS Runtime 通道异常：" + lastError
                + "\nlastStage=" + channelStage + " · " + channelDetail();
        Snapshot effective = snapshot();
        if ("CHANNEL_ERROR".equals(effective.state())) return effective.detail();
        if (!latestLog.isBlank()) return latestLog;
        if ("FAILED".equals(effective.state()) && !effective.detail().isBlank()) return effective.detail();
        if (latestSnapshot.fresh()) return "HyOS Runtime 已连接，暂无新的 Native 日志。";
        return "等待 SystemUI → HyOS Runtime 状态回包…\nstage=" + channelStage + " · " + channelDetail();
    }

    private static void registerReplyReceiverIfNeeded() {
        Context context = appContext;
        if (context == null || receiverRegistered) return;
        try {
            context.registerReceiver(REPLY_RECEIVER,
                    new IntentFilter(SystemUiBridgeModule.ACTION_APP_REPLY), Context.RECEIVER_EXPORTED);
            receiverRegistered = true;
        } catch (Throwable t) {
            lastError = errorMessage(t);
            setChannelStage("APP_RECEIVER_ERROR");
        }
    }

    private static final BroadcastReceiver REPLY_RECEIVER = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!SystemUiBridgeModule.ACTION_APP_REPLY.equals(intent.getAction())) return;
            int senderUid = getSentFromUid();
            if (senderUid == Process.INVALID_UID
                    || senderUid != intent.getIntExtra(SystemUiBridgeModule.EXTRA_SENDER_UID, -1)
                    || !SystemUiBridgeModule.SYSTEM_UI_PACKAGE.equals(getSentFromPackage())
                    || !isUidOwner(context, senderUid, SystemUiBridgeModule.SYSTEM_UI_PACKAGE)) return;
            Query query = pending;
            if (query == null || query.expired(SystemClock.elapsedRealtime())
                    || intent.getLongExtra(SystemUiBridgeModule.EXTRA_NONCE, 0L) != query.nonce()) return;
            String stage = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_CHANNEL_STAGE));
            if (!"NATIVE_REPLY_RELAYED".equals(stage)) {
                if (!stage.isBlank()) setChannelStage(stage);
                return;
            }
            if (!query.matches(intent)) return;
            latestSnapshot = new Snapshot(
                    stateName(intent.getIntExtra(SystemUiBridgeModule.EXTRA_HOOK_STATE, 0)),
                    safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_PATTERN)),
                    safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_DETAIL)),
                    intent.getIntExtra(SystemUiBridgeModule.EXTRA_SYSTEMUI_MODULE_VERSION, 0),
                    intent.getIntExtra(SystemUiBridgeModule.EXTRA_NATIVE_MODULE_VERSION, 0),
                    SystemClock.elapsedRealtime());
            String log = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_NATIVE_LOG));
            if (!log.isBlank()) latestLog = log.trim();
            configPersisted = intent.getBooleanExtra(SystemUiBridgeModule.EXTRA_CONFIG_PERSISTED, false);
            if (configPersisted) {
                pending = null;
                timedOut = false;
                lastError = "";
                setChannelStage("APP_NATIVE_REPLY_RECEIVED");
            } else {
                lastError = "配置已应用，但尚未持久化；正在限时重试";
                setChannelStage("CONFIG_NOT_PERSISTED");
            }
            schedulePulse();
        }
    };

    private static void runOnMain(Runnable action) {
        if (Looper.myLooper() == Looper.getMainLooper()) action.run();
        else MAIN.post(action);
    }

    private static void setChannelStage(String stage) {
        if (stage != null && !stage.isBlank() && !stage.equals(channelStage)) {
            channelStage = stage;
            channelEventElapsedMs = SystemClock.elapsedRealtime();
        }
    }

    private static boolean isUidOwner(Context context, int uid, String packageName) {
        try {
            String[] packages = context.getPackageManager().getPackagesForUid(uid);
            if (packages != null) {
                for (String candidate : packages) if (packageName.equals(candidate)) return true;
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    private static String errorMessage(Throwable t) {
        return t.getMessage() == null || t.getMessage().isBlank()
                ? t.getClass().getSimpleName() : t.getMessage();
    }
    private static String safeString(String value) { return value == null ? "" : value; }
    private static String stateName(int state) {
        return switch (state) {
            case 1 -> "WAITING";
            case 2 -> "HEALTHY";
            case 3 -> "REPAIRING";
            case 4 -> "FAILED";
            default -> "UNKNOWN";
        };
    }
}
