package dev.omarchy.steadysender;

/**
 * What to do after an accelerometer / gyroscope registerListener result.
 * The service may record the mode only when {@link #bound} is true.
 */
public final class SensorBind {
    public enum Step { BOUND, RETRY, STOP }

    public final Step step;
    public final boolean bound;
    /** Idle vs driving flag to store. Meaningful only when {@link #bound}. */
    public final boolean idleModeApplied;
    public final boolean gyroOn;
    public final String error;

    private SensorBind(Step step, boolean bound, boolean idleModeApplied, boolean gyroOn, String error) {
        this.step = step;
        this.bound = bound;
        this.idleModeApplied = idleModeApplied;
        this.gyroOn = gyroOn;
        this.error = error;
    }

    /**
     * @param isRetry true when this result is already the latency-0 fallback
     */
    public static SensorBind after(boolean accelOk, boolean gyroWanted, boolean gyroOk,
                                   boolean wantIdle, boolean isRetry) {
        if (!accelOk) {
            if (!isRetry) {
                return new SensorBind(Step.RETRY, false, false, false, null);
            }
            return new SensorBind(Step.STOP, false, false, false, "無法註冊加速度計");
        }
        if (gyroWanted && !gyroOk) {
            return new SensorBind(Step.BOUND, true, wantIdle, false, "陀螺儀未註冊，僅以加速度計運作");
        }
        return new SensorBind(Step.BOUND, true, wantIdle, gyroWanted, null);
    }
}
