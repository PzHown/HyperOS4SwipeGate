package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.os.SystemClock;

import java.io.DataInputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Rootless one-way diagnostics channel from the hooked Launcher process back to the module app.
 * The endpoint is bound to loopback only; port and token are published to the hooked process via
 * libxposed RemotePreferences, which remain read-only there.
 *
 * Native hook health is transported independently from the user-facing log level so the home page
 * can require real Pattern/HOOK_HEALTH evidence even when log recording is disabled.
 */
public final class DiagnosticsStreamBridge {
    static final String HOOK_STATUS_PREFIX = "SWIPEGATE_STATUS_V1";

    private static final int MAX_PAYLOAD_BYTES = 128 * 1024;
    private static final long HOOK_STATUS_STALE_AFTER_MS = 5_000L;
    private static final AtomicBoolean STARTED = new AtomicBoolean(false);

    private static volatile ServerSocket serverSocket;
    private static volatile int port;
    private static volatile String token = "";
    private static volatile String latestLog = "";
    private static volatile String lastError = "";
    private static volatile NativeHookStatus latestHookStatus = NativeHookStatus.unknown();

    private DiagnosticsStreamBridge() {}

    public record NativeHookStatus(
            String state,
            String pattern,
            String detail,
            long receivedAtElapsedMs
    ) {
        static NativeHookStatus unknown() {
            return new NativeHookStatus("UNKNOWN", "", "", 0L);
        }

        public boolean fresh() {
            if (receivedAtElapsedMs <= 0L) return false;
            long age = SystemClock.elapsedRealtime() - receivedAtElapsedMs;
            return age >= 0L && age <= HOOK_STATUS_STALE_AFTER_MS;
        }

        public boolean healthy() {
            return fresh() && "HEALTHY".equals(state);
        }
    }

    public static void initialize(Context context) {
        if (!STARTED.compareAndSet(false, true)) return;
        try {
            ServerSocket server = new ServerSocket();
            server.setReuseAddress(true);
            server.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 4);
            serverSocket = server;
            port = server.getLocalPort();
            token = UUID.randomUUID().toString().replace("-", "");
            lastError = "";

            Thread worker = new Thread(DiagnosticsStreamBridge::acceptLoop, "SwipeGateDiagnosticsServer");
            worker.setDaemon(true);
            worker.start();
        } catch (Throwable t) {
            String message = t.getMessage();
            lastError = message == null || message.isBlank()
                    ? t.getClass().getSimpleName()
                    : message;
            port = 0;
            token = "";
        }
    }

    public static int port() {
        return port;
    }

    public static String token() {
        return token;
    }

    public static void clearLog() {
        latestLog = "";
        lastError = "";
    }

    public static NativeHookStatus nativeHookStatus() {
        return latestHookStatus;
    }

    public static String currentLog() {
        String current = latestLog;
        if (!current.isBlank()) return current;
        if (!lastError.isBlank()) return "Native 日志通道启动失败：" + lastError;
        if (port <= 0 || token.isBlank()) return "Native 日志通道尚未就绪。";
        return "等待 Native 日志…\n请返回系统桌面执行一次侧滑后点刷新。";
    }

    private static void acceptLoop() {
        ServerSocket server = serverSocket;
        if (server == null) return;

        while (!server.isClosed()) {
            try (Socket client = server.accept()) {
                client.setSoTimeout(1500);
                DataInputStream input = new DataInputStream(client.getInputStream());
                String receivedToken = input.readUTF();
                int length = input.readInt();
                if (!token.equals(receivedToken) || length < 0 || length > MAX_PAYLOAD_BYTES) {
                    continue;
                }
                byte[] payload = new byte[length];
                input.readFully(payload);
                String text = new String(payload, StandardCharsets.UTF_8).trim();
                if (!text.isBlank()) {
                    NativeHookStatus status = parseHookStatus(text);
                    if (status != null) {
                        latestHookStatus = status;
                    } else {
                        latestLog = text;
                    }
                }
                lastError = "";
            } catch (Throwable t) {
                if (server.isClosed()) return;
                String message = t.getMessage();
                if (message != null && !message.isBlank()) lastError = message;
            }
        }
    }

    private static NativeHookStatus parseHookStatus(String text) {
        String[] parts = text.split("\\t", 4);
        if (parts.length < 2 || !HOOK_STATUS_PREFIX.equals(parts[0])) return null;

        String state = parts[1].trim().toUpperCase(Locale.ROOT);
        if (!state.equals("UNKNOWN")
                && !state.equals("WAITING")
                && !state.equals("HEALTHY")
                && !state.equals("REPAIRING")
                && !state.equals("FAILED")) {
            state = "UNKNOWN";
        }
        String pattern = parts.length >= 3 ? parts[2].trim() : "";
        String detail = parts.length >= 4 ? parts[3].trim() : "";
        return new NativeHookStatus(state, pattern, detail, SystemClock.elapsedRealtime());
    }
}
