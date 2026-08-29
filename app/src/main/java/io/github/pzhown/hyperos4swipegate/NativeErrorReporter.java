package io.github.pzhown.hyperos4swipegate;

import android.os.Process;

import androidx.annotation.NonNull;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

/**
 * Independent Native ERROR relay. App log settings must never suppress LSPosed error evidence.
 */
public final class NativeErrorReporter extends XposedModule {
    private static final String TARGET_PROCESS = "com.miui.home";
    private static final String ERROR_FILE_NAME = "hyperos4swipegate_lsp_error.log";
    private static final int ANDROID_USER_OFFSET = 100000;
    private static final int MAX_ERROR_BYTES = 128 * 1024;
    private static final long POLL_INTERVAL_MS = 500L;

    private final List<File> errorFiles = new ArrayList<>();
    private final AtomicBoolean started = new AtomicBoolean(false);

    @Override
    public void onModuleLoaded(@NonNull XposedModuleInterface.ModuleLoadedParam param) {
        if (!TARGET_PROCESS.equals(param.getProcessName())) return;

        int userId = Process.myUid() / ANDROID_USER_OFFSET;
        addDataDir("/data/user_de/" + userId + "/com.miui.home");
        addDataDir("/data/user/" + userId + "/com.miui.home");
        if (userId == 0) addDataDir("/data/data/com.miui.home");
        startReporter();
    }

    private void addDataDir(String dataDir) {
        File file = new File(new File(dataDir, "cache"), ERROR_FILE_NAME);
        if (!errorFiles.contains(file)) errorFiles.add(file);
    }

    private void startReporter() {
        if (!started.compareAndSet(false, true)) return;
        Thread worker = new Thread(this::reportLoop, "SwipeGateNativeErrorReporter");
        worker.setDaemon(true);
        worker.start();
    }

    private void reportLoop() {
        while (true) {
            try {
                for (File source : errorFiles) {
                    if (source.isFile() && source.canRead()) drain(source);
                }
                Thread.sleep(POLL_INTERVAL_MS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                return;
            } catch (Throwable t) {
                log(android.util.Log.ERROR, "HyperOS4SwipeGateNative",
                        "Native error relay failed", t);
                try {
                    Thread.sleep(POLL_INTERVAL_MS);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }

    private void drain(File source) throws Exception {
        File drain = new File(source.getParentFile(), source.getName() + ".drain");
        if (drain.isFile()) drain.delete();

        File input = source;
        boolean renamed = source.renameTo(drain);
        if (renamed) input = drain;

        byte[] bytes;
        try (RandomAccessFile file = new RandomAccessFile(input, "r")) {
            long length = file.length();
            long start = Math.max(0L, length - MAX_ERROR_BYTES);
            file.seek(start);
            bytes = new byte[(int) (length - start)];
            file.readFully(bytes);
        }

        String text = new String(bytes, StandardCharsets.UTF_8);
        for (String line : text.split("\\R")) {
            if (!line.isBlank()) {
                // XposedModule.log writes to the LSPosed module log and is intentionally not tied
                // to the SwipeGate App's local log recording preference.
                log(android.util.Log.ERROR, "HyperOS4SwipeGateNative", line);
            }
        }

        if (renamed) {
            drain.delete();
        } else {
            try (FileOutputStream ignored = new FileOutputStream(source, false)) {
                // Fallback when atomic rename is unavailable: truncate only after successful relay.
            }
        }
    }
}
