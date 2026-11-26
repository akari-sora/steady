package dev.omarchy.steadysender;

/**
 * Stationary rule from the released v1.1.0 sender.
 *
 * Linear acceleration: the sample is motion when {@code max(|ax|, |ay|)}
 * is above {@link #ACCEL_THRESHOLD}, or {@code |gz|} is above
 * {@link #GYRO_THRESHOLD}. {@code az} is not part of that comparison.
 * A gravity-included accelerometer instead compares each axis with the
 * previous moving average (keep {@link #GRAVITY_KEEP}, blend
 * {@link #GRAVITY_BLEND} of the new sample, updated after the comparison).
 * The first gravity sample only seeds that average.
 *
 * Stillness is the time since the last motion sample, taken from the
 * sample's own timestamp. There is no window sized for a particular rate.
 * The caller treats {@link StreamTiming#IDLE_ENTER_MS} of stillness as
 * stationary.
 *
 * A phone whose linear acceleration sits above {@link #ACCEL_THRESHOLD}
 * while it is lying still (SM-G950F) does not enter stationary mode.
 * That is the v1.1.0 limitation; this class does not learn a resting level.
 */
public final class StationaryDetector {
    public static final double ACCEL_THRESHOLD = 0.22;
    public static final double GYRO_THRESHOLD = 0.08;
    /** v1.1.0 gravity average: previous * KEEP + sample * BLEND. */
    public static final double GRAVITY_KEEP = 0.85;
    public static final double GRAVITY_BLEND = 0.15;

    private boolean gravityIncluded;
    private boolean hasAvg;
    private double avgX, avgY, avgZ;
    private long lastMotionMs;
    private long lastGoodMs;
    private boolean haveGood;

    public void setGravityIncluded(boolean gravity) {
        if (gravity == gravityIncluded) return;
        gravityIncluded = gravity;
        hasAvg = false;
    }

    public boolean gravityIncluded() {
        return gravityIncluded;
    }

    public void reset(long nowMs) {
        hasAvg = false;
        avgX = avgY = avgZ = 0.0;
        lastMotionMs = nowMs;
        lastGoodMs = nowMs;
        haveGood = false;
    }

    /**
     * @param gz yaw rate in rad/s, already sign-corrected; 0 when the
     *           gyroscope is not registered
     * @return milliseconds since the last sample that was motion.
     *         A non-finite sample, or a timestamp older than the last
     *         good one, is ignored and does not change this.
     */
    public long observe(long nowMs, double ax, double ay, double az, double gz) {
        if (!Double.isFinite(ax) || !Double.isFinite(ay) || !Double.isFinite(az) || !Double.isFinite(gz)) {
            return stillFrozen();
        }
        if (haveGood && nowMs < lastGoodMs) {
            return stillFrozen();
        }
        boolean motion = Math.abs(gz) > GYRO_THRESHOLD;
        if (gravityIncluded) {
            if (!hasAvg) {
                avgX = ax;
                avgY = ay;
                avgZ = az;
                hasAvg = true;
            } else {
                double mag = Math.max(Math.abs(ax - avgX),
                    Math.max(Math.abs(ay - avgY), Math.abs(az - avgZ)));
                avgX = avgX * GRAVITY_KEEP + ax * GRAVITY_BLEND;
                avgY = avgY * GRAVITY_KEEP + ay * GRAVITY_BLEND;
                avgZ = avgZ * GRAVITY_KEEP + az * GRAVITY_BLEND;
                if (mag > ACCEL_THRESHOLD) motion = true;
            }
        } else if (Math.max(Math.abs(ax), Math.abs(ay)) > ACCEL_THRESHOLD) {
            motion = true;
        }
        if (motion) lastMotionMs = nowMs;
        lastGoodMs = nowMs;
        haveGood = true;
        long still = nowMs - lastMotionMs;
        return still < 0L ? 0L : still;
    }

    private long stillFrozen() {
        if (!haveGood) return 0L;
        long still = lastGoodMs - lastMotionMs;
        return still < 0L ? 0L : still;
    }
}
