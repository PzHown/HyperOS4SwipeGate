package io.github.pzhown.hyperos4swipegate;

import java.security.SecureRandom;

/** A bounded request and its private SystemUI-to-Launcher return capability. */
final class ControlRequest {
    static final long LIFETIME_MS = 12_000L;
    static final long MIN_FORWARD_INTERVAL_MS = 750L;
    private static final SecureRandom RANDOM = new SecureRandom();

    final long nonce;
    final long challengeHigh;
    final long challengeLow;
    final long createdAtMs;
    final int thresholdDp;
    final int logLevel;
    final boolean hapticEnabled;
    final boolean breakOpenEnabled;
    private long lastForwardAtMs;
    private boolean forwarded;

    ControlRequest(long nonce, long now, int thresholdDp, int logLevel,
                   boolean hapticEnabled, boolean breakOpenEnabled) {
        this.nonce = nonce;
        this.createdAtMs = now;
        this.thresholdDp = thresholdDp;
        this.logLevel = logLevel;
        this.hapticEnabled = hapticEnabled;
        this.breakOpenEnabled = breakOpenEnabled;
        long high;
        long low;
        do {
            high = RANDOM.nextLong();
            low = RANDOM.nextLong();
        } while ((high | low) == 0L);
        challengeHigh = high;
        challengeLow = low;
    }

    boolean expired(long now) {
        return now < createdAtMs || now - createdAtMs >= LIFETIME_MS;
    }

    boolean matchesConfig(int threshold, int level, boolean haptic, boolean breakOpen) {
        return thresholdDp == threshold && logLevel == level
                && hapticEnabled == haptic && breakOpenEnabled == breakOpen;
    }

    boolean canForward(long now) {
        return !expired(now) && (!forwarded || now - lastForwardAtMs >= MIN_FORWARD_INTERVAL_MS);
    }

    void markForwarded(long now) {
        lastForwardAtMs = now;
        forwarded = true;
    }

    boolean authenticates(long replyNonce, long high, long low, long now) {
        return !expired(now) && nonce == replyNonce
                && ((challengeHigh ^ high) | (challengeLow ^ low)) == 0L;
    }
}
