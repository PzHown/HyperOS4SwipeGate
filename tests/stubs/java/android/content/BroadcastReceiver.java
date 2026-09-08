package android.content;
public abstract class BroadcastReceiver {
 public static int testSenderUid = -1;
 public static String testSenderPackage;
 public int getSentFromUid() { return testSenderUid; }
 public String getSentFromPackage() { return testSenderPackage; }
 public abstract void onReceive(Context context, Intent intent);
}
