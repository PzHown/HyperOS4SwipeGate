package io.github.pzhown.hyperos4swipegate;

import android.app.Activity;
import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Process;
import android.os.SystemClock;
import java.lang.reflect.Method;

import static io.github.pzhown.hyperos4swipegate.SystemUiBridgeModule.*;

/** Deterministic transport/lifecycle simulation, not an Android/HyperRT integration test. */
public final class ControlChannelHostTest {
    private static int checks;
    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }
    private static Intent last(Context context, String action) {
        for (int i=context.sent.size()-1; i>=0; --i) {
            Intent intent=context.sent.get(i);
            if (action.equals(intent.getAction())) return intent;
        }
        throw new AssertionError("missing broadcast: " + action);
    }
    private static long count(Context context, String action) {
        return context.sent.stream().filter(i -> action.equals(i.getAction())).count();
    }
    private static void deliver(Context context, Intent intent, int uid, String packageName) {
        BroadcastReceiver.testSenderUid=uid;
        BroadcastReceiver.testSenderPackage=packageName;
        context.receivers.get(intent.getAction()).onReceive(context,intent);
    }
    private static Intent query(long nonce, int threshold) {
        return new Intent(ACTION_APP_QUERY).putExtra(EXTRA_NONCE,nonce)
                .putExtra(EXTRA_THRESHOLD_DP,threshold).putExtra(EXTRA_LOG_LEVEL,1)
                .putExtra(EXTRA_HAPTIC_ENABLED,true).putExtra(EXTRA_BREAK_OPEN_ENABLED,true);
    }
    private static Intent nativeReply(Intent carrier, boolean persisted) {
        return new Intent(ACTION_NATIVE_REPLY)
                .putExtra(EXTRA_NONCE,carrier.getLongExtra(EXTRA_NONCE,0L))
                .putExtra(EXTRA_CHALLENGE_HIGH,carrier.getLongExtra(EXTRA_CHALLENGE_HIGH,0L))
                .putExtra(EXTRA_CHALLENGE_LOW,carrier.getLongExtra(EXTRA_CHALLENGE_LOW,0L))
                .putExtra(EXTRA_THRESHOLD_DP,carrier.getIntExtra(EXTRA_THRESHOLD_DP,-1))
                .putExtra(EXTRA_LOG_LEVEL,carrier.getIntExtra(EXTRA_LOG_LEVEL,-1))
                .putExtra(EXTRA_HAPTIC_ENABLED,carrier.getBooleanExtra(EXTRA_HAPTIC_ENABLED,false))
                .putExtra(EXTRA_BREAK_OPEN_ENABLED,carrier.getBooleanExtra(EXTRA_BREAK_OPEN_ENABLED,false))
                .putExtra(EXTRA_HOOK_STATE,2).putExtra(EXTRA_CONFIG_PERSISTED,persisted)
                .putExtra(EXTRA_SENDER_UID,12001);
    }
    private static Intent appReply(Intent request, boolean persisted) {
        return new Intent(ACTION_APP_REPLY)
                .putExtra(EXTRA_NONCE,request.getLongExtra(EXTRA_NONCE,0L))
                .putExtra(EXTRA_THRESHOLD_DP,request.getIntExtra(EXTRA_THRESHOLD_DP,-1))
                .putExtra(EXTRA_LOG_LEVEL,request.getIntExtra(EXTRA_LOG_LEVEL,-1))
                .putExtra(EXTRA_HAPTIC_ENABLED,request.getBooleanExtra(EXTRA_HAPTIC_ENABLED,false))
                .putExtra(EXTRA_BREAK_OPEN_ENABLED,request.getBooleanExtra(EXTRA_BREAK_OPEN_ENABLED,false))
                .putExtra(EXTRA_CONFIG_PERSISTED,persisted).putExtra(EXTRA_HOOK_STATE,2)
                .putExtra(EXTRA_SENDER_UID,1000).putExtra(EXTRA_CHANNEL_STAGE,"NATIVE_REPLY_RELAYED");
    }
    private static void testRelay() throws Exception {
        Process.uid=1000;
        Context systemUi=new Context(SYSTEM_UI_PACKAGE);
        SystemUiBridgeModule module=new SystemUiBridgeModule();
        Method initialize=SystemUiBridgeModule.class.getDeclaredMethod("initialize",Context.class);
        initialize.setAccessible(true);
        initialize.invoke(module,systemUi);
        Intent request=query(111L,170);
        deliver(systemUi,request,12000,MODULE_PACKAGE);
        Intent first=last(systemUi,ACTION_HYOS_CARRIER);
        check(count(systemUi,ACTION_HYOS_CARRIER)==1,"initial request forwarded");
        check((first.getLongExtra(EXTRA_CHALLENGE_HIGH,0L)|first.getLongExtra(EXTRA_CHALLENGE_LOW,0L))!=0,
                "nonempty private challenge");
        Handler.advance(1500);
        deliver(systemUi,request,12000,MODULE_PACKAGE);
        check(count(systemUi,ACTION_HYOS_CARRIER)==2,"same nonce must be retried");
        check(last(systemUi,ACTION_HYOS_CARRIER).getLongExtra(EXTRA_CHALLENGE_HIGH,0L)
                ==first.getLongExtra(EXTRA_CHALLENGE_HIGH,0L),"retry preserves challenge");
        deliver(systemUi,query(111L,171),12000,MODULE_PACKAGE);
        check(count(systemUi,ACTION_HYOS_CARRIER)==2,"changed payload with same nonce rejected");
        int before=systemUi.sent.size();
        deliver(systemUi,nativeReply(first,true).putExtra(EXTRA_CHALLENGE_HIGH,0L)
                .putExtra(EXTRA_CHALLENGE_LOW,0L),-1,null);
        check(systemUi.sent.size()==before,"claimed UID and nonce alone cannot authenticate");
        deliver(systemUi,nativeReply(first,true),13000,"untrusted.package");
        check(systemUi.sent.size()==before,"unexpected actual sender rejected");
        deliver(systemUi,nativeReply(first,true),-1,null);
        Intent reply=last(systemUi,ACTION_APP_REPLY);
        check(reply.getBooleanExtra(EXTRA_CONFIG_PERSISTED,false),"durable native reply relayed");
        check(!reply.hasExtra(EXTRA_CHALLENGE_HIGH)&&!reply.hasExtra(EXTRA_CHALLENGE_LOW),
                "private capability never sent to App");
        long carriers=count(systemUi,ACTION_HYOS_CARRIER);
        Handler.advance(1500);
        deliver(systemUi,request,12000,MODULE_PACKAGE);
        check(count(systemUi,ACTION_HYOS_CARRIER)==carriers,"lost final reply recovered from bounded cache");
        check(last(systemUi,ACTION_APP_REPLY).getBooleanExtra(EXTRA_CONFIG_PERSISTED,false),"cached durable ACK");
        Intent secondRequest=query(112L,180);
        deliver(systemUi,secondRequest,12000,MODULE_PACKAGE);
        Intent second=last(systemUi,ACTION_HYOS_CARRIER);
        long beforeStale=count(systemUi,ACTION_HYOS_CARRIER);
        deliver(systemUi,request,12000,MODULE_PACKAGE);
        check(count(systemUi,ACTION_HYOS_CARRIER)==beforeStale,"out-of-order old edit cannot overwrite newer config");
        deliver(systemUi,nativeReply(second,false),-1,null);
        check(!last(systemUi,ACTION_APP_REPLY).getBooleanExtra(EXTRA_CONFIG_PERSISTED,true),"negative persistence ACK");
        carriers=count(systemUi,ACTION_HYOS_CARRIER);
        Handler.advance(1500);
        deliver(systemUi,secondRequest,12000,MODULE_PACKAGE);
        check(count(systemUi,ACTION_HYOS_CARRIER)==carriers+1,"failed persistence goes back to native on retry");
        Handler.advance(ControlRequest.LIFETIME_MS);
        before=systemUi.sent.size();
        deliver(systemUi,nativeReply(second,true),-1,null);
        check(systemUi.sent.size()==before,"expired capability rejected");
        deliver(systemUi,secondRequest,12000,MODULE_PACKAGE);
        Intent renewed=last(systemUi,ACTION_HYOS_CARRIER);
        check(renewed.getLongExtra(EXTRA_CHALLENGE_HIGH,0L)!=second.getLongExtra(EXTRA_CHALLENGE_HIGH,0L)
                ||renewed.getLongExtra(EXTRA_CHALLENGE_LOW,0L)!=second.getLongExtra(EXTRA_CHALLENGE_LOW,0L),
                "expired session rotates capability");
        before=systemUi.sent.size();
        deliver(systemUi,nativeReply(second,true),-1,null);
        check(systemUi.sent.size()==before,"old-session replay rejected");
    }
    private static void testAppLifecycle() {
        Process.uid=12000;
        Application app=new Application(MODULE_PACKAGE);
        ConfigBridge.localPreferences(app).edit().putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP,170)
                .putInt(ConfigBridge.PREF_KEY_LOG_LEVEL,1)
                .putBoolean(ConfigBridge.PREF_KEY_HAPTIC_ENABLED,true)
                .putBoolean(ConfigBridge.PREF_KEY_BREAK_OPEN_ENABLED,true).apply();
        NativeControlBridge.initialize(app);
        Activity activity=new Activity();
        app.lifecycle.onActivityStarted(activity);
        Intent first=last(app,ACTION_APP_QUERY);
        long firstNonce=first.getLongExtra(EXTRA_NONCE,0L);
        check(firstNonce>0L,"positive ordered App nonce");
        deliver(app,appReply(first,false),1000,SYSTEM_UI_PACKAGE);
        check(NativeControlBridge.hasPendingQuery(),"negative persistence ACK keeps retry active");
        check(!NativeControlBridge.isConfigurationPersisted(),"no false durable-success status");
        app.lifecycle.onActivityStopped(activity);
        Handler.advance(1500);
        check(last(app,ACTION_APP_QUERY).getLongExtra(EXTRA_NONCE,0L)==firstNonce,"App retry nonce stable");
        Handler.advance(4500);
        check(!NativeControlBridge.hasPendingQuery(),"background request expires");
        check(Handler.pendingCount()==0,"no perpetual background polling task");
        check("CHANNEL_ERROR".equals(NativeControlBridge.snapshot().state()),"timeout distinct from hook failure");
        int broadcasts=app.sent.size();
        Handler.advance(60_000);
        check(app.sent.size()==broadcasts,"background App sends nothing after deadline");
        app.lifecycle.onActivityStarted(activity);
        Intent second=last(app,ACTION_APP_QUERY);
        check(second.getLongExtra(EXTRA_NONCE,0L)>firstNonce,"foreground recovery uses fresh nonce");
        deliver(app,appReply(second,true),13000,"untrusted.package");
        check(NativeControlBridge.hasPendingQuery(),"App rejects forged SystemUI reply");
        deliver(app,appReply(first,true),1000,SYSTEM_UI_PACKAGE);
        check(NativeControlBridge.hasPendingQuery(),"late previous-nonce reply ignored");
        deliver(app,appReply(second,true),1000,SYSTEM_UI_PACKAGE);
        check(NativeControlBridge.isConfigurationPersisted(),"durable confirmation recorded");
        check(!NativeControlBridge.hasPendingQuery(),"successful request completed");
        app.lifecycle.onActivityStopped(activity);
        check(Handler.pendingCount()==0,"idle background needs no heartbeat");
        ConfigBridge.localPreferences(app).edit().putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP,210).apply();
        NativeControlBridge.requestConfigRefresh();
        Intent updated=last(app,ACTION_APP_QUERY);
        check(updated.getIntExtra(EXTRA_THRESHOLD_DP,0)==210,"new settings captured in immutable payload");
        check(!NativeControlBridge.isConfigurationPersisted(),"edit clears old durability status");
        deliver(app,appReply(updated,true),1000,SYSTEM_UI_PACKAGE);
        check(Handler.pendingCount()==0,"background one-shot update ends immediately on ACK");
    }
    public static void main(String[] arguments) throws Exception {
        PackageManager.OWNERS.put(1000,new String[]{SYSTEM_UI_PACKAGE});
        PackageManager.OWNERS.put(12000,new String[]{MODULE_PACKAGE});
        PackageManager.OWNERS.put(12001,new String[]{LAUNCHER_PACKAGE});
        testRelay();
        testAppLifecycle();
        System.out.println("PASS: "+checks+" Java transport/lifecycle assertions (host simulation)");
    }
}
