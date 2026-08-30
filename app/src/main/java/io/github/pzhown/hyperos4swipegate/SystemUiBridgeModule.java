package io.github.pzhown.hyperos4swipegate;

import android.app.BroadcastOptions;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Process;

import androidx.annotation.NonNull;

import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

/**
 * Java-side relay that intentionally lives in SystemUI, not MiuiHome.
 *
 * HyperOS 4 MiuiHome is a Rust/hyos_spawner process and does not provide a reliable libxposed Java
 * runtime. SystemUI does. We therefore reuse Xiaomi's existing protected
 * {@code com.android.systemui.fsgesture} receiver as the authenticated carrier into the launcher
 * native runtime, then relay the native HyperOS broadcast reply back to the SwipeGate app.
 */
public final class SystemUiBridgeModule extends XposedModule {
    static final String SYSTEM_UI_PACKAGE = "com.android.systemui";
    static final String LAUNCHER_PACKAGE = "com.miui.home";
    static final String MODULE_PACKAGE = "io.github.pzhown.hyperos4swipegate";

    static final String ACTION_APP_QUERY = MODULE_PACKAGE + ".action.RUNTIME_QUERY";
    static final String ACTION_APP_REPLY = MODULE_PACKAGE + ".action.RUNTIME_REPLY";
    static final String ACTION_NATIVE_REPLY = MODULE_PACKAGE + ".action.NATIVE_RUNTIME_REPLY";
    static final String ACTION_HYOS_CARRIER = "com.android.systemui.fsgesture";

    static final String EXTRA_MARKER = "swipegate_control";
    static final String EXTRA_NONCE = "swipegate_nonce";
    static final String EXTRA_THRESHOLD_DP = "swipegate_threshold_dp";
    static final String EXTRA_LOG_LEVEL = "swipegate_log_level";
    static final String EXTRA_HOOK_STATE = "swipegate_hook_state";
    static final String EXTRA_PATTERN = "swipegate_pattern";
    static final String EXTRA_DETAIL = "swipegate_detail";
    static final String EXTRA_NATIVE_LOG = "swipegate_native_log";
    static final String EXTRA_CHANNEL_STAGE = "swipegate_channel_stage";
    static final String EXTRA_SENDER_UID = "sender_uid";

    private static final int MAX_CONTEXT_ATTEMPTS = 80;
    private static final long CONTEXT_RETRY_MS = 250L;

    private final AtomicBoolean started = new AtomicBoolean(false);
    private final AtomicLong pendingNonce = new AtomicLong(0L);
    private volatile Context systemUiContext;

    @Override
    public void onModuleLoaded(@NonNull XposedModuleInterface.ModuleLoadedParam param) {
        if (!SYSTEM_UI_PACKAGE.equals(param.getProcessName())) return;
        if (!started.compareAndSet(false, true)) return;

        Thread worker = new Thread(this::initializeWhenContextReady, "SwipeGateSystemUiBridge");
        worker.setDaemon(true);
        worker.start();
    }

    private void initializeWhenContextReady() {
        for (int attempt = 0; attempt < MAX_CONTEXT_ATTEMPTS; attempt++) {
            Context context = currentApplication();
            if (context != null) {
                try {
                    initialize(context.getApplicationContext());
                    return;
                } catch (Throwable t) {
                    log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                            "SystemUI runtime bridge initialization failed", t);
                    return;
                }
            }
            try {
                Thread.sleep(CONTEXT_RETRY_MS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                return;
            }
        }
        log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                "SystemUI runtime bridge failed: application Context unavailable");
    }

    private void initialize(Context context) {
        systemUiContext = context;
        IntentFilter appQuery = new IntentFilter(ACTION_APP_QUERY);
        context.registerReceiver(appQueryReceiver, appQuery, Context.RECEIVER_EXPORTED);

        IntentFilter nativeReply = new IntentFilter(ACTION_NATIVE_REPLY);
        context.registerReceiver(nativeReplyReceiver, nativeReply, Context.RECEIVER_EXPORTED);

        log(android.util.Log.INFO, "HyperOS4SwipeGateSystemUI",
                "SystemUI runtime bridge ready uid=" + Process.myUid());
    }

    private final BroadcastReceiver appQueryReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!ACTION_APP_QUERY.equals(intent.getAction())) return;
            final int senderUid = getSentFromUid();
            final String senderPackage = getSentFromPackage();
            if (senderUid == Process.INVALID_UID
                    || !MODULE_PACKAGE.equals(senderPackage)
                    || !isUidOwner(context, senderUid, MODULE_PACKAGE)) {
                log(android.util.Log.WARN, "HyperOS4SwipeGateSystemUI",
                        "Rejected runtime query from uid=" + senderUid
                                + " package=" + senderPackage);
                return;
            }

            final long nonce = intent.getLongExtra(EXTRA_NONCE, 0L);
            final int thresholdDp = intent.getIntExtra(
                    EXTRA_THRESHOLD_DP, ConfigBridge.STOCK_THRESHOLD_DP);
            final int logLevel = intent.getIntExtra(
                    EXTRA_LOG_LEVEL, ConfigBridge.DEFAULT_LOG_LEVEL);
            if (nonce <= 0L
                    || thresholdDp < ConfigBridge.STOCK_THRESHOLD_DP
                    || thresholdDp > ConfigBridge.MAX_THRESHOLD_DP
                    || logLevel < ConfigBridge.LOG_LEVEL_OFF
                    || logLevel > ConfigBridge.LOG_LEVEL_DETAILED) {
                log(android.util.Log.WARN, "HyperOS4SwipeGateSystemUI",
                        "Rejected malformed runtime query nonce=" + nonce
                                + " threshold=" + thresholdDp + " logLevel=" + logLevel);
                return;
            }

            long previousNonce = pendingNonce.getAndSet(nonce);
            if (previousNonce != nonce) {
                sendAppStage(context, nonce, "SYSTEMUI_QUERY_RECEIVED");
            }
            try {
                Intent carrier = new Intent(ACTION_HYOS_CARRIER)
                        .setPackage(LAUNCHER_PACKAGE)
                        .putExtra(EXTRA_MARKER, true)
                        .putExtra(EXTRA_NONCE, nonce)
                        .putExtra(EXTRA_THRESHOLD_DP, thresholdDp)
                        .putExtra(EXTRA_LOG_LEVEL, logLevel)
                        .putExtra(EXTRA_SENDER_UID, Process.myUid());
                context.sendBroadcast(carrier, null, shareIdentityOptions());
                log(android.util.Log.INFO, "HyperOS4SwipeGateSystemUI",
                        "Runtime carrier sent nonce=" + nonce
                                + " threshold=" + thresholdDp + " logLevel=" + logLevel);
                if (previousNonce != nonce) {
                    sendAppStage(context, nonce, "CARRIER_SENT");
                }
            } catch (Throwable t) {
                log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                        "Runtime carrier send failed", t);
                sendAppStage(context, nonce, "CARRIER_SEND_FAILED");
            }
        }
    };

    private final BroadcastReceiver nativeReplyReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!ACTION_NATIVE_REPLY.equals(intent.getAction())) return;
            final long nonce = intent.getLongExtra(EXTRA_NONCE, 0L);
            final long expected = pendingNonce.get();
            final int claimedLauncherUid = intent.getIntExtra(EXTRA_SENDER_UID, -1);
            if (nonce <= 0L || nonce != expected
                    || !isUidOwner(context, claimedLauncherUid, LAUNCHER_PACKAGE)) {
                log(android.util.Log.WARN, "HyperOS4SwipeGateSystemUI",
                        "Rejected native reply nonce=" + nonce + " expected=" + expected
                                + " launcherUid=" + claimedLauncherUid);
                if (expected > 0L) {
                    sendAppStage(context, expected, "NATIVE_REPLY_REJECTED");
                }
                return;
            }

            try {
                Intent reply = new Intent(ACTION_APP_REPLY)
                        .setPackage(MODULE_PACKAGE)
                        .putExtra(EXTRA_NONCE, nonce)
                        .putExtra(EXTRA_HOOK_STATE,
                                intent.getIntExtra(EXTRA_HOOK_STATE, 0))
                        .putExtra(EXTRA_THRESHOLD_DP,
                                intent.getIntExtra(EXTRA_THRESHOLD_DP,
                                        ConfigBridge.STOCK_THRESHOLD_DP))
                        .putExtra(EXTRA_LOG_LEVEL,
                                intent.getIntExtra(EXTRA_LOG_LEVEL,
                                        ConfigBridge.DEFAULT_LOG_LEVEL))
                        .putExtra(EXTRA_PATTERN, safeString(intent.getStringExtra(EXTRA_PATTERN)))
                        .putExtra(EXTRA_DETAIL, safeString(intent.getStringExtra(EXTRA_DETAIL)))
                        .putExtra(EXTRA_NATIVE_LOG,
                                safeString(intent.getStringExtra(EXTRA_NATIVE_LOG)))
                        .putExtra(EXTRA_CHANNEL_STAGE, "NATIVE_REPLY_RELAYED")
                        .putExtra(EXTRA_SENDER_UID, Process.myUid());
                context.sendBroadcast(reply, null, shareIdentityOptions());
                pendingNonce.compareAndSet(nonce, 0L);
                log(android.util.Log.INFO, "HyperOS4SwipeGateSystemUI",
                        "Native runtime reply relayed nonce=" + nonce);
            } catch (Throwable t) {
                log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                        "Runtime reply relay failed", t);
            }
        }
    };

    private void sendAppStage(Context context, long nonce, String stage) {
        if (context == null || nonce <= 0L || stage == null || stage.isBlank()) return;
        try {
            Intent reply = new Intent(ACTION_APP_REPLY)
                    .setPackage(MODULE_PACKAGE)
                    .putExtra(EXTRA_NONCE, nonce)
                    .putExtra(EXTRA_HOOK_STATE, 0)
                    .putExtra(EXTRA_CHANNEL_STAGE, stage)
                    .putExtra(EXTRA_SENDER_UID, Process.myUid());
            context.sendBroadcast(reply, null, shareIdentityOptions());
            log(android.util.Log.INFO, "HyperOS4SwipeGateSystemUI",
                    "Runtime stage ack=" + stage + " nonce=" + nonce);
        } catch (Throwable t) {
            log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                    "Runtime stage ack failed stage=" + stage, t);
        }
    }

    private static Context currentApplication() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method method = activityThread.getDeclaredMethod("currentApplication");
            method.setAccessible(true);
            Object value = method.invoke(null);
            return value instanceof Context ? (Context) value : null;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static boolean isUidOwner(Context context, int uid, String packageName) {
        if (context == null || uid < 0 || packageName == null) return false;
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

    private static android.os.Bundle shareIdentityOptions() {
        return BroadcastOptions.makeBasic().setShareIdentityEnabled(true).toBundle();
    }

    private static String safeString(String value) {
        return value == null ? "" : value;
    }
}
