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
        CharSequence text = logText.getText();
        ClipboardManager clipboard = (ClipboardManager) requireContext()
                .getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("HyperOS4 SwipeGate logs", text));
        Toast.makeText(requireContext(), R.string.log_copied, Toast.LENGTH_SHORT).show();
    }

    private String collectDiagnostics() {
        StringBuilder out = new StringBuilder();
        out.append("=== HyperOS4 SwipeGate diagnostics ===\n");
        out.append("appVersion=").append(BuildConfig.VERSION_NAME).append('\n');
        out.append("device=").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        out.append("android=").append(Build.VERSION.RELEASE)
                .append(" sdk=").append(Build.VERSION.SDK_INT).append('\n');
        out.append("expectedLauncher=RELEASE-8.01.02.5459\n");
        out.append("expectedExe=/system_ext/bin/hyos_spawner\n\n");

        String script = ""
                + "echo '[root]'\n"
                + "id\n"
                + "echo\n"
                + "echo '[threshold px]'\n"
                + "getprop persist.hyperos4swipegate.threshold_px\n"
                + "echo\n"
                + "echo '[legacy threshold percent - ignored]'\n"
                + "getprop persist.hyperos4swipegate.threshold\n"
                + "echo\n"
                + "echo '[native display sources - reference only]'\n"
                + "for p in persist.sys.miui_resolution persist.sys.display-size vendor.display-size ro.boot.display_resolution; do v=$(getprop $p); [ -n \"$v\" ] && echo \"$p=$v\"; done\n"
                + "for f in /sys/class/graphics/fb0/virtual_size /sys/class/drm/card0-DSI-1/modes /sys/class/drm/card0-DSI-0/modes /sys/class/drm/card0-DSI-2/modes; do [ -r \"$f\" ] && echo \"$f=$(head -n 1 \"$f\")\"; done\n"
                + "echo\n"
                + "echo '[processes]'\n"
                + "ps -A 2>/dev/null | grep -E 'hyos_spawner|com.miui.home' || true\n"
                + "PID=$(pidof com.miui.home 2>/dev/null | awk '{print $1}')\n"
                + "echo \"launcherPid=${PID:-<none>}\"\n"
                + "echo\n"
                + "echo '[installed module apk]'\n"
                + "MODAPK=$(pm path io.github.pzhown.hyperos4swipegate 2>/dev/null | head -n 1 | sed 's/^package://')\n"
                + "echo \"moduleApk=${MODAPK:-<none>}\"\n"
                + "BB=/data/adb/magisk/busybox\n"
                + "if [ ! -x \"$BB\" ]; then BB=$(command -v busybox 2>/dev/null); fi\n"
                + "echo \"busybox=${BB:-<none>}\"\n"
                + "if [ -n \"$MODAPK\" ] && [ -n \"$BB\" ]; then\n"
                + "  echo 'native_init.list:'\n"
                + "  \"$BB\" unzip -p \"$MODAPK\" META-INF/xposed/native_init.list 2>/dev/null || true\n"
                + "  echo 'scope.list:'\n"
                + "  \"$BB\" unzip -p \"$MODAPK\" META-INF/xposed/scope.list 2>/dev/null || true\n"
                + "  SO_SIZE=$(\"$BB\" unzip -p \"$MODAPK\" lib/arm64-v8a/libhyperos4swipegate.so 2>/dev/null | \"$BB\" wc -c)\n"
                + "  echo \"nativeSoBytes=$SO_SIZE\"\n"
                + "fi\n"
                + "echo\n"
                + "echo '[module mapping in launcher]'\n"
                + "if [ -n \"$PID\" ] && [ -r \"/proc/$PID/maps\" ]; then\n"
                + "  grep -F 'libhyperos4swipegate.so' \"/proc/$PID/maps\" || echo '<module so not mapped>'\n"
                + "else echo '<launcher maps unavailable>'; fi\n"
                + "echo\n"
                + "echo '[launcher apk]'\n"
                + "LAUNCHER_APK=$(pm path com.miui.home 2>/dev/null | head -n 1 | sed 's/^package://')\n"
                + "echo \"launcherApk=${LAUNCHER_APK:-<none>}\"\n"
                + "dumpsys package com.miui.home 2>/dev/null | grep -E 'versionCode=|versionName=' | head -n 4 || true\n"
                + "echo\n"
                + "echo '[direct exact-5459 code signature]'\n"
                + "if [ -n \"$LAUNCHER_APK\" ] && [ -n \"$BB\" ]; then\n"
                + "  SIG=$(\"$BB\" unzip -p \"$LAUNCHER_APK\" lib/arm64-v8a/libapp_launcher.so 2>/dev/null | \"$BB\" dd bs=1 skip=$((0x816fc4)) count=16 2>/dev/null | \"$BB\" od -An -tx1 -v | tr -d ' \\n')\n"
                + "  echo \"offset=0x816fc4 signature=$SIG\"\n"
                + "  if [ \"$SIG\" = 'ff8305d1ea7b00fde9a30f6dfdfb10a9' ]; then\n"
                + "    echo 'signatureMatch=1'\n"
                + "    echo 'CODE_FILE_PROBE_BEGIN encoding=base64 offset=0x816fc4 bytes=8192'\n"
                + "    \"$BB\" unzip -p \"$LAUNCHER_APK\" lib/arm64-v8a/libapp_launcher.so 2>/dev/null | \"$BB\" dd bs=1 skip=$((0x816fc4)) count=$((0x2000)) 2>/dev/null | \"$BB\" base64 | \"$BB\" fold -w 512\n"
                + "    echo 'CODE_FILE_PROBE_END'\n"
                + "  else\n"
                + "    echo 'signatureMatch=0'\n"
                + "  fi\n"
                + "else echo '<launcher APK extraction unavailable>'; fi\n"
                + "echo\n"
                + "echo '[native file logs]'\n"
                + "FOUND=0\n"
                + "for f in /data/user_de/0/com.miui.home/cache/hyperos4swipegate_native.log /data/user/0/com.miui.home/cache/hyperos4swipegate_native.log /data/data/com.miui.home/cache/hyperos4swipegate_native.log; do\n"
                + "  if [ -r \"$f\" ]; then FOUND=1; echo \"--- $f ---\"; tail -n 500 \"$f\"; fi\n"
                + "done\n"
                + "if [ \"$FOUND\" = 0 ]; then echo '<no native log file found>'; fi\n"
                + "echo\n"
                + "echo '[logcat HyperOS4SwipeGateNative]'\n"
                + "logcat -d -v threadtime -s HyperOS4SwipeGateNative:* 2>/dev/null | tail -n 500\n"
                + "echo\n"
                + "echo '[recent LSPosed/module loader logcat]'\n"
                + "logcat -d -v threadtime 2>/dev/null | grep -iE 'LSPosed|HyperOS4SwipeGate|libhyperos4swipegate|native_init' | tail -n 300 || true\n";

        try {
            Process process = new ProcessBuilder("su", "-c", script)
                    .redirectErrorStream(true)
                    .start();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    out.append(line).append('\n');
                }
            }
            int exit = process.waitFor();
            out.append("\n[su exit] ").append(exit).append('\n');
            if (exit != 0) {
                out.append("Root 日志读取失败。请在 Root 管理器中允许 HyperOS4 SwipeGate 的 su 权限。\n");
            }
        } catch (Exception e) {
            out.append("[su unavailable] ").append(e.getClass().getSimpleName())
                    .append(": ").append(e.getMessage()).append('\n');
            out.append("无法跨 UID 读取 com.miui.home 的 Native 日志。请授予本 App su 权限后刷新。\n");
        }
        return out.toString();
    }
}
