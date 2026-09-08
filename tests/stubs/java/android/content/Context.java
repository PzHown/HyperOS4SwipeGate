package android.content;
import android.content.pm.PackageManager; import android.os.Bundle;
import java.util.ArrayList; import java.util.HashMap; import java.util.List; import java.util.Map;
public class Context {
 public static final int MODE_PRIVATE=0, RECEIVER_EXPORTED=2;
 public String packageName;
 public final List<Intent> sent = new ArrayList<>();
 public final Map<String,BroadcastReceiver> receivers = new HashMap<>();
 private final SharedPreferences preferences = new SharedPreferences();
 public Context(String packageName) { this.packageName=packageName; }
 public Context getApplicationContext() { return this; }
 public String getPackageName() { return packageName; }
 public PackageManager getPackageManager() { return new PackageManager(); }
 public SharedPreferences getSharedPreferences(String name,int mode) { return preferences; }
 public Intent registerReceiver(BroadcastReceiver receiver,IntentFilter filter,int flags) { receivers.put(filter.action,receiver); return null; }
 public void sendBroadcast(Intent intent,String permission,Bundle options) { sent.add(new Intent(intent)); }
}
