package android.app;
import android.os.Bundle;
public final class BroadcastOptions {
 public static BroadcastOptions makeBasic() { return new BroadcastOptions(); }
 public BroadcastOptions setShareIdentityEnabled(boolean enabled) { return this; }
 public Bundle toBundle() { return new Bundle(); }
}
