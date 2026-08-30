from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    s = p.read_text()
    n = s.count(old)
    if n != 1:
        raise SystemExit(f'{label}: expected 1 match, got {n}')
    p.write_text(s.replace(old, new, 1))


replace_once('native/include/control_channel.h',
'''int swipegate_control_haptic_enabled();\n''',
'''int swipegate_control_haptic_enabled();
// Request one haptic pulse through the authenticated HyOS Runtime -> SystemUI bridge.
// kind: 0 = threshold/ready tick, 1 = committed back action. Returns 1 when queued.
int swipegate_control_request_haptic(int kind);
''', 'control header')

replace_once('native/src/control_channel.cpp',
'''constexpr const char *kNativeReplyAction =
        "io.github.pzhown.hyperos4swipegate.action.NATIVE_RUNTIME_REPLY";
''',
'''constexpr const char *kNativeReplyAction =
        "io.github.pzhown.hyperos4swipegate.action.NATIVE_RUNTIME_REPLY";
constexpr const char *kNativeHapticAction =
        "io.github.pzhown.hyperos4swipegate.action.NATIVE_HAPTIC_REQUEST";
''', 'native action')

replace_once('native/src/control_channel.cpp',
'''constexpr const char *kHapticEnabledExtra = "swipegate_haptic_enabled";
''',
'''constexpr const char *kHapticEnabledExtra = "swipegate_haptic_enabled";
constexpr const char *kHapticKindExtra = "swipegate_haptic_kind";
''', 'native extra')

transport = '''bool sendNativeHapticRequest(int32_t kind) {
    if (kind < 0 || kind > 1) return false;
    void *runtime = gCapturedRuntime.load(std::memory_order_acquire);
    if (reinterpret_cast<uintptr_t>(runtime) < 0x100000000ull) return false;

    const auto intentDefault = resolveLauncherSymbol<IntentDefaultFn>("Intent_default");
    const auto intentDrop = resolveLauncherSymbol<IntentDropFn>("Intent_drop");
    const auto setAction = resolveLauncherSymbol<IntentSetStringFn>("Intent_set_action");
    const auto setPackage = resolveLauncherSymbol<IntentSetStringFn>("Intent_set_package");
    const auto setExtras = resolveLauncherSymbol<IntentSetExtrasFn>("Intent_set_extras");
    const auto bundleDefault = resolveLauncherSymbol<BundleDefaultFn>("Bundle_default");
    const auto send = resolveLauncherSymbol<BroadcastSendFn>("Broadcast_send_broadcast");
    const auto inc = resolveLauncherSymbol<RuntimeStrongFn>("Runtime_inc_strong");
    const auto dec = resolveLauncherSymbol<RuntimeStrongFn>("Runtime_dec_strong");
    if (intentDefault == nullptr || intentDrop == nullptr || setAction == nullptr
            || setPackage == nullptr || setExtras == nullptr || bundleDefault == nullptr
            || send == nullptr || inc == nullptr || dec == nullptr || rStringVtable() == nullptr) {
        return false;
    }

    void *extras = bundleDefault();
    if (extras == nullptr
            || !addBundleBool(extras, kMarkerExtra, true)
            || !addBundleI32(extras, kHapticKindExtra, kind)
            || !addBundleI32(extras, kSenderUidExtra, static_cast<int32_t>(getuid()))) {
        return false;
    }

    void *intent = intentDefault();
    if (intent == nullptr) return false;
    ROptionRString action{};
    ROptionRString package{};
    if (!makeOwnedROptionString(kNativeHapticAction, &action)
            || !makeOwnedROptionString(kSystemUiPackage, &package)) {
        intentDrop(intent);
        return false;
    }
    setAction(intent, &action);
    setPackage(intent, &package);
    setExtras(intent, extras);

    inc(runtime);
    void *sharedRuntime = runtime;
    const NativeResult result = send(&sharedRuntime, intent);
    dec(runtime);
    intentDrop(intent);
    return isNativeSuccess(result);
}

'''
replace_once('native/src/control_channel.cpp',
'''void handleControlCarrier(void *intent) {
''', transport + '''void handleControlCarrier(void *intent) {
''', 'transport')

replace_once('native/src/control_channel.cpp',
'''extern "C" void swipegate_control_on_log(int, const char *text) {
''',
'''extern "C" int swipegate_control_request_haptic(int kind) {
    return sendNativeHapticRequest(static_cast<int32_t>(kind)) ? 1 : 0;
}

extern "C" void swipegate_control_on_log(int, const char *text) {
''', 'native export')

replace_once('native/src/main.cpp',
'''bool performReturnHaptic(const char *stage, bool light) {
    if (swipegate_control_haptic_enabled() != 1) return false;
    const auto original = reinterpret_cast<HapticFeedbackFn>(gOriginalHapticFeedback.load(std::memory_order_acquire));
''',
'''bool performReturnHaptic(const char *stage, bool light) {
    if (swipegate_control_haptic_enabled() != 1) return false;
    if (swipegate_control_request_haptic(light ? 0 : 1) == 1) {
        logLine(ANDROID_LOG_INFO, "HAPTIC feedback stage=%s kind=systemui",
                stage == nullptr ? "unknown" : stage);
        return true;
    }
    const auto original = reinterpret_cast<HapticFeedbackFn>(gOriginalHapticFeedback.load(std::memory_order_acquire));
''', 'main preference')

java = 'app/src/main/java/io/github/pzhown/hyperos4swipegate/SystemUiBridgeModule.java'
replace_once(java, '''import android.os.Process;\n''',
'''import android.os.Process;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
''', 'java imports')
replace_once(java,
'''    static final String ACTION_NATIVE_REPLY = MODULE_PACKAGE + ".action.NATIVE_RUNTIME_REPLY";
''',
'''    static final String ACTION_NATIVE_REPLY = MODULE_PACKAGE + ".action.NATIVE_RUNTIME_REPLY";
    static final String ACTION_NATIVE_HAPTIC = MODULE_PACKAGE + ".action.NATIVE_HAPTIC_REQUEST";
''', 'java action')
replace_once(java,
'''    static final String EXTRA_HAPTIC_ENABLED = "swipegate_haptic_enabled";
''',
'''    static final String EXTRA_HAPTIC_ENABLED = "swipegate_haptic_enabled";
    static final String EXTRA_HAPTIC_KIND = "swipegate_haptic_kind";
''', 'java extra')
replace_once(java,
'''        IntentFilter nativeReply = new IntentFilter(ACTION_NATIVE_REPLY);
        context.registerReceiver(nativeReplyReceiver, nativeReply, Context.RECEIVER_EXPORTED);
''',
'''        IntentFilter nativeReply = new IntentFilter(ACTION_NATIVE_REPLY);
        context.registerReceiver(nativeReplyReceiver, nativeReply, Context.RECEIVER_EXPORTED);

        IntentFilter nativeHaptic = new IntentFilter(ACTION_NATIVE_HAPTIC);
        context.registerReceiver(nativeHapticReceiver, nativeHaptic, Context.RECEIVER_EXPORTED);
''', 'java register')

receiver = '''    private final BroadcastReceiver nativeHapticReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!ACTION_NATIVE_HAPTIC.equals(intent.getAction())) return;
            final int senderUid = getSentFromUid();
            final int claimedLauncherUid = intent.getIntExtra(EXTRA_SENDER_UID, -1);
            final boolean marker = intent.getBooleanExtra(EXTRA_MARKER, false);
            final int kind = intent.getIntExtra(EXTRA_HAPTIC_KIND, -1);
            if (!marker || kind < 0 || kind > 1
                    || senderUid == Process.INVALID_UID
                    || senderUid != claimedLauncherUid
                    || !isUidOwner(context, senderUid, LAUNCHER_PACKAGE)) {
                log(android.util.Log.WARN, "HyperOS4SwipeGateSystemUI",
                        "Rejected native haptic senderUid=" + senderUid
                                + " claimedUid=" + claimedLauncherUid + " kind=" + kind);
                return;
            }

            try {
                Vibrator vibrator = null;
                VibratorManager manager = (VibratorManager) context.getSystemService(
                        Context.VIBRATOR_MANAGER_SERVICE);
                if (manager != null) vibrator = manager.getDefaultVibrator();
                if (vibrator == null) {
                    vibrator = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
                }
                if (vibrator == null || !vibrator.hasVibrator()) {
                    log(android.util.Log.WARN, "HyperOS4SwipeGateSystemUI",
                            "Native haptic unavailable: vibrator missing kind=" + kind);
                    return;
                }
                final int effectId = kind == 0
                        ? VibrationEffect.EFFECT_TICK
                        : VibrationEffect.EFFECT_CLICK;
                vibrator.vibrate(VibrationEffect.createPredefined(effectId));
                log(android.util.Log.INFO, "HyperOS4SwipeGateSystemUI",
                        "Native haptic performed kind=" + kind + " effect=" + effectId);
            } catch (Throwable t) {
                log(android.util.Log.ERROR, "HyperOS4SwipeGateSystemUI",
                        "Native haptic execution failed kind=" + kind, t);
            }
        }
    };

'''
replace_once(java,
'''    private final BroadcastReceiver nativeReplyReceiver = new BroadcastReceiver() {
''', receiver + '''    private final BroadcastReceiver nativeReplyReceiver = new BroadcastReceiver() {
''', 'java receiver')
