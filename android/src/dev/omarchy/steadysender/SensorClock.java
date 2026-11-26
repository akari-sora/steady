package dev.omarchy.steadysender;

/**
 * {@code SensorEvent.timestamp} is nanoseconds on the elapsed-realtime
 * clock. The detector must use that, not the time the callback runs, or a
 * batch delivered together looks like a 1 ms gap.
 */
public final class SensorClock {
    private SensorClock() {}

    /** @return elapsed milliseconds, or -1 when the stamp cannot be used */
    public static long toElapsedMs(long timestampNs) {
        if (timestampNs <= 0L) return -1L;
        return timestampNs / 1_000_000L;
    }
}
