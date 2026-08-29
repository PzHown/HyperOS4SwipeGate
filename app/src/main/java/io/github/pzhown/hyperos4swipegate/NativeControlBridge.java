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
    private static final long PULSE_INTERVAL_MS = 1_500L;

    private static final AtomicBoolean STARTED = new AtomicBoolean(false);
    private static final AtomicBoolean RECEIVER_REGISTERED = new AtomicBoolean(false);
    private static final AtomicLong PENDING_NONCE = new AtomicLong(0L);

    private static volatile Context appContext;
    private static volatile String lastError = "";
    private static volatile String latestLog = "";
    private static volatile Snapshot latestSnapshot = Snapshot.unknown();

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

        long nonce = SystemClock.elapsedRealtimeNanos();
        if (nonce <= 0L) nonce = System.nanoTime();
        if (nonce <= 0L) nonce = 1L;
        PENDING_NONCE.set(nonce);

        int threshold = ConfigBridge.localPreferences(context)
                .getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP);
        threshold = Math.max(ConfigBridge.STOCK_THRESHOLD_DP,
                Math.min(ConfigBridge.MAX_THRESHOLD_DP, threshold));
        int logLevel = ConfigBridge.sanitizeLogLevel(
                ConfigBridge.localPreferences(context).getInt(
                        ConfigBridge.PREF_KEY_LOG_LEVEL,
                        ConfigBridge.DEFAULT_LOG_LEVEL));

        try {
            Intent query = new Intent(SystemUiBridgeModule.ACTION_APP_QUERY)
                    .setPackage(SystemUiBridgeModule.SYSTEM_UI_PACKAGE)
                    .putExtra(SystemUiBridgeModule.EXTRA_NONCE, nonce)
                    .putExtra(SystemUiBridgeModule.EXTRA_THRESHOLD_DP, threshold)
                    .putExtra(SystemUiBridgeModule.EXTRA_LOG_LEVEL, logLevel)
                    .putExtra(SystemUiBridgeModule.EXTRA_SENDER_UID, Process.myUid());
            context.sendBroadcast(query, null,
                    BroadcastOptions.makeBasic().setShareIdentityEnabled(true).toBundle());
            lastError = "";
        } catch (Throwable t) {
            String message = t.getMessage();
            lastError = message == null || message.isBlank()
                    ? t.getClass().getSimpleName()
                    : message;
        }
    }

    public static Snapshot snapshot() {
        return latestSnapshot;
    }

    public static boolean hasFreshPeer() {
        return latestSnapshot.fresh();
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
        if (!latestLog.isBlank()) return latestLog;
        if (!lastError.isBlank()) return "HyOS Runtime 通道异常：" + lastError;
        if (latestSnapshot.fresh()) return "HyOS Runtime 已连接，暂无新的 Native 日志。";
        return "等待 SystemUI → HyOS Runtime 状态回包…\n"
                + "配置和状态通过小米 fsgesture Native Broadcast 通道同步，不再使用本地端口。";
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
        }
    }

    private static void pulseLoop() {
        while (true) {
            try {
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

            int state = intent.getIntExtra(SystemUiBridgeModule.EXTRA_HOOK_STATE, 0);
            String pattern = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_PATTERN));
            String detail = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_DETAIL));
            String log = safeString(intent.getStringExtra(SystemUiBridgeModule.EXTRA_NATIVE_LOG));
            latestSnapshot = new Snapshot(
                    stateName(state), pattern, detail, SystemClock.elapsedRealtime());
            if (!log.isBlank()) latestLog = log.trim();
            lastError = "";
            PENDING_NONCE.compareAndSet(nonce, 0L);
        }
    };

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
