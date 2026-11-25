package dev.omarchy.steadysender;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;

public class ImuService extends Service {
    public static final String ACTION_START = "dev.omarchy.steadysender.START";
    public static final String ACTION_STOP = "dev.omarchy.steadysender.STOP";
    public static final String ACTION_CALIBRATE = "dev.omarchy.steadysender.CALIBRATE";
    public static final String ACTION_UPDATE_LANDSCAPE = "dev.omarchy.steadysender.UPDATE_LANDSCAPE";

    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";
    public static final String EXTRA_LANDSCAPE = "landscape";

    private static final String CHANNEL_ID = "steady_imu_stream";
    private static final int NOTIF_ID = 8766;
    private static final int AUTOSTOP_NOTIF_ID = 8767;

    // Power-optimized timing constants:
    // Desktop applies a ~3 Hz vestibular low-pass filter, so 25 Hz active streaming with
    // 40 ms hardware FIFO batching is visually identical to 55 Hz while cutting radio + CPU
    // wakeups by ~55%. When stationary >= 4s, TX drops to 1 Hz keepalive (-96% radio wakeups).
    private static final int SAMPLE_PERIOD_US = 40_000;        // 25 Hz
    private static final int MAX_REPORT_LATENCY_US = 40_000;   // 40 ms sensor-hub FIFO batch
    private static final long ACTIVE_SEND_INTERVAL_MS = 38L;   // ~25 Hz
    private static final long IDLE_SEND_INTERVAL_MS = 1000L;   // 1 Hz keepalive when stationary
    private static final long IDLE_ENTER_MS = 4_000L;          // enter 1 Hz after 4s stillness
    private static final long AUTO_STOP_STILL_MS = 20L * 60L * 1000L; // auto-stop after 20 min still
    private static final long MAX_ERROR_DURATION_MS = 5L * 60L * 1000L; // auto-stop after 5 min offline
    private static final long WAKELOCK_SLICE_MS = 25L * 60L * 1000L;    // bounded WakeLock slice
    private static final long WAKELOCK_RENEW_INTERVAL_MS = 10L * 60L * 1000L;

    private static final float MOTION_ACCEL_THRESHOLD = 0.22f; // m/s^2
    private static final float MOTION_GYRO_THRESHOLD = 0.08f;  // rad/s

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private SensorManager sensorManager;
    private PowerManager.WakeLock wakeLock;
    private volatile StreamSession activeSession;
    private volatile boolean landscape = false;
    private boolean foregroundStarted = false;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopStreaming(null);
            return START_NOT_STICKY;
        }

        final String action = intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopStreaming(null);
            return START_NOT_STICKY;
        }

        if (ACTION_UPDATE_LANDSCAPE.equals(action)) {
            landscape = intent.getBooleanExtra(EXTRA_LANDSCAPE, false);
            TelemetryState.landscape = landscape;
            if (activeSession == null) {
                stopSelf();
                return START_NOT_STICKY;
            }
            return START_NOT_STICKY;
        }

        if (ACTION_CALIBRATE.equals(action)) {
            StreamSession session = activeSession;
            if (session != null) {
                session.sendCalibrate();
            } else {
                stopSelf();
            }
            return START_NOT_STICKY;
        }

        // ACTION_START (or default): always call startForeground() defensively before any early stop
        // to satisfy Android 8-17 startForegroundService() contract and avoid crash.
        String rawHost = intent.getStringExtra(EXTRA_HOST);
        String host = (rawHost == null) ? "" : rawHost.trim();
        int port = clampPort(intent.getIntExtra(EXTRA_PORT, 8766));
        landscape = intent.getBooleanExtra(EXTRA_LANDSCAPE, false);

        if (!promoteToForeground(host.isEmpty() ? "未設定" : host, port)) {
            TelemetryState.running = false;
            stopSelf();
            return START_NOT_STICKY;
        }

        if (host.isEmpty()) {
            TelemetryState.lastError = "未設定目標 IP";
            stopStreaming(null);
            return START_NOT_STICKY;
        }

        startSession(host, port);
        return START_NOT_STICKY;
    }

    private static int clampPort(int port) {
        return (port >= 1 && port <= 65535) ? port : 8766;
    }

    private boolean promoteToForeground(String host, int port) {
        Notification notif = buildNotification(host, port);
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else {
                startForeground(NOTIF_ID, notif);
            }
            foregroundStarted = true;
            return true;
        } catch (Exception e) {
            TelemetryState.lastError = "無法啟動前景服務：" + e.getMessage();
            return false;
        }
    }

    private void startSession(String host, int port) {
        StreamSession prev = activeSession;
        activeSession = null;
        if (prev != null) {
            prev.stopAsync();
        }

        TelemetryState.targetHost = host;
        TelemetryState.targetPort = port;
        TelemetryState.landscape = landscape;
        TelemetryState.lastError = "";
        TelemetryState.packetsSent = 0;
        TelemetryState.hz = 0;
        TelemetryState.running = true;
        TelemetryState.powerSavingIdle = false;
        TelemetryState.powerModeLabel = "活躍 25Hz";

        acquireOrRenewWakeLock();

        StreamSession session = new StreamSession(host, port);
        activeSession = session;
        session.start();
    }

    /**
     * Marshals worker-initiated stops onto the main thread and verifies session ownership so
     * a stale session failing late can never tear down a newly started healthy session.
     */
    private void requestSessionStop(final StreamSession session, final String errorMsg, final String autoStopReason) {
        mainHandler.post(() -> {
            if (activeSession != session) {
                return;
            }
            if (errorMsg != null && !errorMsg.isEmpty()) {
                TelemetryState.lastError = errorMsg;
            }
            stopStreaming(autoStopReason);
        });
    }

    private synchronized void acquireOrRenewWakeLock() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm == null) return;
        if (wakeLock == null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SteadySender:ImuWakeLock");
            wakeLock.setReferenceCounted(false);
        }
        // Bounded WakeLock prevents permanent battery drain if anything stalls unexpectedly.
        wakeLock.acquire(WAKELOCK_SLICE_MS);
    }

    private synchronized void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) {
            try {
                wakeLock.release();
            } catch (Exception ignored) {}
        }
        wakeLock = null;
    }

    private void stopStreaming(String autoStopReason) {
        StreamSession prev = activeSession;
        activeSession = null;
        if (prev != null) {
            prev.stopAsync();
        }
        TelemetryState.running = false;
        TelemetryState.powerSavingIdle = false;
        TelemetryState.hz = 0;
        TelemetryState.powerModeLabel = "待機";

        releaseWakeLock();

        if (foregroundStarted) {
            try {
                stopForeground(STOP_FOREGROUND_REMOVE);
            } catch (Exception ignored) {}
            foregroundStarted = false;
        }

        if (autoStopReason != null && !autoStopReason.isEmpty()) {
            postAutoStopNotification(autoStopReason);
        }

        stopSelf();
    }

    @Override
    public void onDestroy() {
        StreamSession prev = activeSession;
        activeSession = null;
        if (prev != null) {
            prev.stopAsync();
        }
        TelemetryState.running = false;
        TelemetryState.powerSavingIdle = false;
        TelemetryState.hz = 0;
        releaseWakeLock();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void createNotificationChannel() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(
            CHANNEL_ID,
            "Steady IMU 背景串流",
            NotificationManager.IMPORTANCE_LOW
        );
        ch.setDescription("維持手機陀螺儀與加速度計背景傳送給 Omarchy Steady");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification(String host, int port) {
        Intent openIntent = new Intent(this, MainActivity.class);
        openIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent openPi = PendingIntent.getActivity(
            this, 0, openIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        Intent calIntent = new Intent(this, ImuService.class);
        calIntent.setAction(ACTION_CALIBRATE);
        PendingIntent calPi = PendingIntent.getService(
            this, 1, calIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        Intent stopIntent = new Intent(this, ImuService.class);
        stopIntent.setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(
            this, 2, stopIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        return new Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_steady)
            .setContentTitle("Steady 背景串流中（省電自適應）")
            .setContentText("UDP → " + host + ":" + port + " · 靜止自動降頻至 1Hz")
            .setOngoing(true)
            .setContentIntent(openPi)
            .addAction(new Notification.Action.Builder(null, "校正原點", calPi).build())
            .addAction(new Notification.Action.Builder(null, "停止", stopPi).build())
            .build();
    }

    private void postAutoStopNotification(String reason) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        Intent openIntent = new Intent(this, MainActivity.class);
        PendingIntent openPi = PendingIntent.getActivity(
            this, 3, openIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );
        Notification n = new Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_steady)
            .setContentTitle("Steady 已自動停止省電")
            .setContentText(reason)
            .setAutoCancel(true)
            .setContentIntent(openPi)
            .build();
        try {
            nm.notify(AUTOSTOP_NOTIF_ID, n);
        } catch (Exception ignored) {}
    }

    /**
     * Self-contained per-session worker: owns its own HandlerThread, DatagramSocket,
     * pre-allocated send buffers, and sensor listener registration so start/stop/restart
     * never race across threads.
     */
    private final class StreamSession implements SensorEventListener {
        private final String host;
        private final int port;
        private final HandlerThread thread;
        private final Handler handler;

        // Reusable zero-allocation hot-loop buffers (accessed exclusively on worker thread)
        private final StringBuilder sb = new StringBuilder(144);
        private final byte[] sendBuf = new byte[192];
        private DatagramSocket socket;
        private InetAddress targetAddr;
        private DatagramPacket packet;

        private boolean stopped = false;
        private boolean gravityIncluded = false;
        private boolean hasPrevRaw = false;
        private float prevRawX = 0f, prevRawY = 0f, prevRawZ = 0f;
        private float latestGyroZ = 0f;

        private long lastSendMs = 0L;
        private long lastMotionMs = 0L;
        private long lastWakeRenewMs = 0L;
        private long hzWindowStartMs = 0L;
        private int hzCounter = 0;

        // Send error exponential backoff
        private long firstErrorMs = 0L;
        private long nextRetryMs = 0L;
        private long backoffMs = 1000L;

        StreamSession(String host, int port) {
            this.host = host;
            this.port = port;
            this.thread = new HandlerThread("SteadyImuWorker");
            this.thread.start();
            this.handler = new Handler(this.thread.getLooper());
        }

        void start() {
            handler.post(this::initOnWorker);
        }

        void sendCalibrate() {
            handler.post(() -> {
                if (stopped || socket == null || targetAddr == null) return;
                try {
                    byte[] data = "{\"cmd\":\"calibrate\"}".getBytes(StandardCharsets.UTF_8);
                    socket.send(new DatagramPacket(data, data.length, targetAddr, port));
                } catch (Exception e) {
                    TelemetryState.lastError = "校正送出失敗：" + e.getMessage();
                }
            });
        }

        void stopAsync() {
            handler.post(this::teardownOnWorker);
        }

        private void initOnWorker() {
            if (stopped || activeSession != this) return;
            try {
                targetAddr = InetAddress.getByName(host);
                socket = new DatagramSocket();
                packet = new DatagramPacket(sendBuf, 0, targetAddr, port);
            } catch (Exception e) {
                teardownOnWorker();
                requestSessionStop(this, "網路初始化失敗：" + e.getMessage(), null);
                return;
            }

            if (stopped || activeSession != this) {
                teardownOnWorker();
                return;
            }

            Sensor accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION);
            gravityIncluded = false;
            if (accelSensor == null) {
                accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
                gravityIncluded = true;
            }
            Sensor gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);

            if (accelSensor == null) {
                teardownOnWorker();
                requestSessionStop(this, "裝置找不到加速度計", null);
                return;
            }

            String label = gravityIncluded ? "加速度計" : "線性加速度";
            if (gyroSensor != null) {
                label += " + 陀螺儀";
            }
            TelemetryState.sensorLabel = label;

            final long now = SystemClock.elapsedRealtime();
            lastMotionMs = now;
            lastWakeRenewMs = now;
            hzWindowStartMs = now;
            hzCounter = 0;
            lastSendMs = 0L;

            sensorManager.registerListener(this, accelSensor, SAMPLE_PERIOD_US, MAX_REPORT_LATENCY_US, handler);
            if (gyroSensor != null) {
                sensorManager.registerListener(this, gyroSensor, SAMPLE_PERIOD_US, MAX_REPORT_LATENCY_US, handler);
            }
        }

        private void teardownOnWorker() {
            if (stopped) return;
            stopped = true;
            if (sensorManager != null) {
                sensorManager.unregisterListener(this);
            }
            if (socket != null) {
                try {
                    socket.close();
                } catch (Exception ignored) {}
                socket = null;
            }
            thread.quitSafely();
        }

        @Override
        public void onSensorChanged(SensorEvent event) {
            if (stopped || socket == null || packet == null) return;

            final int type = event.sensor.getType();
            if (type == Sensor.TYPE_GYROSCOPE) {
                // Negate Android +Z (CCW) so right turn matches +X lateral cue.
                latestGyroZ = -event.values[2];
                return;
            }

            if (type != Sensor.TYPE_LINEAR_ACCELERATION && type != Sensor.TYPE_ACCELEROMETER) {
                return;
            }

            final long now = SystemClock.elapsedRealtime();
            final float rawX = event.values[0];
            final float rawY = event.values[1];
            final float rawZ = event.values[2];

            // Motion detection for adaptive radio power-saving
            float motionMag;
            if (gravityIncluded) {
                if (!hasPrevRaw) {
                    prevRawX = rawX; prevRawY = rawY; prevRawZ = rawZ;
                    hasPrevRaw = true;
                    motionMag = 0f;
                } else {
                    motionMag = Math.max(
                        Math.abs(rawX - prevRawX),
                        Math.max(Math.abs(rawY - prevRawY), Math.abs(rawZ - prevRawZ))
                    );
                    prevRawX = prevRawX * 0.85f + rawX * 0.15f;
                    prevRawY = prevRawY * 0.85f + rawY * 0.15f;
                    prevRawZ = prevRawZ * 0.85f + rawZ * 0.15f;
                }
            } else {
                motionMag = Math.max(Math.abs(rawX), Math.abs(rawY));
            }

            if (motionMag > MOTION_ACCEL_THRESHOLD || Math.abs(latestGyroZ) > MOTION_GYRO_THRESHOLD) {
                lastMotionMs = now;
                if (now - lastWakeRenewMs >= WAKELOCK_RENEW_INTERVAL_MS) {
                    lastWakeRenewMs = now;
                    acquireOrRenewWakeLock();
                }
            }

            final long stillMs = now - lastMotionMs;
            if (stillMs >= AUTO_STOP_STILL_MS) {
                teardownOnWorker();
                requestSessionStop(this, null, "已靜止 20 分鐘，自動停止串流省電");
                return;
            }

            final boolean isIdle = stillMs >= IDLE_ENTER_MS;
            if (TelemetryState.powerSavingIdle != isIdle) {
                TelemetryState.powerSavingIdle = isIdle;
                TelemetryState.powerModeLabel = isIdle ? "靜止省電 1Hz" : "活躍 25Hz";
            }

            final long sendInterval = isIdle ? IDLE_SEND_INTERVAL_MS : ACTIVE_SEND_INTERVAL_MS;
            if (now - lastSendMs < sendInterval) {
                return;
            }
            if (nextRetryMs > now) {
                return;
            }
            lastSendMs = now;

            final boolean isLand = landscape;
            final double ax = isLand ? -rawY : rawX;
            final double ay = isLand ? rawX : rawY;
            final double az = rawZ;
            final double gz = latestGyroZ;

            TelemetryState.ax = ax;
            TelemetryState.ay = ay;
            TelemetryState.az = az;
            TelemetryState.gz = gz;

            final int len = encodeJsonAscii(ax, ay, az, gz, gravityIncluded);
            if (len <= 0) return;

            packet.setData(sendBuf, 0, len);
            try {
                socket.send(packet);
                TelemetryState.packetsSent++;
                hzCounter++;
                if (firstErrorMs != 0L) {
                    firstErrorMs = 0L;
                    nextRetryMs = 0L;
                    backoffMs = 1000L;
                    TelemetryState.lastError = "";
                }
            } catch (Exception e) {
                if (firstErrorMs == 0L) {
                    firstErrorMs = now;
                    backoffMs = 1000L;
                } else if (now - firstErrorMs >= MAX_ERROR_DURATION_MS) {
                    teardownOnWorker();
                    requestSessionStop(this, "連續 5 分鐘無法連線，已自動停止", "無法連線到 Omarchy，已自動停止省電");
                    return;
                } else {
                    backoffMs = Math.min(backoffMs * 2L, 5000L);
                }
                nextRetryMs = now + backoffMs;
                TelemetryState.lastError = "UDP 傳送重試中 (" + (backoffMs / 1000L) + "s)";
            }

            final long dtMs = now - hzWindowStartMs;
            if (dtMs >= 1000L) {
                TelemetryState.hz = (int) Math.round((hzCounter * 1000.0) / Math.max(dtMs, 1L));
                hzCounter = 0;
                hzWindowStartMs = now;
            }
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
            // No-op
        }

        /**
         * Allocation-free ASCII JSON encoder writing directly into sendBuf.
         */
        private int encodeJsonAscii(double ax, double ay, double az, double gz, boolean grav) {
            sb.setLength(0);
            sb.append("{\"ax\":");
            appendFixed3(sb, ax);
            sb.append(",\"ay\":");
            appendFixed3(sb, ay);
            sb.append(",\"az\":");
            appendFixed3(sb, az);
            sb.append(",\"gz\":");
            appendFixed3(sb, gz);
            sb.append(",\"grav\":");
            sb.append(grav ? "true" : "false");
            sb.append(",\"src\":\"Steady Android\"}");

            final int n = sb.length();
            if (n > sendBuf.length) return 0;
            for (int i = 0; i < n; i++) {
                sendBuf[i] = (byte) sb.charAt(i);
            }
            return n;
        }

        private void appendFixed3(StringBuilder out, double v) {
            if (!Double.isFinite(v)) {
                out.append("0");
                return;
            }
            if (v > 1000.0) v = 1000.0;
            else if (v < -1000.0) v = -1000.0;
            long scaled = Math.round(v * 1000.0);
            if (scaled < 0) {
                out.append('-');
                scaled = -scaled;
            }
            long whole = scaled / 1000L;
            long frac = scaled % 1000L;
            out.append(whole).append('.');
            if (frac < 100L) out.append('0');
            if (frac < 10L) out.append('0');
            out.append(frac);
        }
    }
}
