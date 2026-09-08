package android.content.pm;
import java.util.HashMap; import java.util.Map;
public class PackageManager {
 public static final Map<Integer,String[]> OWNERS = new HashMap<>();
 public String[] getPackagesForUid(int uid) { return OWNERS.get(uid); }
}
