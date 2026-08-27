package io.github.pzhown.hyperos4swipegate;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class LogFragment extends Fragment {
    private static final long DIAGNOSTIC_TIMEOUT_MS = 20_000L;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private TextView logText;
    private Button refreshButton;
    private Button copyButton;

    public LogFragment() {
        super(R.layout.fragment_logs);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        logText = view.findViewById(R.id.log_text);
        refreshButton = view.findViewById(R.id.log_refresh);
        copyButton = view.findViewById(R.id.log_copy);
        refreshButton.setOnClickListener(v -> refreshLogs());
        copyButton.setOnClickListener(v -> copyLogs());
        refreshLogs();
    }

    @Override
    public void onDestroyView() {
        logText = null;
        refreshButton = null;
        copyButton = null;
        super.onDestroyView();
    }

    @Override
    public void onDestroy() {
        executor.shutdownNow();
        super.onDestroy();
    }

    private void refreshLogs() {
        if (refreshButton != null) refreshButton.setEnabled(false);
        if (logText != null) logText.setText(R.string.log_loading);
        executor.execute(() -> {
            String result = collectDiagnostics();
            if (!isAdded()) return;
            requireActivity().runOnUiThread(() -> {
                if (logText != null) logText.setText(result);
                if (refreshButton != null) refreshButton.setEnabled(true);
                if (copyButton != null) copyButton.setEnabled(!result.isEmpty());
            });
        });
    }

    private void copyLogs() {
        if (logText == null) return;
        ClipboardManager clipboard = (ClipboardManager) requireContext()
                .getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText(
                "HyperOS4 SwipeGate logs", logText.getText()));
        Toast.makeText(requireContext(), R.string.log_copied, Toast.LENGTH_SHORT).show();
    }

    private String collectDiagnostics() {
        StringBuilder out = new StringBuilder();
        out.append("=== HyperOS4 SwipeGate diagnostics ===\n");
        out.append("appVersion=").append(BuildConfig.VERSION_NAME).append('\n');
        out.append("device=").append(Build.MANUFACTURER).append(' ')
                .append(Build.MODEL).append('\n');
        out.append("android=").append(Build.VERSION.RELEASE)
                .append(" sdk=").append(Build.VERSION.SDK_INT).append('\n');
        out.append("expectedLauncher=RELEASE-8.01.02.5459\n");
        out.append("expectedExe=/system_ext/bin/hyos_spawner\n\n");

        // 0x816fc4 = 2070 * 4096 + 0xfc4. The previous probe used
        // dd bs=1 skip=8482756, which makes BusyBox perform millions of
        // one-byte reads and looks like a hung log page. This probe extracts
        // libapp_launcher.so once, then jumps in 4 KiB blocks and performs
        // the final 0xfc4-byte adjustment only inside a tiny 12 KiB window.
        String script = ""
                + "echo '[root]'\n"
                + "id\n"
                + "echo\n"
                + "echo '[threshold px]'\n"
                + "getprop persist.hyperos4swipegate.threshold_px\n"
                + "echo\n"
                + "PID=$(pidof com.miui.home 2>/dev/null | awk '{print $1}')\n"
                + "echo '[launcher process]'\n"
                + "echo \"launcherPid=${PID:-<none>}\"\n"
                + "ps -A 2>/dev/null | grep -E 'com.miui.home|hyos_spawner' || true\n"
                + "echo\n"
                + "echo '[module mapping in launcher]'\n"
                + "if [ -n \"$PID\" ] && [ -r \"/proc/$PID/maps\" ]; then grep -F 'libhyperos4swipegate.so' \"/proc/$PID/maps\" || echo '<module so not mapped>'; else echo '<launcher maps unavailable>'; fi\n"
                + "echo\n"
                + "LAUNCHER_APK=$(pm path com.miui.home 2>/dev/null | head -n 1 | sed 's/^package://')\n"
                + "echo '[launcher apk]'\n"
                + "echo \"launcherApk=${LAUNCHER_APK:-<none>}\"\n"
                + "dumpsys package com.miui.home 2>/dev/null | grep -E 'versionCode=|versionName=' | head -n 4 || true\n"
                + "BB=/data/adb/magisk/busybox\n"
                + "if [ ! -x \"$BB\" ]; then BB=$(command -v busybox 2>/dev/null); fi\n"
                + "echo \"busybox=${BB:-<none>}\"\n"
                + "echo\n"
                + "echo '[direct exact-5459 code probe]'\n"
                + "TMP=/data/local/tmp/hyperos4swipegate_launcher_$$.so\n"
                + "rm -f \"$TMP\"\n"
                + "if [ -n \"$LAUNCHER_APK\" ] && [ -n \"$BB\" ]; then\n"
                + "  if \"$BB\" unzip -p \"$LAUNCHER_APK\" lib/arm64-v8a/libapp_launcher.so > \"$TMP\" 2>/dev/null; then\n"
                + "    SIZE=$(\"$BB\" wc -c < \"$TMP\")\n"
                + "    echo \"launcherSoBytes=$SIZE\"\n"
                + "    SIG=$(\"$BB\" dd if=\"$TMP\" bs=4096 skip=2070 count=3 2>/dev/null | \"$BB\" dd bs=1 skip=4036 count=16 2>/dev/null | \"$BB\" od -An -tx1 -v | tr -d ' \\n')\n"
                + "    echo \"offset=0x816fc4 signature=$SIG\"\n"
                + "    if [ \"$SIG\" = 'ff8305d1ea7b00fde9a30f6dfdfb10a9' ]; then\n"
                + "      echo 'signatureMatch=1'\n"
                + "      echo 'CODE_FILE_PROBE_BEGIN encoding=base64 offset=0x816fc4 bytes=8192'\n"
                + "      \"$BB\" dd if=\"$TMP\" bs=4096 skip=2070 count=3 2>/dev/null | \"$BB\" dd bs=1 skip=4036 count=8192 2>/dev/null | \"$BB\" base64 | \"$BB\" fold -w 512\n"
                + "      echo 'CODE_FILE_PROBE_END'\n"
                + "    else\n"
                + "      echo 'signatureMatch=0'\n"
                + "    fi\n"
                + "  else\n"
                + "    echo '<failed to extract libapp_launcher.so>'\n"
                + "  fi\n"
                + "else\n"
                + "  echo '<launcher APK extraction unavailable>'\n"
                + "fi\n"
                + "rm -f \"$TMP\"\n"
                + "echo\n"
                + "echo '[native file logs]'\n"
                + "FOUND=0\n"
                + "for f in /data/user_de/0/com.miui.home/cache/hyperos4swipegate_native.log /data/user/0/com.miui.home/cache/hyperos4swipegate_native.log /data/data/com.miui.home/cache/hyperos4swipegate_native.log; do if [ -r \"$f\" ]; then FOUND=1; echo \"--- $f ---\"; tail -n 120 \"$f\"; fi; done\n"
                + "if [ \"$FOUND\" = 0 ]; then echo '<no native log file found>'; fi\n"
                + "echo\n"
                + "echo '[logcat HyperOS4SwipeGateNative]'\n"
                + "logcat -d -v threadtime -s HyperOS4SwipeGateNative:* 2>/dev/null | tail -n 120\n";

        Process process = null;
        Thread watchdog = null;
        try {
            process = new ProcessBuilder("su", "-c", script)
                    .redirectErrorStream(true)
                    .start();
            final Process watchedProcess = process;
            watchdog = new Thread(() -> {
                try {
                    Thread.sleep(DIAGNOSTIC_TIMEOUT_MS);
                    if (watchedProcess.isAlive()) watchedProcess.destroyForcibly();
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                }
            }, "SwipeGateLogWatchdog");
            watchdog.start();

            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    out.append(line).append('\n');
                }
            }
            int exit = process.waitFor();
            if (watchdog != null) watchdog.interrupt();
            out.append("\n[su exit] ").append(exit).append('\n');
            if (exit != 0) {
                out.append("诊断命令未正常完成；20 秒 watchdog 会强制结束卡住的读取。\n");
            }
        } catch (Exception e) {
            if (process != null && process.isAlive()) process.destroyForcibly();
            if (watchdog != null) watchdog.interrupt();
            out.append("[su unavailable] ").append(e.getClass().getSimpleName())
                    .append(": ").append(e.getMessage()).append('\n');
        }
        return out.toString();
    }
}
