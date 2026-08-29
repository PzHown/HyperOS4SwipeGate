package io.github.pzhown.hyperos4swipegate;

import android.content.SharedPreferences;
import android.os.Process;

import androidx.annotation.NonNull;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

/**
 * Modern libxposed Java entry used as a rootless configuration and diagnostics bridge.
 * The actual gesture hook remains in native_init.
 */
public final class ModuleMain extends XposedModule {
    private static final String TARGET_PROCESS = "com.miui.home";
    private static final String NATIVE_LOG_NAME = "hyperos4swipegate_native.log";
    private static final int ANDROID_USER_OFFSET = 100000;
    private static final int MAX_STREAM_LOG_BYTES = 96 * 1024;
    private static final int MAX_SOURCE_LOG_BYTES = 512 * 1024;
    private static final int RETAIN_SOURCE_LOG_BYTES = 256 * 1024;
    private static final long LOG_MIRROR_INTERVAL_MS = 1000L;
    private static final long HOOK_STATUS_HEARTBEAT_MS = 1500L;

    private SharedPreferences remotePreferences;
    private SharedPreferences.OnSharedPreferenceChangeListener preferenceListener;
    private final List<File> configFiles = new ArrayList<>();
    private final List<File> logLevelFiles = new ArrayList<>();
    private final List<File> nativeLogFiles = new ArrayList<>();
    private final AtomicBoolean logMirrorStarted = new AtomicBoolean(false);

    private volatile int diagnosticsPort;
    private volatile String diagnosticsToken = "";
    private volatile int currentLogLevel = ConfigBridge.DEFAULT_LOG_LEVEL;
    private volatile long lastMirroredLength = -1L;
    private volatile long lastMirroredModified = -1L;
    private volatile String lastMirroredPath = "";
    private volatile String lastMirrorError = "";

    private volatile String hookState = "UNKNOWN";
    private volatile String hookPattern = "";
    private volatile String hookDetail = "等待 Native Hook 状态";
    private volatile String lastHookStatusPayload = "";
    private volatile long lastHookStatusSentAtMs;

    @Override
    public void onModuleLoaded(@NonNull XposedModuleInterface.ModuleLoadedParam param) {
        if (!TARGET_PROCESS.equals(param.getProcessName())) return;

        // HyperOS 4 Launcher is a hyos_spawner/Rust process. Do not depend on
        // onPackageLoaded being delivered: initialize as soon as the module
        // generation itself is attached to com.miui.home.
        final int userId = Process.myUid() / ANDROID_USER_OFFSET;
        addDataDir("/data/user_de/" + userId + "/com.miui.home");
        addDataDir("/data/user/" + userId + "/com.miui.home");
        if (userId == 0) addDataDir("/data/data/com.miui.home");

        try {
            remotePreferences = getRemotePreferences(ConfigBridge.REMOTE_PREF_GROUP);
            preferenceListener = (preferences, key) -> {
                if (ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP.equals(key)) {
                    publishThreshold(preferences);
                }
                if (ConfigBridge.REMOTE_PREF_KEY_LOG_LEVEL.equals(key)) {
                    publishLogLevel(preferences);
                }
                if (ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_PORT.equals(key)
                        || ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_TOKEN.equals(key)) {
                    updateDiagnosticsEndpoint(preferences);
                }
            };
            remotePreferences.registerOnSharedPreferenceChangeListener(preferenceListener);
            if (remotePreferences.contains(ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP)) {
                publishThreshold(remotePreferences);
            }
            publishLogLevel(remotePreferences);
            updateDiagnosticsEndpoint(remotePreferences);
            startNativeLogMirror();
            log(android.util.Log.INFO, "HyperOS4SwipeGateJava",
                    "Remote bridge ready user=" + userId
                            + " configFiles=" + configFiles.size()
                            + " logLevel=" + currentLogLevel
                            + " nativeLogFiles=" + nativeLogFiles.size());
        } catch (Throwable t) {
            log(android.util.Log.ERROR, "HyperOS4SwipeGateJava",
                    "Remote bridge failed", t);
        }
    }

    private void addDataDir(String dataDir) {
        File cacheDir = new File(dataDir, "cache");
        File configFile = new File(cacheDir, ConfigBridge.NATIVE_CONFIG_FILE);
        if (!configFiles.contains(configFile)) configFiles.add(configFile);

        File logLevelFile = new File(cacheDir, ConfigBridge.NATIVE_LOG_LEVEL_FILE);
        if (!logLevelFiles.contains(logLevelFile)) logLevelFiles.add(logLevelFile);

        File nativeLogFile = new File(cacheDir, NATIVE_LOG_NAME);
        if (!nativeLogFiles.contains(nativeLogFile)) nativeLogFiles.add(nativeLogFile);
    }

    private void publishThreshold(SharedPreferences preferences) {
        int thresholdDp = preferences.getInt(
                ConfigBridge.REMOTE_PREF_KEY_THRESHOLD_DP,
                ConfigBridge.DEFAULT_THRESHOLD_DP);
        thresholdDp = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, thresholdDp));
        int successes = writeValueFiles(configFiles, thresholdDp);

        log(android.util.Log.INFO, "HyperOS4SwipeGateJava",
                "Published threshold=" + thresholdDp + "dp files=" + successes);
    }

    private void publishLogLevel(SharedPreferences preferences) {
        int requestedLogLevel = preferences.getInt(
                ConfigBridge.REMOTE_PREF_KEY_LOG_LEVEL,
                ConfigBridge.DEFAULT_LOG_LEVEL);
        int logLevel = ConfigBridge.sanitizeLogLevel(requestedLogLevel);
        currentLogLevel = logLevel;
        int successes = writeValueFiles(logLevelFiles, logLevel);

        // Do not purge here. The worker first consumes Native hook state from the transient file,
        // then removes it when user-facing logging is disabled.
        resetMirrorSnapshot();

        log(android.util.Log.INFO, "HyperOS4SwipeGateJava",
                "Published logLevel=" + logLevel + " files=" + successes);
    }

    private int writeValueFiles(List<File> files, int value) {
        byte[] bytes = (Integer.toString(value) + "\n").getBytes(StandardCharsets.US_ASCII);
        int successes = 0;
        for (File file : files) {
            File parent = file.getParentFile();
            if (parent == null || (!parent.exists() && !parent.mkdirs())) continue;
            File temporary = new File(parent, file.getName() + ".tmp");
            try (FileOutputStream output = new FileOutputStream(temporary, false)) {
                output.write(bytes);
                output.flush();
                try {
                    output.getFD().sync();
                } catch (Throwable ignored) {
                }
            } catch (Throwable ignored) {
                continue;
            }

            if (!temporary.renameTo(file)) {
                try (FileOutputStream output = new FileOutputStream(file, false)) {
                    output.write(bytes);
                    output.flush();
                } catch (Throwable ignored) {
                    temporary.delete();
                    continue;
                }
                temporary.delete();
            }
            successes++;
        }
        return successes;
    }

    private void updateDiagnosticsEndpoint(SharedPreferences preferences) {
        int nextPort = preferences.getInt(ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_PORT, 0);
        String nextToken = preferences.getString(
                ConfigBridge.REMOTE_PREF_KEY_DIAGNOSTICS_TOKEN, "");
        if (nextToken == null) nextToken = "";
        if (nextPort == diagnosticsPort && nextToken.equals(diagnosticsToken)) return;

        diagnosticsPort = nextPort;
        diagnosticsToken = nextToken;
        resetMirrorSnapshot();
        lastHookStatusPayload = "";
        lastHookStatusSentAtMs = 0L;
    }

    private void resetMirrorSnapshot() {
        lastMirroredLength = -1L;
        lastMirroredModified = -1L;
        lastMirroredPath = "";
        lastMirrorError = "";
    }

    private void startNativeLogMirror() {
        if (!logMirrorStarted.compareAndSet(false, true)) return;
        Thread worker = new Thread(this::nativeLogMirrorLoop, "SwipeGateLogMirror");
        worker.setDaemon(true);
        worker.start();
    }

    private void nativeLogMirrorLoop() {
        while (true) {
            try {
                File source = findNativeLogFile();
                if (source != null) {
                    trimNativeLogIfNeeded(source);
                    updateHookStatusFromLog(source);
                }

                sendHookStatusIfNeeded();

                if (currentLogLevel <= ConfigBridge.LOG_LEVEL_OFF) {
                    // Hook readiness is cached in memory and sent as a tiny heartbeat. Keep the
                    // existing "logging off" promise by deleting the transient source afterwards.
                    purgeNativeLogs();
                    resetMirrorSnapshot();
                    Thread.sleep(LOG_MIRROR_INTERVAL_MS);
                    continue;
                }

                if (source != null && diagnosticsPort > 0 && !diagnosticsToken.isBlank()) {
                    long length = source.length();
                    long modified = source.lastModified();
                    String path = source.getAbsolutePath();
                    if (length != lastMirroredLength
                            || modified != lastMirroredModified
                            || !path.equals(lastMirroredPath)) {
                        try {
                            sendNativeLog(source, currentLogLevel);
                        } catch (Throwable t) {
                            String message = t.getClass().getSimpleName() + ": "
                                    + (t.getMessage() == null ? "" : t.getMessage());
                            if (!message.equals(lastMirrorError)) {
                                lastMirrorError = message;
                                log(android.util.Log.WARN, "HyperOS4SwipeGateJava",
                                        "Native log stream failed: " + message);
                            }
                        } finally {
                            // Do not reconnect to a stale app endpoint every second. A log change
                            // or a newly published endpoint will make the snapshot eligible again.
                            lastMirroredLength = source.length();
                            lastMirroredModified = source.lastModified();
                            lastMirroredPath = path;
                        }
                    }
                }
                Thread.sleep(LOG_MIRROR_INTERVAL_MS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                return;
            } catch (Throwable t) {
                String message = t.getClass().getSimpleName() + ": "
                        + (t.getMessage() == null ? "" : t.getMessage());
                if (!message.equals(lastMirrorError)) {
                    lastMirrorError = message;
                    log(android.util.Log.WARN, "HyperOS4SwipeGateJava",
                            "Native log worker failed: " + message);
                }
                try {
                    Thread.sleep(LOG_MIRROR_INTERVAL_MS);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }

    private void updateHookStatusFromLog(File source) throws Exception {
        byte[] bytes;
        try (RandomAccessFile input = new RandomAccessFile(source, "r")) {
            long length = input.length();
            long start = Math.max(0L, length - MAX_STREAM_LOG_BYTES);
            input.seek(start);
            bytes = new byte[(int) (length - start)];
            input.readFully(bytes);
        }

        String text = new String(bytes, StandardCharsets.UTF_8);
        String[] lines = text.split("\\R");
        for (String line : lines) {
            if (line.isBlank()) continue;
            applyHookStatusLine(line);
        }
    }

    private void applyHookStatusLine(String line) {
        if (line.contains("DP_GATE native_init accepted")) {
            setHookStatus("WAITING", "", "Native 模块已加载，等待目标库与 Hook");
            return;
        }
        if (line.contains("HOOK_HEALTH launcher mapping changed")) {
            setHookStatus("WAITING", hookPattern, "系统桌面映射已变化，正在重新定位目标函数");
            return;
        }
        if (line.contains("HOOK_HEALTH libapp_launcher.so absent")) {
            setHookStatus("WAITING", hookPattern, "等待系统桌面目标库 libapp_launcher.so");
            return;
        }
        if (line.contains("HOOK_SCAN resolved")) {
            String pattern = extractField(line, "pattern=");
            setHookStatus("WAITING", pattern, "已定位目标函数，等待安装 Hook");
            return;
        }
        if (line.contains("HOOK_HEALTH original bytes restored")
                || line.contains("starting unhook+rehook repair")
                || line.contains("HOOK_HEALTH repair deferred")) {
            setHookStatus("REPAIRING", extractPatternOrCurrent(line), "Hook 被恢复，正在修复");
            return;
        }
        if (line.contains("DP_GATE hook installed")
                || line.contains("HOOK_HEALTH healthy ")
                || line.contains("HOOK_HEALTH repaired successfully")
                || line.contains("DP_GATE rawDx=")) {
            String pattern = extractPatternOrCurrent(line);
            setHookStatus("HEALTHY", pattern, "目标函数 Hook 健康");
            return;
        }
        if (line.contains("HOOK_SCAN install refused")) {
            setHookStatus("FAILED", hookPattern, "未匹配到唯一的 on_swipe_process 目标函数");
            return;
        }
        if (line.contains("HOOK_SCAN pattern changed before hook")) {
            setHookStatus("FAILED", extractPatternOrCurrent(line), "目标函数特征在安装前发生变化");
            return;
        }
        if (line.contains("DP_GATE hook_func failed")) {
            setHookStatus("FAILED", extractPatternOrCurrent(line), "LSPosed hook_func 安装失败");
            return;
        }
        if (line.contains("hook_func returned success but entry is not patched")) {
            setHookStatus("FAILED", hookPattern, "Hook 返回成功，但目标函数入口未被修改");
            return;
        }
        if (line.contains("HOOK_HEALTH foreign patch detected")) {
            setHookStatus("FAILED", extractPatternOrCurrent(line), "目标函数被其他 Hook 修改，已拒绝不安全修复");
            return;
        }
        if (line.contains("HOOK_HEALTH repair unavailable")
                || line.contains("HOOK_HEALTH repair failed")
                || line.contains("HOOK_HEALTH repair aborted")) {
            setHookStatus("FAILED", extractPatternOrCurrent(line), "Native Hook 自动修复失败");
        }
    }

    private String extractPatternOrCurrent(String line) {
        String pattern = extractField(line, "pattern=");
        return pattern.isBlank() ? hookPattern : pattern;
    }

    private String extractField(String line, String key) {
        int start = line.indexOf(key);
        if (start < 0) return "";
        start += key.length();
        int end = line.indexOf(' ', start);
        if (end < 0) end = line.length();
        if (start >= end) return "";
        return line.substring(start, end).trim();
    }

    private void setHookStatus(String state, String pattern, String detail) {
        hookState = state == null || state.isBlank() ? "UNKNOWN" : state;
        if (pattern != null && !pattern.isBlank() && !"<none>".equals(pattern)) {
            hookPattern = pattern;
        }
        hookDetail = detail == null ? "" : detail;
    }

    private void sendHookStatusIfNeeded() throws Exception {
        if (diagnosticsPort <= 0 || diagnosticsToken.isBlank()) return;

        String payload = DiagnosticsStreamBridge.HOOK_STATUS_PREFIX
                + "\t" + sanitizeStatusField(hookState)
                + "\t" + sanitizeStatusField(hookPattern)
                + "\t" + sanitizeStatusField(hookDetail);
        long now = android.os.SystemClock.elapsedRealtime();
        if (payload.equals(lastHookStatusPayload)
                && now - lastHookStatusSentAtMs < HOOK_STATUS_HEARTBEAT_MS) {
            return;
        }

        sendPayload(payload.getBytes(StandardCharsets.UTF_8));
        lastHookStatusPayload = payload;
        lastHookStatusSentAtMs = now;
    }

    private String sanitizeStatusField(String value) {
        if (value == null) return "";
        return value.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim();
    }

    private void purgeNativeLogs() {
        for (File file : nativeLogFiles) {
            try {
                if (file.isFile()) file.delete();
            } catch (Throwable ignored) {
            }
        }
    }

    private File findNativeLogFile() {
        for (File file : nativeLogFiles) {
            if (file.isFile() && file.canRead()) return file;
        }
        return null;
    }

    private void trimNativeLogIfNeeded(File source) throws Exception {
        long length = source.length();
        if (length <= MAX_SOURCE_LOG_BYTES) return;

        long start = Math.max(0L, length - RETAIN_SOURCE_LOG_BYTES);
        byte[] tail;
        try (RandomAccessFile input = new RandomAccessFile(source, "r")) {
            input.seek(start);
            tail = new byte[(int) (length - start)];
            input.readFully(tail);
        }

        int offset = 0;
        if (start > 0L) {
            while (offset < tail.length && tail[offset] != '\n') offset++;
            if (offset < tail.length) offset++;
        }

        try (FileOutputStream output = new FileOutputStream(source, false)) {
            output.write("[... older native log trimmed ...]\n"
                    .getBytes(StandardCharsets.UTF_8));
            if (offset < tail.length) {
                output.write(tail, offset, tail.length - offset);
            }
            output.flush();
        }
        resetMirrorSnapshot();
    }

    private void sendNativeLog(File source, int logLevel) throws Exception {
        byte[] bytes;
        boolean truncated;
        try (RandomAccessFile input = new RandomAccessFile(source, "r")) {
            long length = input.length();
            long start = Math.max(0L, length - MAX_STREAM_LOG_BYTES);
            truncated = start > 0L;
            input.seek(start);
            bytes = new byte[(int) (length - start)];
            input.readFully(bytes);
        }

        int offset = 0;
        if (truncated) {
            while (offset < bytes.length && bytes[offset] != '\n') offset++;
            if (offset < bytes.length) offset++;
        }

        String text = offset < bytes.length
                ? new String(bytes, offset, bytes.length - offset, StandardCharsets.UTF_8)
                : "";
        if (logLevel == ConfigBridge.LOG_LEVEL_COMPACT) {
            text = compactLog(text);
        }
        if (text.isBlank()) return;

        ByteArrayOutputStream payload = new ByteArrayOutputStream();
        if (truncated) {
            payload.write("[... earlier native log omitted ...]\n"
                    .getBytes(StandardCharsets.UTF_8));
        }
        payload.write(text.getBytes(StandardCharsets.UTF_8));
        sendPayload(payload.toByteArray());
        lastMirrorError = "";
    }

    private void sendPayload(byte[] data) throws Exception {
        try (Socket socket = new Socket()) {
            socket.connect(
                    new InetSocketAddress(InetAddress.getLoopbackAddress(), diagnosticsPort),
                    1000);
            socket.setSoTimeout(1000);
            try (DataOutputStream output = new DataOutputStream(socket.getOutputStream())) {
                output.writeUTF(diagnosticsToken);
                output.writeInt(data.length);
                output.write(data);
                output.flush();
            }
        }
    }

    private String compactLog(String text) {
        StringBuilder out = new StringBuilder(text.length());
        String[] lines = text.split("\\R");
        for (String line : lines) {
            if (line.startsWith("DP_GATE rawDx=")
                    || line.startsWith("HOOK_HEALTH healthy ")) {
                continue;
            }
            if (!line.isBlank()) out.append(line).append('\n');
        }
        return out.toString();
    }
}
