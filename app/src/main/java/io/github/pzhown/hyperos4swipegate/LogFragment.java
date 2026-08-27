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
        ConfigBridge.syncScreenWidthAsync(requireContext());
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
        ConfigBridge.syncScreenWidthAsync(requireContext());
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
                + "echo '[threshold]'\n"
                + "getprop persist.hyperos4swipegate.threshold\n"
                + "echo\n"
                + "echo '[screen width]'\n"
                + "getprop persist.hyperos4swipegate.screen_width\n"
                + "echo\n"
                + "echo '[processes]'\n"
                + "ps -A 2>/dev/null | grep -E 'hyos_spawner|com.miui.home' || true\n"
                + "echo\n"
                + "echo '[native file logs]'\n"
                + "FOUND=0\n"
                + "for f in /data/user_de/0/com.miui.home/cache/hyperos4swipegate_native.log /data/user/0/com.miui.home/cache/hyperos4swipegate_native.log /data/data/com.miui.home/cache/hyperos4swipegate_native.log; do\n"
                + "  if [ -r \"$f\" ]; then FOUND=1; echo \"--- $f ---\"; tail -n 500 \"$f\"; fi\n"
                + "done\n"
                + "if [ \"$FOUND\" = 0 ]; then echo '<no native log file found>'; fi\n"
                + "echo\n"
                + "echo '[logcat HyperOS4SwipeGateNative]'\n"
                + "logcat -d -v threadtime -s HyperOS4SwipeGateNative:* 2>/dev/null | tail -n 500\n";

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
