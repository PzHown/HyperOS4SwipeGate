package android.content;
import java.util.HashMap;
import java.util.Map;
public class Intent {
 private final String action;
 private String targetPackage;
 private final Map<String,Object> extras = new HashMap<>();
 public Intent(String action) { this.action = action; }
 public Intent(Intent source) { action = source.action; targetPackage = source.targetPackage; extras.putAll(source.extras); }
 public String getAction() { return action; }
 public Intent setPackage(String value) { targetPackage = value; return this; }
 public String getPackage() { return targetPackage; }
 public Intent putExtra(String key, int value) { extras.put(key,value); return this; }
 public Intent putExtra(String key, long value) { extras.put(key,value); return this; }
 public Intent putExtra(String key, boolean value) { extras.put(key,value); return this; }
 public Intent putExtra(String key, String value) { extras.put(key,value); return this; }
 public boolean hasExtra(String key) { return extras.containsKey(key); }
 public int getIntExtra(String key,int fallback) { Object value=extras.get(key); return value instanceof Integer ? (Integer)value : fallback; }
 public long getLongExtra(String key,long fallback) { Object value=extras.get(key); return value instanceof Long ? (Long)value : fallback; }
 public boolean getBooleanExtra(String key,boolean fallback) { Object value=extras.get(key); return value instanceof Boolean ? (Boolean)value : fallback; }
 public String getStringExtra(String key) { Object value=extras.get(key); return value instanceof String ? (String)value : null; }
}
