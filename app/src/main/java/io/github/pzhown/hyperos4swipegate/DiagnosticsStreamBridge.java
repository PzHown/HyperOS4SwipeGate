package io.github.pzhown.hyperos4swipegate;

import android.content.Context;

import java.io.DataInputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Rootless one-way diagnostics channel from the hooked Launcher process back to the module app.
 * The endpoint is bound to loopback only; port and token are published to the hooked process via
 * libxposed RemotePreferences, which remain read-only there.
 */
public final class DiagnosticsStreamBridge {
    private static final int MAX_PAYLOAD_BYTES = 128 * 1024;
    private static final AtomicBoolean STARTED = new AtomicBoolean(false);

    private static volatile ServerSocket serverSocket;
    private static volatile int port;
    private static volatile String token = "";
    private static volatile String latestLog = "";
    private static volatile String lastError = "";

    private DiagnosticsStreamBridge() {}

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
                if (!text.isBlank()) latestLog = text;
                lastError = "";
            } catch (Throwable t) {
                if (server.isClosed()) return;
                String message = t.getMessage();
                if (message != null && !message.isBlank()) lastError = message;
            }
        }
    }
}
