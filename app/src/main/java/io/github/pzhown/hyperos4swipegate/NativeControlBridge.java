package io.github.pzhown.hyperos4swipegate;

import android.app.BroadcastOptions;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Process;
import android.os.SystemClock;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * App-side endpoint for the HyperOS Runtime control protocol.
 *
 * There is intentionally no localhost socket. The app sends a nonce-bound query to the module code
 * running inside SystemUI. SystemUI carries the configuration through Xiaomi's existing protected
 * fsgesture broadcast into the HyOS launcher native receiver; the native bridge replies through the
 * HyperOS broadcast runtime and SystemUI relays the authenticated response back here.
 */
public final class NativeControlBridge {
    private static final long PEER_FRESH_MS = 5_000L;
    private static final long STATUS_REPLY_TIMEOUT_MS = 6_000L;
    private static final long PULSE_INTERVAL_MS = 1_500L;

    private static final AtomicBoolean STARTED = new AtomicBoolean(false);
    private static final AtomicBoolean RECEIVER_REGISTERED = new AtomicBoolean(false);
    private static final AtomicLong PENDING_NONCE = new AtomicLong(0L);

    private static volatile Context appContext;
    private static volatile String lastError = "";
    private static volatile String latestLog = "";
    private static volatile Snapshot latestSnapshot = Snapshot.unknown();
    private static volatile long unansweredSinceElapsedMs;
    private static volatile String channelStage = "APP_READY";
    private static volatile long channelEventElapsedMs = SystemClock.elapsedRealtime();

    private NativeControlBridge() {}

    public record Snapshot(
            String state,
            String pattern,
            String detail,
            long receivedAtElapsedMs
    ) {
        static Snapshot unknown() {
            return new Snapshot("UNKNOWN", "", "等待 HyOS Runtime Hook 状态", 0L);
        }

        public boolean fresh() {
            if (receivedAtElapsedMs <= 0L) return false;
            long age = SystemClock.elapsedRealtime() - receivedAtElapsedMs;
            return age >= 0L && age <= PEER_FRESH_MS;
        }
    }

    public static void initialize(Context context) {
        appContext = context.getApplicationContext();
        registerReplyReceiverIfNeeded();
        requestSync();
        if (!STARTED.compareAndSet(false, true)) return;

        Thread pulse = new Thread(NativeControlBridge::pulseLoop, "SwipeGateRuntimePulse");
        pulse.setDaemon(true);
        pulse.start();
    }

    public static void requestSync() {
        Context context = appContext;
        if (context == null) return;
        registerReplyReceiverIfNeeded();

        long now = SystemClock.elapsedRealtime();
        long nonce = PENDING_NONCE.get();
        boolean created = false;
        if (nonce <= 0L) {
            long candidate = SystemClock.elapsedRealtimeNanos();
            if (candidate <= 0L) candidate = System.nanoTime();
            if (candidate <= 0L) candidate = 1L;
            if (PENDING_NONCE.compareAndSet(0L, candidate)) {
                nonce = candidate;
                unansweredSinceElapsedMs = now;
                created = true;
            } else {
                nonce = PENDING_NONCE.get();
            }
        }
        if (nonce <= 0L) return;
        if (unansweredSinceElapsedMs <= 0L) unansweredSinceElapsedMs = now;

        if (created) {
            setChannelStage("APP_QUERY_SENT");
        }

        int threshold = ConfigBridge.localPreferences(context)
                .getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP);
        threshold = Math.max(ConfigBridge.STOCK_THRESHOLD_DP,
                Math.min(ConfigBridge.MAX_THRESHOLD_DP, threshold));
        int logLevel = ConfigBridge.sanitizeLogLevel(
                ConfigBridge.localPreferences(context).getInt(
                        ConfigBridge.PREF_KEY_LOG_LEVEL,
                        ConfigBridge.DEFAULT_LOG_LEVEL));
        boolean hapticEnabled = ConfigBridge.localPreferences(context).getBoolean(
                ConfigBridge.PREF_KEY_HAPTIC_FEEDBACK,
                ConfigBridge.DEFAULT_HAPTIC_FEEDBACK);

        try {
            Intent query = new Intent(SystemUiBridgeModule.ACTION_APP_QUERY)
                    .setPackage(SystemUiBridgeModule.SYSTEM_UI_PACKAGE)
                    .putExtra(SystemUiBridgeModule.EXTRA_NONCE, nonce)
                    .putExtra(SystemUiBridgeModule.EXTRA_THRESHOLD_DP, threshold)
                    .putExtra(SystemUiBridgeModule.EXTRA_LOG_LEVEL, logLevel)
                    .putExtra(SystemUiBridgeModule.EXTRA_HAPTIC_ENABLED, hapticEnabled)
                    .putExtra(SystemUiBridgeModule.EXTRA_SENDER_UID, Process.myUid());
            context.sendBroadcast(query, null,
                    BroadcastOptions.makeBasic().setShareIdentityEnabled(true).toBundle());
            lastError = "";
        } catch (Throwable t) {
            String message = t.getMessage();
            lastError = message == null || message.isBlank()
                    ? t.getClass().getSimpleName()
                    : message;
            setChannelStage("APP_SEND_ERROR");
        }
    }

    public static Snapshot snapshot() {
        Snapshot current = latestSnapshot;

        // Only a failure explicitly reported by the native side is a Native Hook failure.
        // Transport timeout must not be rewritten into FAILED because that hides whether the
        // gesture hook itself is healthy and made issue #7 look like a Launcher pattern problem.
        if ("FAILED".equals(current.state())) return current;

        long unansweredSince = unansweredSinceElapsedMs;
        long now = SystemClock.elapsedRealtime();
        if (!current.fresh()
                && unansweredSince > 0L
                && now - unansweredSince >= STATUS_REPLY_TIMEOUT_MS) {
            String detail = lastError.isBlank()
                    ? "Native 状态通道超时；Hook 本身尚未确认失败。lastStage=" + channelStage
                    : "Native 状态通道异常：" + lastError + "；lastStage=" + channelStage;
            return new Snapshot("CHANNEL_ERROR", current.pattern(), detail,
                    current.receivedAtElapsedMs());
        }
        return current;
    }

    public static boolean hasFreshPeer() {
        return latestSnapshot.fresh();
    }

    public static String channelStage() {
        return channelStage;
    }

    public static String channelDetail() {
        return switch (channelStage) {
            case "APP_READY" -> "App 状态通道已初始化";
            case "APP_QUERY_SENT" -> "App 已向 SystemUI 发送状态查询";
            case "SYSTEMUI_QUERY_RECEIVED" -> "SystemUI 已收到 App 状态查询";
            case "CARRIER_SENT" -> "SystemUI 已向 Launcher 发送 fsgesture carrier，等待 Native 回包";
            case "CARRIER_SEND_FAILED" -> "SystemUI 无法发送 fsgesture carrier";
            case "NATIVE_REPLY_REJECTED" -> "SystemUI 收到 Native 回包但认证或 nonce 校验失败";
            case "NATIVE_REPLY_RELAYED" -> "SystemUI 已把 Native 回包转发给 App";
            case "APP_NATIVE_REPLY_RECEIVED" -> "App 已收到并验证 Native 回包";
            case "APP_SEND_ERROR" -> "App 无法向 SystemUI 发送状态查询";
            case "APP_RECEIVER_ERROR" -> "App 无法注册状态回包接收器";
            default -> channelStage;
        };
    }

    public static long channelAgeMs() {
        long age = SystemClock.elapsedRealtime() - channelEventElapsedMs;
        return Math.max(0L, age);
    }

    public static boolean hasPendingQuery() {
        return PENDING_NONCE.get() > 0L;
    }

    public static void clearLog() {
        latestLog = "";
    }

    public static String currentLog() {
        Context context = appContext;
        int level = context == null
                ? ConfigBridge.DEFAULT_LOG_LEVEL
                : ConfigBridge.sanitizeLogLevel(
                        ConfigBridge.localPreferences(context).getInt(
                                ConfigBridge.PREF_KEY_LOG_LEVEL,
                                ConfigBridge.DEFAULT_LOG_LEVEL));
        if (level <= ConfigBridge.LOG_LEVEL_OFF) return "日志记录已关闭。";
        if (!lastError.isBlank()) {
            return "HyOS Runtime 通道异常：" + lastError
                    + "\nlastStage=" + channelStage + " · " + channelDetail();
        }
        Snapshot effective = snapshot();
        if ("CHANNEL_ERROR".equals(effective.state())) {
            return effective.detail() + "\nchannel=" + channelDetail();
        }
        if (!latestLog.isBlank()) return latestLog;
        if ("FAILED".equals(effective.state()) && !effective.detail().isBlank()) {
            return effective.detail();
        }
        if (latestSnapshot.fresh()) return "HyOS Runtime 已连接，暂无新的 Native 日志。";
        return "等待 SystemUI → HyOS Runtime 状态回包…\n"
                + "stage=" + channelStage + " · " + channelDetail();
    }

    private static void registerReplyReceiverIfNeeded() {
        Context context = appContext;
        if (context == null || !RECEIVER_REGISTERED.compareAndSet(false, true)) return;
        try {
            IntentFilter filter = new IntentFilter(SystemUiBridgeModule.ACTION_APP_REPLY);
            context.registerReceiver(replyReceiver, filter, Context.RECEIVER_EXPORTED);
        } catch (Throwable t) {
            RECEIVER_REGISTERED.set(false);
            String message = t.getMessage();
            lastError = message == null || message.isBlank()
                    ? t.getClass().getSimpleName()
                    : message;
            setChannelStage("APP_RECEIVER_ERROR");
        }
    }

    private static void pulseLoop() {
        while (true) {
            try {
                // Re-send the same in-flight nonce until it gets a valid Native reply. 0.7.1
                // generated a new nonce every 1.5 s, so a slower reply could always arrive after
                // both SystemUI and the App had already moved on to another nonce.
                requestSync();
                Thread.sleep(PULSE_INTERVAL_MS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                return;
            } catch (Throwable ignored) {
                try {
                    Thread.sleep(PULSE_INTERVAL_MS);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }

    private static final BroadcastReceiver replyReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!SystemUiBridgeModule.ACTION_APP_REPLY.equals(intent.getAction())) return;
            int senderUid = getSentFromUid();
            String senderPackage = getSentFromPackage();
            int claimedUid = intent.getIntExtra(SystemUiBridgeModule.EXTRA_SENDER_UID, -1);
            if (senderUid == Process.INVALID_UID
                    || senderUid != claimedUid
                    || !SystemUiBridgeModule.SYSTEM_UI_PACKAGE.equals(senderPackage)
                    || !isUidOwner(context, senderUid, SystemUiBridgeModule.SYSTEM_UI_PACKAGE)) {
                return;
            }

            long nonce = intent.getLongExtra(SystemUiBridgeModule.EXTRA_NONCE, 0L);
            long expected = PENDING_NONCE.get();
            if (nonce <= 0L || nonce != expected) return;

            String stage = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_CHANNEL_STAGE));
            if (!stage.isBlank()) setChannelStage(stage);

            int state = intent.getIntExtra(SystemUiBridgeModule.EXTRA_HOOK_STATE, 0);
            String pattern = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_PATTERN));
            String detail = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_DETAIL));
            String log = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_NATIVE_LOG));

            boolean nativeReply = "NATIVE_REPLY_RELAYED".equals(stage)
                    || state != 0
                    || !pattern.isBlank()
                    || !detail.isBlank()
                    || !log.isBlank();
            if (!nativeReply) {
                // This is a SystemUI transport ACK. Keep the nonce pending so the real Native reply
                // can still complete the same request.
                lastError = "";
                return;
            }

            latestSnapshot = new Snapshot(
                    stateName(state), pattern, detail, SystemClock.elapsedRealtime());
            if (!log.isBlank()) latestLog = log.trim();
            lastError = "";
            unansweredSinceElapsedMs = 0L;
            PENDING_NONCE.compareAndSet(nonce, 0L);
            setChannelStage("APP_NATIVE_REPLY_RECEIVED");
        }
    };

    private static void setChannelStage(String stage) {
        if (stage == null || stage.isBlank()) return;
        if (!stage.equals(channelStage)) {
            channelStage = stage;
            channelEventElapsedMs = SystemClock.elapsedRealtime();
        }
    }

    private static boolean isUidOwner(Context context, int uid, String packageName) {
        if (context == null || uid < 0) return false;
        try {
            String[] packages = context.getPackageManager().getPackagesForUid(uid);
            if (packages == null) return false;
            for (String candidate : packages) {
                if (packageName.equals(candidate)) return true;
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    private static String safeString(String value) {
        return value == null ? "" : value;
    }

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
