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
        out.append("stockSidebarBoundary=88dp (264px at 480dpi)\n");
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
                + "echo\n"
                + "echo '[processes]'\n"
                + "ps -A 2>/dev/null | grep -E 'hyos_spawner|com.miui.home' || true\n"
                + "PID=$(pidof com.miui.home 2>/dev/null | awk '{print $1}')\n"
                + "echo \"launcherPid=${PID:-<none>}\"\n"
                + "echo\n"
                + "echo '[module mapping in launcher]'\n"
                + "if [ -n \"$PID\" ] && [ -r \"/proc/$PID/maps\" ]; then grep -F 'libhyperos4swipegate.so' \"/proc/$PID/maps\" || echo '<module so not mapped>'; else echo '<launcher maps unavailable>'; fi\n"
                + "echo\n"
                + "echo '[native file logs]'\n"
                + "FOUND=0\n"
                + "for f in /data/user_de/0/com.miui.home/cache/hyperos4swipegate_native.log /data/user/0/com.miui.home/cache/hyperos4swipegate_native.log /data/data/com.miui.home/cache/hyperos4swipegate_native.log; do if [ -r \"$f\" ]; then FOUND=1; echo \"--- $f ---\"; tail -n 240 \"$f\"; fi; done\n"
                + "if [ \"$FOUND\" = 0 ]; then echo '<no native log file found>'; fi\n"
                + "echo\n"
                + "echo '[logcat HyperOS4SwipeGateNative]'\n"
                + "logcat -d -v threadtime -s HyperOS4SwipeGateNative:* 2>/dev/null | tail -n 240\n";

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
                out.append("Root 日志读取失败。请确认已授予本 App su 权限。\n");
            }
        } catch (Exception e) {
            out.append("[su unavailable] ").append(e.getClass().getSimpleName())
                    .append(": ").append(e.getMessage()).append('\n');
        }
        return out.toString();
    }
}
