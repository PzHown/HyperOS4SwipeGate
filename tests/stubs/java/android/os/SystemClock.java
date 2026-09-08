package android.os;
public final class SystemClock {
 public static long now = 100L;
 public static long elapsedRealtime() { return now; }
 public static long elapsedRealtimeNanos() { return now * 1_000_000L; }
}
