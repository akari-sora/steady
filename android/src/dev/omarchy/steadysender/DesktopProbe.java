package dev.omarchy.steadysender;

/**
 * Discover-probe state machine. While the desktop answers, probe every
 * {@link #REACHABLE_INTERVAL_MS}. A probe that is still unanswered when the
 * next one is due counts as a miss. After {@link #MISSES_TO_PAUSE} misses,
 * sample packets stop and probes continue no faster than
 * {@link #UNREACHABLE_INTERVAL_MS}. A reply resumes samples immediately.
 * Being unreachable for {@link #UNREACHABLE_STOP_MS} ends the session.
 * The first miss is counted at the second send, so three misses land about
 * 30 s after the first probe.
 */
public final class DesktopProbe {
    public static final long REACHABLE_INTERVAL_MS = 10_000L;
    public static final long UNREACHABLE_INTERVAL_MS = 1_000L;
    public static final int MISSES_TO_PAUSE = 3;
    public static final long UNREACHABLE_STOP_MS = 5L * 60L * 1000L;

    private long stopAfterMs = UNREACHABLE_STOP_MS;
    private int misses;
    private long lastSentMs = -1L;
    private boolean awaiting;
    private boolean reachable = true;
    private long unreachableSinceMs = -1L;

    /** Debug-only. Rejects a value that would lengthen the production timeout. */
    public void overrideStopAfterMs(long ms) {
        if (ms >= 1_000L && ms < UNREACHABLE_STOP_MS) {
            stopAfterMs = ms;
        }
    }

    public long stopAfterMs() {
        return stopAfterMs;
    }

    public boolean samplesEnabled() {
        return reachable;
    }

    /** True after {@link #markSent} until a matching reply is accepted. */
    public boolean awaitingReply() {
        return awaiting;
    }

    public int misses() {
        return misses;
    }

    public boolean due(long nowMs) {
        if (lastSentMs < 0L) return true;
        long interval = reachable ? REACHABLE_INTERVAL_MS : UNREACHABLE_INTERVAL_MS;
        return nowMs - lastSentMs >= interval;
    }

    public void markSent(long nowMs) {
        if (awaiting) {
            misses++;
            if (reachable && misses >= MISSES_TO_PAUSE) {
                reachable = false;
                unreachableSinceMs = nowMs;
            }
        }
        lastSentMs = nowMs;
        awaiting = true;
    }

    public void markReply(long nowMs) {
        awaiting = false;
        misses = 0;
        reachable = true;
        unreachableSinceMs = -1L;
        lastSentMs = nowMs;
    }

    public boolean shouldStop(long nowMs) {
        return !reachable
            && unreachableSinceMs >= 0L
            && nowMs - unreachableSinceMs >= stopAfterMs;
    }
}
