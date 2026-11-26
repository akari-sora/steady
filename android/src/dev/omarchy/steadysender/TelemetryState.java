package dev.omarchy.steadysender;

public final class TelemetryState {
    public static volatile boolean running = false;
    public static volatile boolean powerSavingIdle = false;
    public static volatile boolean desktopReachable = true;
    public static volatile String targetHost = "";
    public static volatile int targetPort = 8766;
    public static volatile boolean landscape = false;

    public static volatile double ax = 0.0;
    public static volatile double ay = 0.0;
    public static volatile double az = 0.0;
    public static volatile double gz = 0.0;
    public static volatile int hz = 0;
    public static volatile long packetsSent = 0;
    public static volatile String sensorLabel = "待機中";
    public static volatile String powerModeLabel = "待機";
    public static volatile String lastError = "";

    private TelemetryState() {}
}
