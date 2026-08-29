package io.github.pzhown.hyperos4swipegate;

import android.content.Context;
import android.os.SystemClock;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Direct App <-> HyperOS Runtime native transport.
 *
 * HyperOS Launcher native children do not execute libxposed Java module entries, so configuration,
 * hook health and user-facing logs must not depend on ModuleMain. The app owns a fixed loopback
 * endpoint while it is alive; the injected native module connects outward from the Launcher child,
 * publishes a snapshot and receives the latest locally persisted settings in the same exchange.
 */
public final class NativeControlBridge {
    private static final int PROTOCOL_MAGIC = 0x53474331; // SGC1
    private static final int PROTOCOL_VERSION = 1;
    private static final int CONTROL_PORT = 39173;
    private static final int MAX_PATTERN_BYTES = 128;
    private static final int MAX_DETAIL_BYTES = 768;
    private static final int MAX_LOG_BYTES = 24 * 1024;
    private static final long PEER_FRESH_MS = 5_000L;

    private static final AtomicBoolean STARTED = new AtomicBoolean(false);
    private static volatile Context appContext;
    private static volatile ServerSocket serverSocket;
    private static volatile String lastError = "";
    private static volatile String latestLog = "";
    private static volatile Snapshot latestSnapshot = Snapshot.unknown();

    private NativeControlBridge() {}

    public record Snapshot(
            String state,
            String pattern,
            String detail,
            long receivedAtElapsedMs
    ) {
        static Snapshot unknown() {
            return new Snapshot("UNKNOWN", "", "等待 Native Hook 状态", 0L);
        }

        public boolean fresh() {
            if (receivedAtElapsedMs <= 0L) return false;
            long age = SystemClock.elapsedRealtime() - receivedAtElapsedMs;
            return age >= 0L && age <= PEER_FRESH_MS;
        }
    }

    public static void initialize(Context context) {
        appContext = context.getApplicationContext();
        if (!STARTED.compareAndSet(false, true)) return;

        try {
            ServerSocket server = new ServerSocket();
            server.setReuseAddress(true);
            server.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), CONTROL_PORT), 8);
            serverSocket = server;
            lastError = "";
            Thread worker = new Thread(NativeControlBridge::acceptLoop, "SwipeGateNativeControl");
            worker.setDaemon(true);
            worker.start();
        } catch (Throwable t) {
            String message = t.getMessage();
            lastError = message == null || message.isBlank()
                    ? t.getClass().getSimpleName()
                    : message;
        }
    }

    public static Snapshot snapshot() {
        return latestSnapshot;
    }

    public static boolean hasFreshPeer() {
        return latestSnapshot.fresh();
    }

    public static void clearLog() {
        latestLog = "";
        lastError = "";
    }

    public static String currentLog() {
        Context context = appContext;
        int level = context == null
                ? ConfigBridge.DEFAULT_LOG_LEVEL
                : ConfigBridge.sanitizeLogLevel(
                        ConfigBridge.localPreferences(context).getInt(
                                ConfigBridge.PREF_KEY_LOG_LEVEL,
                                ConfigBridge.DEFAULT_LOG_LEVEL));
        if (level <= ConfigBridge.LOG_LEVEL_OFF) return "日志记录已关闭。";
        if (!latestLog.isBlank()) return latestLog;
        if (!lastError.isBlank()) return "Native 直连通道异常：" + lastError;
        if (latestSnapshot.fresh()) return "Native 已连接，暂无运行日志。";
        return "等待 Native 直连…\n返回系统桌面执行一次侧滑后会自动建立通道。";
    }

    private static void acceptLoop() {
        ServerSocket server = serverSocket;
        if (server == null) return;

        while (!server.isClosed()) {
            try (Socket client = server.accept()) {
                client.setSoTimeout(250);
                if (!client.getInetAddress().isLoopbackAddress()) continue;
                handleClient(client);
                lastError = "";
            } catch (Throwable t) {
                if (server.isClosed()) return;
                String message = t.getMessage();
                if (message != null && !message.isBlank()) lastError = message;
            }
        }
    }

    private static void handleClient(Socket client) throws Exception {
        DataInputStream input = new DataInputStream(client.getInputStream());
        DataOutputStream output = new DataOutputStream(client.getOutputStream());

        int magic = input.readInt();
        int version = input.readInt();
        if (magic != PROTOCOL_MAGIC || version != PROTOCOL_VERSION) return;

        String state = stateName(input.readInt());
        String pattern = readUtf8(input, MAX_PATTERN_BYTES);
        String detail = readUtf8(input, MAX_DETAIL_BYTES);
        String log = readUtf8(input, MAX_LOG_BYTES);

        long now = SystemClock.elapsedRealtime();
        latestSnapshot = new Snapshot(state, pattern, detail, now);
        if (!log.isBlank()) latestLog = log.trim();

        Context context = appContext;
        int threshold = ConfigBridge.STOCK_THRESHOLD_DP;
        int logLevel = ConfigBridge.DEFAULT_LOG_LEVEL;
        if (context != null) {
            threshold = ConfigBridge.localPreferences(context)
                    .getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.STOCK_THRESHOLD_DP);
            threshold = Math.max(
                    ConfigBridge.STOCK_THRESHOLD_DP,
                    Math.min(ConfigBridge.MAX_THRESHOLD_DP, threshold));
            logLevel = ConfigBridge.sanitizeLogLevel(
                    ConfigBridge.localPreferences(context).getInt(
                            ConfigBridge.PREF_KEY_LOG_LEVEL,
                            ConfigBridge.DEFAULT_LOG_LEVEL));
        }

        output.writeInt(PROTOCOL_MAGIC);
        output.writeInt(PROTOCOL_VERSION);
        output.writeInt(threshold);
        output.writeInt(logLevel);
        output.flush();
    }

    private static String readUtf8(DataInputStream input, int maxBytes) throws Exception {
        int length = input.readInt();
        if (length < 0 || length > maxBytes) throw new IllegalArgumentException("invalid payload length");
        if (length == 0) return "";
        byte[] bytes = new byte[length];
        input.readFully(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static String stateName(int state) {
        return switch (state) {
            case 1 -> "WAITING";
            case 2 -> "HEALTHY";
            case 3 -> "REPAIRING";
            case 4 -> "FAILED";
            default -> "UNKNOWN";
        };
    }
}
