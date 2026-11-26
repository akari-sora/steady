package dev.omarchy.steadysender;

/**
 * Shortened still-stop and unreachable-stop values come from an adb-set
 * property string. An intent extra is not a source: {@code ignoredExtra}
 * does not change the result.
 */
public final class TimeoutSource {
    private TimeoutSource() {}

    public static long stillMs(long ignoredExtra, String propertyValue) {
        return shorten(propertyValue, StreamTiming.AUTO_STOP_STILL_MS);
    }

    public static long unreachableMs(long ignoredExtra, String propertyValue) {
        return shorten(propertyValue, DesktopProbe.UNREACHABLE_STOP_MS);
    }

    static long shorten(String propertyValue, long production) {
        if (propertyValue == null) return production;
        String s = propertyValue.trim();
        if (s.isEmpty()) return production;
        long v;
        try {
            v = Long.parseLong(s);
        } catch (NumberFormatException e) {
            return production;
        }
        if (v < 1_000L || v >= production) return production;
        return v;
    }
}
