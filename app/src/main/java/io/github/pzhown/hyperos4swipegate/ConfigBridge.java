package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ConfigBridge {
    public static final String PREF_KEY_THRESHOLD = "trigger_threshold_percent";
    public static final int DEFAULT_THRESHOLD = 55;
    public static final String SYSTEM_PROPERTY = "persist.hyperos4swipegate.threshold";

    private static final ExecutorService EXECUTOR = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private ConfigBridge() {}

    public interface Callback {
        void onResult(Result result);
    }

    public record Result(boolean success, int value, String message) {}

    public static void applyThresholdAsync(Context context, int threshold, Callback callback) {
        int safeThreshold = Math.max(0, Math.min(100, threshold));
        EXECUTOR.execute(() -> {
            Result result = applyThreshold(safeThreshold);
            MAIN.post(() -> callback.onResult(result));
        });
    }

    private static Result applyThreshold(int threshold) {
        String value = Integer.toString(threshold);
        CommandResult resetProp = runSu("resetprop " + SYSTEM_PROPERTY + " " + value);
        if (!resetProp.success()) {
            CommandResult setProp = runSu("setprop " + SYSTEM_PROPERTY + " " + value);
            if (!setProp.success()) {
                String detail = !resetProp.output().isBlank()
                        ? resetProp.output()
                        : setProp.output();
                if (detail.isBlank()) {
                    detail = "需要 Root，且 resetprop/setprop 不可用";
                }
                return new Result(false, threshold, detail);
            }
        }

        CommandResult verify = runSu("getprop " + SYSTEM_PROPERTY);
        if (!verify.success() || !value.equals(verify.output().trim())) {
            return new Result(false, threshold, "系统属性校验失败");
        }
        return new Result(true, threshold, "ok");
    }

    private static CommandResult runSu(String command) {
        Process process = null;
        try {
            process = new ProcessBuilder("su", "-c", command)
                    .redirectErrorStream(true)
                    .start();
            StringBuilder output = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if (output.length() > 0) {
                        output.append('\n');
                    }
                    output.append(line);
                }
            }
            int exit = process.waitFor();
            return new CommandResult(exit == 0, output.toString().trim());
        } catch (Exception e) {
            return new CommandResult(false, e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage());
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    private record CommandResult(boolean success, String output) {}
}
