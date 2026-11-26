package dev.omarchy.steadysender;

/**
 * Per-sample output decision for the IMU stream. The service delivers each
 * accelerometer event with its sensor timestamp and sends only what this
 * class asks for. Leaving idle emits that sample at once.
 */
public final class StreamOutput {
    public enum Kind { NONE, EMIT, STOP_STILL, STOP_SENSOR }

    public static final int BAD_SAMPLES_TO_STOP = 8;
    public static final String SENSOR_ERROR = "感測器數值異常，已停止";

    public static final class Out {
        public final Kind kind;
        public final double ax;
        public final double ay;
        public final double az;
        public final double gz;
        public final boolean idle;
        public final boolean motionIdle;
        public final boolean wantIdleSensors;
        public final String error;

        private Out(Kind kind, double ax, double ay, double az, double gz,
                    boolean idle, boolean motionIdle, boolean wantIdleSensors, String error) {
            this.kind = kind;
            this.ax = ax;
            this.ay = ay;
            this.az = az;
            this.gz = gz;
            this.idle = idle;
            this.motionIdle = motionIdle;
            this.wantIdleSensors = wantIdleSensors;
            this.error = error;
        }

        static Out none(boolean motionIdle, boolean wantIdleSensors) {
            return new Out(Kind.NONE, 0, 0, 0, 0, false, motionIdle, wantIdleSensors, null);
        }

        static Out emit(double ax, double ay, double az, double gz, boolean idle,
                        boolean motionIdle, boolean wantIdleSensors) {
            return new Out(Kind.EMIT, ax, ay, az, gz, idle, motionIdle, wantIdleSensors, null);
        }

        static Out stop(Kind kind, String error, boolean motionIdle) {
            return new Out(kind, 0, 0, 0, 0, false, motionIdle, false, error);
        }
    }

    private final StationaryDetector detector = new StationaryDetector();
    private final StreamTiming.DriveWindow drive = new StreamTiming.DriveWindow();
    private boolean motionIdle;
    private boolean landscapeSet;
    private boolean landscape;
    private long lastSendMs = -1L;
    private int badSamples;
    private long lastSensorMs = Long.MIN_VALUE;

    public void reset(long nowMs) {
        detector.reset(nowMs);
        drive.reset(nowMs);
        motionIdle = false;
        landscapeSet = false;
        lastSendMs = -1L;
        badSamples = 0;
        lastSensorMs = Long.MIN_VALUE;
    }

    /** Call after a packet was actually sent, so the idle rate limit counts it. */
    public void noteSent(long sensorMs) {
        lastSendMs = sensorMs;
    }

    public void resetDrive(long sensorMs) {
        drive.reset(sensorMs);
    }

    /** Gravity-included accelerometer uses the v1.1.0 moving-average path. */
    public void setGravityIncluded(boolean gravity) {
        detector.setGravityIncluded(gravity);
    }

    public StationaryDetector detector() {
        return detector;
    }

    /**
     * @param sensorMs   event time in elapsed milliseconds, not delivery time
     * @param detectGz   yaw used for the stationary decision; 0 while the gyro is off
     * @param wireGz     yaw written into the packet
     */
    public Out onSample(long sensorMs, double rawX, double rawY, double rawZ,
                        double detectGz, double wireGz,
                        boolean land, boolean reachable, long stillStopMs) {
        boolean wantIdle = motionIdle || !reachable;
        if (sensorMs < 0L || !finite(rawX) || !finite(rawY) || !finite(rawZ) || !finite(wireGz)) {
            badSamples++;
            if (badSamples >= BAD_SAMPLES_TO_STOP) {
                return Out.stop(Kind.STOP_SENSOR, SENSOR_ERROR, motionIdle);
            }
            return Out.none(motionIdle, wantIdle);
        }
        if (lastSensorMs != Long.MIN_VALUE && sensorMs < lastSensorMs) {
            return Out.none(motionIdle, wantIdle);
        }
        badSamples = 0;
        lastSensorMs = sensorMs;

        if (!landscapeSet) {
            landscape = land;
            landscapeSet = true;
        } else if (land != landscape) {
            landscape = land;
            detector.reset(sensorMs);
            drive.reset(sensorMs);
        }

        double jx = rawX;
        double jy = rawY;
        if (!detector.gravityIncluded()) {
            jx = land ? -rawY : rawX;
            jy = land ? rawX : rawY;
        }
        boolean wasIdle = motionIdle;
        long still = detector.observe(sensorMs, jx, jy, rawZ, detectGz);
        if (still >= stillStopMs) {
            return Out.stop(Kind.STOP_STILL, null, motionIdle);
        }
        motionIdle = still >= StreamTiming.IDLE_ENTER_MS;
        boolean reportIdle = motionIdle;
        wantIdle = motionIdle || !reachable;
        if (!reachable) {
            return Out.none(motionIdle, wantIdle);
        }
        if (motionIdle) {
            if (reportIdle && lastSendMs >= 0L
                    && sensorMs - lastSendMs < StreamTiming.IDLE_SEND_INTERVAL_MS) {
                return Out.none(motionIdle, wantIdle);
            }
            return Out.emit(rawX, rawY, rawZ, wireGz, reportIdle, motionIdle, wantIdle);
        }
        // Left idle on this sample: the desktop must see it now, not after
        // the driving averager has collected a pair.
        if (wasIdle) {
            drive.reset(sensorMs);
            return Out.emit(rawX, rawY, rawZ, wireGz, false, false, wantIdle);
        }
        if (drive.onSample(sensorMs, rawX, rawY, rawZ, wireGz)) {
            return Out.emit(drive.emitAx(), drive.emitAy(), drive.emitAz(), drive.emitGz(),
                false, false, false);
        }
        return Out.none(false, false);
    }

    private static boolean finite(double v) {
        return Double.isFinite(v);
    }
}
