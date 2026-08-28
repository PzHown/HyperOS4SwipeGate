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

        if (logLevel == ConfigBridge.LOG_LEVEL_OFF) {
            purgeNativeLogs();
        }
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
                if (currentLogLevel <= ConfigBridge.LOG_LEVEL_OFF) {
                    purgeNativeLogs();
                    Thread.sleep(LOG_MIRROR_INTERVAL_MS);
                    continue;
                }

                File source = findNativeLogFile();
                if (source != null) {
                    trimNativeLogIfNeeded(source);
                    if (diagnosticsPort > 0 && !diagnosticsToken.isBlank()) {
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
        byte[] data = payload.toByteArray();

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
        lastMirrorError = "";
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
