package io.github.pzhown.hyperos4swipegate;

import android.app.BroadcastOptions;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Process;
import android.os.SystemClock;

import androidx.annotation.NonNull;

import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicBoolean;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

/**
 * Relay in SystemUI, independent from the settings App's lifetime.
 * The return capability is sent only through Xiaomi's protected, package-targeted
 * fsgesture carrier. It is never included in App broadcasts or diagnostic logs.
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
    static final String EXTRA_CHALLENGE_HIGH = "swipegate_challenge_high";
    static final String EXTRA_CHALLENGE_LOW = "swipegate_challenge_low";
    static final String EXTRA_CONFIG_PERSISTED = "swipegate_config_persisted";
    static final String EXTRA_THRESHOLD_DP = "swipegate_threshold_dp";
    static final String EXTRA_LOG_LEVEL = "swipegate_log_level";
    static final String EXTRA_HAPTIC_ENABLED = "swipegate_haptic_enabled";
    static final String EXTRA_BREAK_OPEN_ENABLED = "swipegate_break_open_enabled";
    static final String EXTRA_HOOK_STATE = "swipegate_hook_state";
    static final String EXTRA_SYSTEMUI_MODULE_VERSION = "swipegate_systemui_module_version";
    static final String EXTRA_NATIVE_MODULE_VERSION = "swipegate_native_module_version";
    static final String EXTRA_PATTERN = "swipegate_pattern";
    static final String EXTRA_DETAIL = "swipegate_detail";
    static final String EXTRA_NATIVE_LOG = "swipegate_native_log";
    static final String EXTRA_CHANNEL_STAGE = "swipegate_channel_stage";
    static final String EXTRA_SENDER_UID = "sender_uid";

    private static final int MAX_CONTEXT_ATTEMPTS = 80;
    private static final long CONTEXT_RETRY_MS = 250L;
    private final AtomicBoolean started = new AtomicBoolean(false);
    private ControlRequest pending;
    private long newestAppNonce;
    private Intent cachedReply;

    @Override
    public void onModuleLoaded(@NonNull XposedModuleInterface.ModuleLoadedParam param) {
        if (!SYSTEM_UI_PACKAGE.equals(param.getProcessName())
                || !started.compareAndSet(false, true)) return;
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
        context.registerReceiver(appQueryReceiver, new IntentFilter(ACTION_APP_QUERY),
                Context.RECEIVER_EXPORTED);
        context.registerReceiver(nativeReplyReceiver, new IntentFilter(ACTION_NATIVE_REPLY),
                Context.RECEIVER_EXPORTED);
        log(android.util.Log.INFO, "HyperOS4SwipeGateSystemUI",
                "SystemUI runtime bridge ready uid=" + Process.myUid()
                        + " loadedVersion=" + BuildConfig.VERSION_CODE);
    }

    private final BroadcastReceiver appQueryReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!ACTION_APP_QUERY.equals(intent.getAction())) return;
            int senderUid = getSentFromUid();
            if (senderUid == Process.INVALID_UID
                    || !MODULE_PACKAGE.equals(getSentFromPackage())
                    || !isUidOwner(context, senderUid, MODULE_PACKAGE)) return;

            long nonce = intent.getLongExtra(EXTRA_NONCE, 0L);
            int threshold = intent.getIntExtra(EXTRA_THRESHOLD_DP, -1);
            int level = intent.getIntExtra(EXTRA_LOG_LEVEL, -1);
            boolean haptic = intent.getBooleanExtra(EXTRA_HAPTIC_ENABLED, false);
            boolean breakOpen = intent.getBooleanExtra(EXTRA_BREAK_OPEN_ENABLED, false);
            if (nonce <= 0L || threshold < ConfigBridge.STOCK_THRESHOLD_DP
                    || threshold > ConfigBridge.MAX_THRESHOLD_DP
                    || level < ConfigBridge.LOG_LEVEL_OFF || level > ConfigBridge.LOG_LEVEL_DETAILED
                    || !intent.hasExtra(EXTRA_HAPTIC_ENABLED)
                    || !intent.hasExtra(EXTRA_BREAK_OPEN_ENABLED)) return;

            if (nonce < newestAppNonce) return;
            newestAppNonce = nonce;
            long now = SystemClock.elapsedRealtime();
            if (pending == null || pending.nonce != nonce || pending.expired(now)) {
                pending = new ControlRequest(nonce, now, threshold, level, haptic, breakOpen);
                cachedReply = null;
            } else if (!pending.matchesConfig(threshold, level, haptic, breakOpen)) {
                return;
            }
            if (!pending.canForward(now)) return;
            pending.markForwarded(now);
            if (cachedReply != null) {
                relay(context, cachedReply);
                return;
            }
            sendAppStage(context, nonce, "SYSTEMUI_QUERY_RECEIVED");
            try {
                Intent carrier = new Intent(ACTION_HYOS_CARRIER)
                        .setPackage(LAUNCHER_PACKAGE)
                        .putExtra(EXTRA_MARKER, true)
                        .putExtra(EXTRA_NONCE, nonce)
                        .putExtra(EXTRA_CHALLENGE_HIGH, pending.challengeHigh)
                        .putExtra(EXTRA_CHALLENGE_LOW, pending.challengeLow)
                        .putExtra(EXTRA_THRESHOLD_DP, threshold)
                        .putExtra(EXTRA_LOG_LEVEL, level)
                        .putExtra(EXTRA_HAPTIC_ENABLED, haptic)
                        .putExtra(EXTRA_BREAK_OPEN_ENABLED, breakOpen)
                        .putExtra(EXTRA_SENDER_UID, Process.myUid());
                context.sendBroadcast(carrier, null, shareIdentityOptions());
                sendAppStage(context, nonce, "CARRIER_SENT");
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
            if (!ACTION_NATIVE_REPLY.equals(intent.getAction()) || pending == null) return;
            long nonce = intent.getLongExtra(EXTRA_NONCE, 0L);
            if (!pending.authenticates(nonce,
                    intent.getLongExtra(EXTRA_CHALLENGE_HIGH, 0L),
                    intent.getLongExtra(EXTRA_CHALLENGE_LOW, 0L),
                    SystemClock.elapsedRealtime())) return;

            int actualUid = getSentFromUid();
            String actualPackage = getSentFromPackage();
            if (actualUid != Process.INVALID_UID
                    && !isUidOwner(context, actualUid, LAUNCHER_PACKAGE)) return;
            if (actualPackage != null && !LAUNCHER_PACKAGE.equals(actualPackage)) return;

            int threshold = intent.getIntExtra(EXTRA_THRESHOLD_DP, -1);
            int level = intent.getIntExtra(EXTRA_LOG_LEVEL, -1);
            boolean haptic = intent.getBooleanExtra(EXTRA_HAPTIC_ENABLED, false);
            boolean breakOpen = intent.getBooleanExtra(EXTRA_BREAK_OPEN_ENABLED, false);
            if (!intent.hasExtra(EXTRA_HAPTIC_ENABLED)
                    || !intent.hasExtra(EXTRA_BREAK_OPEN_ENABLED)
                    || !pending.matchesConfig(threshold, level, haptic, breakOpen)) return;
            boolean persisted = intent.getBooleanExtra(EXTRA_CONFIG_PERSISTED, false);
            Intent reply = new Intent(ACTION_APP_REPLY)
                    .setPackage(MODULE_PACKAGE)
                    .putExtra(EXTRA_NONCE, nonce)
                    .putExtra(EXTRA_HOOK_STATE, intent.getIntExtra(EXTRA_HOOK_STATE, 0))
                    .putExtra(EXTRA_SYSTEMUI_MODULE_VERSION, BuildConfig.VERSION_CODE)
                    .putExtra(EXTRA_NATIVE_MODULE_VERSION,
                            intent.getIntExtra(EXTRA_NATIVE_MODULE_VERSION, 0))
                    .putExtra(EXTRA_THRESHOLD_DP, threshold)
                    .putExtra(EXTRA_LOG_LEVEL, level)
                    .putExtra(EXTRA_HAPTIC_ENABLED, haptic)
                    .putExtra(EXTRA_BREAK_OPEN_ENABLED, breakOpen)
                    .putExtra(EXTRA_CONFIG_PERSISTED, persisted)
                    .putExtra(EXTRA_PATTERN, safeString(intent.getStringExtra(EXTRA_PATTERN)))
                    .putExtra(EXTRA_DETAIL, safeString(intent.getStringExtra(EXTRA_DETAIL)))
                    .putExtra(EXTRA_NATIVE_LOG, safeString(intent.getStringExtra(EXTRA_NATIVE_LOG)))
                    .putExtra(EXTRA_CHANNEL_STAGE, "NATIVE_REPLY_RELAYED")
                    .putExtra(EXTRA_SENDER_UID, Process.myUid());
            if (persisted) cachedReply = new Intent(reply);
            relay(context, reply);
        }
    };

    private void relay(Context context, Intent reply) {
        try {
            context.sendBroadcast(reply, null, shareIdentityOptions());
        } catch (Throwable t) {
            log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                    "Runtime reply relay failed", t);
        }
    }

    private void sendAppStage(Context context, long nonce, String stage) {
        relay(context, new Intent(ACTION_APP_REPLY).setPackage(MODULE_PACKAGE)
                .putExtra(EXTRA_NONCE, nonce)
                .putExtra(EXTRA_HOOK_STATE, 0)
                .putExtra(EXTRA_SYSTEMUI_MODULE_VERSION, BuildConfig.VERSION_CODE)
                .putExtra(EXTRA_CHANNEL_STAGE, stage)
                .putExtra(EXTRA_SENDER_UID, Process.myUid()));
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
            if (packages != null) {
                for (String candidate : packages) {
                    if (packageName.equals(candidate)) return true;
                }
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
