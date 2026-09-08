package android.os;
import java.util.PriorityQueue;
public final class Handler {
 private record Task(long when, long sequence, Runnable runnable) implements Comparable<Task> {
  public int compareTo(Task other) {
   int value = Long.compare(when, other.when);
   return value != 0 ? value : Long.compare(sequence, other.sequence);
  }
 }
 private static final PriorityQueue<Task> TASKS = new PriorityQueue<>();
 private static long sequence;
 public Handler(Looper looper) {}
 public boolean post(Runnable runnable) { return postDelayed(runnable, 0); }
 public boolean postDelayed(Runnable runnable, long delay) {
  TASKS.add(new Task(SystemClock.now + delay, sequence++, runnable)); return true;
 }
 public void removeCallbacks(Runnable runnable) { TASKS.removeIf(t -> t.runnable == runnable); }
 public static int pendingCount() { return TASKS.size(); }
 public static void advance(long delta) {
  long end = SystemClock.now + delta; int budget = 10000;
  while (!TASKS.isEmpty() && TASKS.peek().when <= end) {
   if (--budget == 0) throw new AssertionError("unbounded scheduling");
   Task task = TASKS.remove(); SystemClock.now = task.when; task.runnable.run();
  }
  SystemClock.now = end;
 }
}
