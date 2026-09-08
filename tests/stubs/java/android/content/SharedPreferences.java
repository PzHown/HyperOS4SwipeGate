package android.content;
import java.util.HashMap; import java.util.Map;
public class SharedPreferences {
 private final Map<String,Object> values = new HashMap<>();
 public int getInt(String key,int fallback) { Object value=values.get(key); return value instanceof Integer ? (Integer)value : fallback; }
 public boolean getBoolean(String key,boolean fallback) { Object value=values.get(key); return value instanceof Boolean ? (Boolean)value : fallback; }
 public Editor edit() { return new Editor(); }
 public final class Editor {
  public Editor putInt(String key,int value) { values.put(key,value); return this; }
  public Editor putBoolean(String key,boolean value) { values.put(key,value); return this; }
  public void apply() {}
 }
}
