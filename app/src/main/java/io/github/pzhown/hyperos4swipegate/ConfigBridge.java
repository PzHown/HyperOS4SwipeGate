package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ConfigBridge {
    public static final String PREF_KEY_THRESHOLD = "trigger_threshold_percent";
    public static final int DEFAULT_THRESHOLD = 55;
    public static final String SYSTEM_PROPERTY = "persist.hyperos4swipegate.threshold";
    public static final String SCREEN_WIDTH_PROPERTY = "persist.hyperos4swipegate.screen_width";

    private static final ExecutorService EXECUTOR = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private ConfigBridge() {}

    public interface Callback {
        void onResult(Result result);
    }

    public record Result(boolean success, int value, String message) {}

    public static void applyThresholdAsync(Context context, int threshold, Callback callback) {
        int safeThreshold = Math.max(0, Math.min(100, threshold));
        int screenWidth = resolveScreenWidth(context);
        EXECUTOR.execute(() -> {
            if (screenWidth > 0) {
                applyIntegerProperty(SCREEN_WIDTH_PROPERTY, screenWidth);
            }
            Result result = applyThreshold(safeThreshold);
            MAIN.post(() -> callback.onResult(result));
        });
    }

    public static void syncScreenWidthAsync(Context context) {
        int screenWidth = resolveScreenWidth(context);
        if (screenWidth <= 0) {
            return;
        }
        EXECUTOR.execute(() -> applyIntegerProperty(SCREEN_WIDTH_PROPERTY, screenWidth));
    }

    private static int resolveScreenWidth(Context context) {
        try {
            WindowManager windowManager = context.getSystemService(WindowManager.class);
            if (windowManager != null) {
                Rect bounds = windowManager.getCurrentWindowMetrics().getBounds();
                if (bounds.width() > 0) {
                    return bounds.width();
                }
            }
        } catch (Throwable ignored) {
        }
        return context.getResources().getDisplayMetrics().widthPixels;
    }

    private static Result applyThreshold(int threshold) {
        CommandResult result = applyIntegerProperty(SYSTEM_PROPERTY, threshold);
        if (!result.success()) {
            String detail = result.output();
            if (detail.isBlank()) {
                detail = "需要 Root，且 resetprop/setprop 不可用";
            }
            return new Result(false, threshold, detail);
        }
        return new Result(true, threshold, "ok");
    }

    private static CommandResult applyIntegerProperty(String property, int integerValue) {
        String value = Integer.toString(integerValue);
        CommandResult resetProp = runSu("resetprop " + property + " " + value);
        if (!resetProp.success()) {
            CommandResult setProp = runSu("setprop " + property + " " + value);
            if (!setProp.success()) {
                String detail = !resetProp.output().isBlank()
                        ? resetProp.output()
                        : setProp.output();
                return new CommandResult(false, detail);
            }
        }

        CommandResult verify = runSu("getprop " + property);
        if (!verify.success() || !value.equals(verify.output().trim())) {
            return new CommandResult(false, "系统属性校验失败: " + property);
        }
        return new CommandResult(true, value);
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
