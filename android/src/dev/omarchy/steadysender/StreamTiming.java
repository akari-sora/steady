package dev.omarchy.steadysender;

/**
 * Sample and transmit timing shared by the service and the JVM tests.
 * Driving packets are the average of each pair of 25 Hz samples, so the
 * wire rate is {@link #DRIVE_SEND_HZ}.
 */
public final class StreamTiming {
    /** Sensor sample rate while driving. Averaging happens after this. */
    public static final int SAMPLE_HZ = 25;
    public static final int SAMPLE_PERIOD_US = 1_000_000 / SAMPLE_HZ;

    /** One named wire rate. Interval and batch latency are derived from it. */
    public static final double DRIVE_SEND_HZ = 12.5;
    public static final int DRIVE_AVERAGE_COUNT = 2;
    public static final long DRIVE_SEND_INTERVAL_MS = Math.round(1000.0 / DRIVE_SEND_HZ);
    public static final int DRIVE_REPORT_LATENCY_US = (int) DRIVE_SEND_INTERVAL_MS * 1000;

    /**
     * Stationary sampling. Report latency stays 0 so a brake is not held in
     * a FIFO. The period is one sample inside the 200 ms / 1.0 m/s² limit
     * even when the step starts just after a sample.
     */
    public static final int IDLE_SAMPLE_PERIOD_US = 100_000;
    public static final int IDLE_REPORT_LATENCY_US = 0;
    public static final long IDLE_SEND_INTERVAL_MS = 1_000L;
    /** Retry registration here when the requested period is refused. */
    public static final int BIND_FALLBACK_PERIOD_US = SAMPLE_PERIOD_US;
    public static final int BIND_FALLBACK_LATENCY_US = 0;

    public static final long IDLE_ENTER_MS = 4_000L;
    public static final long AUTO_STOP_STILL_MS = 20L * 60L * 1000L;

    private StreamTiming() {}

    /**
     * Debug override for the 20-minute still stop. Zero or a longer value
     * keeps the production constant; only a shorter positive value applies.
     */
    public static long clampStillStop(long overrideMs) {
        if (overrideMs <= 0L || overrideMs >= AUTO_STOP_STILL_MS) {
            return AUTO_STOP_STILL_MS;
        }
        return overrideMs;
    }

    /**
     * Averages each pair of driving samples and emits one packet per
     * {@link #DRIVE_SEND_INTERVAL_MS}. No per-sample allocation.
     */
    public static final class DriveWindow {
        private double sx, sy, sz, sg;
        private int n;
        private long lastSendMs;
        private double emitAx, emitAy, emitAz, emitGz;

        public void reset(long nowMs) {
            n = 0;
            sx = sy = sz = sg = 0.0;
            lastSendMs = nowMs - DRIVE_SEND_INTERVAL_MS;
            emitAx = emitAy = emitAz = emitGz = 0.0;
        }

        /** @return true when a pair is ready to send; read emitAx/Ay/Az/Gz */
        public boolean onSample(long nowMs, double ax, double ay, double az, double gz) {
            if (n >= DRIVE_AVERAGE_COUNT && nowMs - lastSendMs >= DRIVE_SEND_INTERVAL_MS) {
                stash();
                clearSums();
                lastSendMs = nowMs;
                addOne(ax, ay, az, gz);
                return true;
            }
            if (n < DRIVE_AVERAGE_COUNT) {
                addOne(ax, ay, az, gz);
            }
            if (n >= DRIVE_AVERAGE_COUNT && nowMs - lastSendMs >= DRIVE_SEND_INTERVAL_MS) {
                stash();
                clearSums();
                lastSendMs = nowMs;
                return true;
            }
            return false;
        }

        public double emitAx() { return emitAx; }
        public double emitAy() { return emitAy; }
        public double emitAz() { return emitAz; }
        public double emitGz() { return emitGz; }

        private void addOne(double ax, double ay, double az, double gz) {
            sx += ax;
            sy += ay;
            sz += az;
            sg += gz;
            n++;
        }

        private void stash() {
            double k = n;
            emitAx = sx / k;
            emitAy = sy / k;
            emitAz = sz / k;
            emitGz = sg / k;
        }

        private void clearSums() {
            n = 0;
            sx = sy = sz = sg = 0.0;
        }
    }
}
