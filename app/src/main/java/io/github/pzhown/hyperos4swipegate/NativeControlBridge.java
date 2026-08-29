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
 * hook health and user-facing logs must not depend on ModuleMain. The app owns a fixed IPv4
 * loopback endpoint while it is alive; the injected native module connects outward from the
 * Launcher child, publishes a snapshot and receives the latest locally persisted settings in the
 * same exchange.
 */
public final class NativeControlBridge {
    private static final int PROTOCOL_MAGIC = 0x53474331; // SGC1
    private static final int PROTOCOL_VERSION = 1;
    private static final String CONTROL_HOST = "127.0.0.1";
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
            // Native side uses AF_INET + INADDR_LOOPBACK. Do not use getLoopbackAddress() here:
            // Android may resolve that to IPv6 ::1, leaving the IPv4 native peer unable to connect.
            InetAddress ipv4Loopback = InetAddress.getByName(CONTROL_HOST);
            server.bind(new InetSocketAddress(ipv4Loopback, CONTROL_PORT), 8);
            serverSocket = server;
            lastError = "";
            Thread worker = new Thread(NativeControlBridge::acceptLoop, "SwipeGateNativeControl");
            worker.setDaemon(true);
            worker.start();
        } catch (Throwable t) {
            serverSocket = null;
            STARTED.set(false);
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
        // Keep transport errors. Clearing user-facing runtime logs must not erase the only evidence
        // that the control listener failed to bind or accept a peer.
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

        ServerSocket server = serverSocket;
        if (server == null || server.isClosed()) {
            return "Native 直连监听未启动。";
        }
        return "等待 Native 直连…\nApp 已监听 127.0.0.1:39173，等待 Launcher Native 连接。";
    }

    private static void acceptLoop() {
        ServerSocket server = serverSocket;
        if (server == null) return;

        while (!server.isClosed()) {
            try (Socket client = server.accept()) {
                client.setSoTimeout(1_000);
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
