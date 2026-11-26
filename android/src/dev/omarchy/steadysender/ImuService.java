package dev.omarchy.steadysender;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
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
import android.util.Log;

import java.lang.reflect.Method;
import java.net.DatagramPacket;
import java.util.concurrent.atomic.AtomicInteger;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketTimeoutException;
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
    private static final String LOG_TAG = "SteadySender";
    private static final long TICK_MS = 200L;
    private static final long WAKELOCK_SLICE_MS = 25L * 60L * 1000L;
    private static final long WAKELOCK_RENEW_INTERVAL_MS = 10L * 60L * 1000L;
    private static final int RECV_TIMEOUT_MS = 8;

    private static final byte[] DISCOVER = "{\"cmd\":\"discover\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] CALIBRATE = "{\"cmd\":\"calibrate\"}".getBytes(StandardCharsets.US_ASCII);
    private static final String UNREACHABLE = "desktop not reachable";

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final AtomicInteger networkEpoch = new AtomicInteger();
    private SensorManager sensorManager;
    private ConnectivityManager.NetworkCallback networkCallback;
    private volatile StreamSession activeSession;
    private volatile boolean landscape = false;
    private boolean foregroundStarted = false;
    private String notifHost = "";
    private int notifPort = 8766;
    private String notifDetail = "靜止自動降頻至 1Hz";

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        watchNetwork();
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

        String rawHost = intent.getStringExtra(EXTRA_HOST);
        String host = (rawHost == null) ? "" : rawHost.trim();
        int port = clampPort(intent.getIntExtra(EXTRA_PORT, 8766));
        landscape = intent.getBooleanExtra(EXTRA_LANDSCAPE, false);
        long stopOverride = TimeoutSource.unreachableMs(0L, debugOverride("debug.steady.unreachable_ms"));
        long stillOverride = TimeoutSource.stillMs(0L, debugOverride("debug.steady.still_ms"));

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

        startSession(host, port, stopOverride, stillOverride);
        return START_NOT_STICKY;
    }

    private static int clampPort(int port) {
        return (port >= 1 && port <= 65535) ? port : 8766;
    }

    /**
     * Adb can set {@code debug.steady.unreachable_ms} or {@code debug.steady.still_ms}
     * as a system property, or the same name in {@code settings global}.
     * A normal app cannot write either. Intent extras are not read.
     */
    private String debugOverride(String key) {
        String prop = systemProperty(key);
        if (prop != null && !prop.trim().isEmpty()) return prop.trim();
        try {
            String g = android.provider.Settings.Global.getString(getContentResolver(), key);
            if (g != null && !g.trim().isEmpty()) return g.trim();
        } catch (Throwable ignored) {}
        return "";
    }

    private static String systemProperty(String key) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            Method get = sp.getMethod("get", String.class, String.class);
            Object v = get.invoke(null, key, "");
            return v == null ? "" : v.toString();
        } catch (Throwable ignored) {
            return "";
        }
    }

    private void watchNetwork() {
        ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        if (cm == null || networkCallback != null) return;
        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                networkEpoch.incrementAndGet();
            }

            @Override
            public void onLost(Network network) {
                networkEpoch.incrementAndGet();
            }
        };
        try {
            cm.registerDefaultNetworkCallback(networkCallback);
        } catch (Exception e) {
            networkCallback = null;
        }
    }

    private void unwatchNetwork() {
        if (networkCallback == null) return;
        ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        if (cm != null) {
            try {
                cm.unregisterNetworkCallback(networkCallback);
            } catch (Exception ignored) {}
        }
        networkCallback = null;
    }

    private boolean promoteToForeground(String host, int port) {
        notifHost = host;
        notifPort = port;
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

    private void startSession(String host, int port, long stopOverride, long stillOverride) {
        StreamSession prev = activeSession;
        activeSession = null;
        if (prev != null) {
            prev.releaseWake();
            prev.stopAsync();
        }

        notifHost = host;
        notifPort = port;
        TelemetryState.targetHost = host;
        TelemetryState.targetPort = port;
        TelemetryState.landscape = landscape;
        TelemetryState.lastError = "";
        TelemetryState.packetsSent = 0;
        TelemetryState.hz = 0;
        TelemetryState.running = true;
        TelemetryState.powerSavingIdle = false;
        TelemetryState.desktopReachable = true;
        TelemetryState.powerModeLabel = "行車 12.5Hz";

        StreamSession session = new StreamSession(host, port, stopOverride, stillOverride);
        activeSession = session;
        session.armWake();
        session.start();
    }

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

    private void showDetail(final StreamSession session, final String detail) {
        mainHandler.post(() -> {
            if (activeSession != session || !foregroundStarted) return;
            if (detail.equals(notifDetail)) return;
            notifDetail = detail;
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (nm != null) {
                nm.notify(NOTIF_ID, buildNotification(notifHost, notifPort));
            }
        });
    }

    private void stopStreaming(String autoStopReason) {
        StreamSession prev = activeSession;
        activeSession = null;
        if (prev != null) {
            prev.releaseWake();
            prev.stopAsync();
        }
        TelemetryState.running = false;
        TelemetryState.powerSavingIdle = false;
        TelemetryState.desktopReachable = true;
        TelemetryState.hz = 0;
        TelemetryState.powerModeLabel = "待機";

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
        unwatchNetwork();
        StreamSession prev = activeSession;
        activeSession = null;
        if (prev != null) {
            prev.releaseWake();
            prev.stopAsync();
        }
        TelemetryState.running = false;
        TelemetryState.powerSavingIdle = false;
        TelemetryState.desktopReachable = true;
        TelemetryState.hz = 0;
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
        ch.setDescription("維持手機加速度計與陀螺儀背景傳送給 Steady");
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
            .setContentText("UDP → " + host + ":" + port + " · " + notifDetail)
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

    private final class StreamSession implements SensorEventListener {
        private final String host;
        private final int port;
        private final HandlerThread thread;
        private final Handler handler;
        private final DesktopProbe probe = new DesktopProbe();
        private final StreamOutput output = new StreamOutput();
        private final WakeGate wakeGate = new WakeGate();
        private final Object wakeMu = new Object();
        private final StringBuilder jsonSb = new StringBuilder(144);
        private final byte[] sendBuf = new byte[192];
        private final byte[] recvBuf = new byte[512];

        private DatagramSocket socket;
        private InetAddress targetAddr;
        private String currentIp = "";
        private TargetRefresh.View targetView;
        private int seenNet;
        private PowerManager.WakeLock sessionLock;
        private boolean sessionActive;
        private DatagramPacket samplePacket;
        private DatagramPacket discoverPacket;
        private DatagramPacket recvPacket;

        private Sensor accelSensor;
        private Sensor gyroSensor;
        private boolean stopped = false;
        private boolean gravityIncluded = false;
        private boolean sensorsBound = false;
        private boolean sensorsAreIdle = false;
        private boolean gyroGaveUp = false;
        private boolean motionIdle = false;

        private float latestGyroZ = 0f;
        private long lastWakeRenewMs = 0L;
        private long hzWindowStartMs = 0L;
        private int hzCounter = 0;
        private String lastDetail = "";

        private long firstErrorMs = 0L;
        private long nextRetryMs = 0L;
        private long backoffMs = 1000L;
        private final long stillStopMs;

        StreamSession(String host, int port, long stopOverride, long stillOverride) {
            this.host = host;
            this.port = port;
            this.stillStopMs = StreamTiming.clampStillStop(stillOverride);
            if (stopOverride > 0L) probe.overrideStopAfterMs(stopOverride);
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
                    socket.send(new DatagramPacket(CALIBRATE, CALIBRATE.length, targetAddr, port));
                } catch (Exception e) {
                    TelemetryState.lastError = "校正送出失敗：" + e.getMessage();
                }
            });
        }

        void stopAsync() {
            handler.post(this::teardownOnWorker);
        }

        void armWake() {
            synchronized (wakeMu) {
                sessionActive = true;
                wakeGate.arm();
                acquirePm();
            }
        }

        void releaseWake() {
            synchronized (wakeMu) {
                sessionActive = false;
                wakeGate.release();
                releasePm();
            }
        }

        private void renewWake(boolean earlyPermit) {
            synchronized (wakeMu) {
                if (activeSession != this || !sessionActive) {
                    wakeGate.release();
                    releasePm();
                    return;
                }
                if (!wakeGate.tryRenew(earlyPermit && !stopped)) return;
                acquirePm();
            }
        }

        private void acquirePm() {
            PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
            if (pm == null) return;
            if (sessionLock == null) {
                sessionLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SteadySender:ImuWakeLock");
                sessionLock.setReferenceCounted(false);
            }
            sessionLock.acquire(WAKELOCK_SLICE_MS);
        }

        private void releasePm() {
            if (sessionLock != null && sessionLock.isHeld()) {
                try {
                    sessionLock.release();
                } catch (Exception ignored) {}
            }
            sessionLock = null;
        }

        private void initOnWorker() {
            if (stopped || activeSession != this) return;
            seenNet = networkEpoch.get();
            try {
                clearDns();
                openConnected(InetAddress.getByName(host));
            } catch (Exception e) {
                teardownOnWorker();
                requestSessionStop(this, "網路初始化失敗：" + e.getMessage(), null);
                return;
            }

            if (stopped || activeSession != this) {
                teardownOnWorker();
                return;
            }

            accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION);
            gravityIncluded = false;
            if (accelSensor == null) {
                accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
                gravityIncluded = true;
            }
            gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
            if (accelSensor == null) {
                teardownOnWorker();
                requestSessionStop(this, "裝置找不到加速度計", null);
                return;
            }

            String label = gravityIncluded ? "加速度計" : "線性加速度";
            if (gyroSensor != null) label += " + 陀螺儀";
            TelemetryState.sensorLabel = label;
            Log.i(LOG_TAG, "accel type=" + accelSensor.getType()
                + " grav=" + gravityIncluded
                + " name=" + accelSensor.getName());

            final long now = SystemClock.elapsedRealtime();
            output.setGravityIncluded(gravityIncluded);
            output.reset(now);
            lastWakeRenewMs = now;
            hzWindowStartMs = now;
            hzCounter = 0;
            Log.i(LOG_TAG, "unreachable stop ms=" + probe.stopAfterMs()
                + " still stop ms=" + stillStopMs
                + " target=" + currentIp);

            syncSensorMode(false);
            publishLinkState(false, true);
            handler.post(this::tick);
        }

        private void teardownOnWorker() {
            if (stopped) {
                releaseWake();
                return;
            }
            stopped = true;
            handler.removeCallbacksAndMessages(null);
            if (sensorManager != null) {
                sensorManager.unregisterListener(this);
            }
            sensorsBound = false;
            if (socket != null) {
                try {
                    socket.close();
                } catch (Exception ignored) {}
                socket = null;
            }
            releaseWake();
            thread.quitSafely();
        }

        private void openConnected(InetAddress fresh) throws Exception {
            DatagramSocket neu = new DatagramSocket();
            neu.setSoTimeout(RECV_TIMEOUT_MS);
            try {
                neu.connect(fresh, port);
            } catch (Exception ignored) {}
            String ip = HostCheck.normalizeAddress(fresh.getHostAddress());
            TargetRefresh.View next = TargetRefresh.swap(targetView, ip, port);
            DatagramSocket old = socket;
            socket = neu;
            targetAddr = fresh;
            if (next != null) {
                targetView = next;
                currentIp = next.ip;
            }
            samplePacket = new DatagramPacket(sendBuf, 0, fresh, port);
            discoverPacket = new DatagramPacket(DISCOVER, DISCOVER.length, fresh, port);
            recvPacket = new DatagramPacket(recvBuf, recvBuf.length);
            if (old != null && old != neu) {
                try {
                    old.close();
                } catch (Exception ignored) {}
            }
        }

        private void refreshTarget(boolean networkChanged) {
            if (stopped || !TargetRefresh.shouldResolve(probe.misses(), probe.samplesEnabled(), networkChanged)) {
                return;
            }
            try {
                clearDns();
                InetAddress fresh = InetAddress.getByName(host);
                String ip = HostCheck.normalizeAddress(fresh.getHostAddress());
                if (targetView != null && ip != null && ip.equals(targetView.ip) && socket != null && !socket.isClosed()) {
                    return;
                }
                openConnected(fresh);
                Log.i(LOG_TAG, "target refreshed " + currentIp);
            } catch (Exception e) {
                TelemetryState.lastError = "重新解析失敗：" + e.getMessage();
            }
        }

        private void clearDns() {
            System.setProperty("networkaddress.cache.ttl", "0");
            System.setProperty("networkaddress.cache.negative.ttl", "0");
            try {
                Method m = InetAddress.class.getDeclaredMethod("clearDnsCache");
                m.setAccessible(true);
                m.invoke(null);
            } catch (Throwable ignored) {}
        }

        private void tick() {
            if (stopped) return;
            final long now = SystemClock.elapsedRealtime();
            int net = networkEpoch.get();
            boolean netChanged = net != seenNet;
            seenNet = net;
            refreshTarget(netChanged);
            pumpReceive(now);
            if (probe.due(now)) {
                try {
                    if (socket != null) {
                        socket.send(discoverPacket);
                        String err = TelemetryState.lastError;
                        if (err != null && err.startsWith("探測失敗")) TelemetryState.lastError = "";
                    }
                } catch (Exception e) {
                    TelemetryState.lastError = "探測失敗：" + e.getMessage();
                }
                probe.markSent(now);
            }
            pumpReceive(now);
            if (probe.shouldStop(now)) {
                teardownOnWorker();
                requestSessionStop(this, UNREACHABLE, "桌面端無法連線超過 5 分鐘，已自動停止");
                return;
            }
            syncSensorMode(motionIdle || !probe.samplesEnabled());
            publishLinkState(motionIdle, probe.samplesEnabled());
            if (now - lastWakeRenewMs >= WAKELOCK_RENEW_INTERVAL_MS) {
                lastWakeRenewMs = now;
                renewWake(!stopped);
            }
            rollHz(now);
            if (!stopped) handler.postDelayed(this::tick, TICK_MS);
        }

        private void pumpReceive(long now) {
            if (socket == null || recvPacket == null) return;
            for (int i = 0; i < 4; i++) {
                try {
                    recvPacket.setData(recvBuf);
                    socket.receive(recvPacket);
                    String body = new String(recvBuf, 0, recvPacket.getLength(), StandardCharsets.UTF_8);
                    InetAddress from = recvPacket.getAddress();
                    String fromIp = from == null ? null : from.getHostAddress();
                    ReplyIntake.take(probe, now, currentIp, port, fromIp, recvPacket.getPort(), body);
                } catch (SocketTimeoutException e) {
                    return;
                } catch (Exception e) {
                    return;
                }
            }
        }

        private boolean registerSensor(Sensor sensor, int periodUs, int latencyUs) {
            if (sensor == null || sensorManager == null) return false;
            return sensorManager.registerListener(this, sensor, periodUs, latencyUs, handler);
        }

        private void syncSensorMode(boolean idle) {
            if (stopped || accelSensor == null || sensorManager == null) return;
            if (sensorsBound && sensorsAreIdle == idle) return;
            sensorManager.unregisterListener(this);
            int period = idle ? StreamTiming.IDLE_SAMPLE_PERIOD_US : StreamTiming.SAMPLE_PERIOD_US;
            int latency = idle ? StreamTiming.IDLE_REPORT_LATENCY_US : StreamTiming.DRIVE_REPORT_LATENCY_US;
            boolean gyroWanted = !idle && gyroSensor != null && !gyroGaveUp;
            boolean accelOk = registerSensor(accelSensor, period, latency);
            boolean gyroOk = !gyroWanted || registerSensor(gyroSensor, period, latency);
            boolean accelRetried = false;
            if (!accelOk) {
                accelRetried = true;
                sensorManager.unregisterListener(this);
                accelOk = registerSensor(accelSensor, StreamTiming.BIND_FALLBACK_PERIOD_US, StreamTiming.BIND_FALLBACK_LATENCY_US);
                gyroOk = !gyroWanted || registerSensor(gyroSensor, StreamTiming.BIND_FALLBACK_PERIOD_US, StreamTiming.BIND_FALLBACK_LATENCY_US);
            } else if (gyroWanted && !gyroOk) {
                gyroOk = registerSensor(gyroSensor, StreamTiming.BIND_FALLBACK_PERIOD_US, StreamTiming.BIND_FALLBACK_LATENCY_US);
            }
            SensorBind decision = SensorBind.after(accelOk, gyroWanted, gyroOk, idle, accelRetried);
            Log.i(LOG_TAG, "sensors idle=" + idle + " periodUs=" + period + " latencyUs=" + latency
                + " step=" + decision.step + " accel=" + accelOk + " gyro=" + gyroOk);
            if (decision.step != SensorBind.Step.BOUND || !decision.bound) {
                sensorsBound = false;
                sensorsAreIdle = false;
                String err = decision.error == null ? "無法註冊加速度計" : decision.error;
                teardownOnWorker();
                requestSessionStop(this, err, err);
                return;
            }
            sensorsBound = true;
            sensorsAreIdle = decision.idleModeApplied;
            if (gyroWanted && !decision.gyroOn) gyroGaveUp = true;
            if (decision.error != null) {
                TelemetryState.lastError = decision.error;
                showDetail(this, decision.error);
            }
            if (!idle) output.resetDrive(SystemClock.elapsedRealtime());
        }

        private void publishLinkState(boolean idle, boolean reachable) {
            TelemetryState.powerSavingIdle = idle && reachable;
            TelemetryState.desktopReachable = reachable;
            String detail;
            String mode;
            if (!reachable) {
                detail = UNREACHABLE;
                mode = UNREACHABLE;
                TelemetryState.lastError = UNREACHABLE;
            } else if (gyroGaveUp && !idle) {
                detail = "行車 12.5Hz · 陀螺儀未註冊";
                mode = "行車 12.5Hz · 無陀螺儀";
                if (UNREACHABLE.equals(TelemetryState.lastError)) TelemetryState.lastError = "";
            } else if (idle) {
                detail = "靜止 1Hz · 移動即刻恢復";
                mode = "靜止省電 1Hz";
                if (UNREACHABLE.equals(TelemetryState.lastError)) TelemetryState.lastError = "";
            } else {
                detail = "行車 12.5Hz";
                mode = "行車 12.5Hz";
                if (UNREACHABLE.equals(TelemetryState.lastError)) TelemetryState.lastError = "";
            }
            TelemetryState.powerModeLabel = mode;
            if (!detail.equals(lastDetail)) {
                lastDetail = detail;
                showDetail(this, detail);
            }
        }

        private void rollHz(long now) {
            long dt = now - hzWindowStartMs;
            if (dt >= 1000L) {
                TelemetryState.hz = (int) Math.round((hzCounter * 1000.0) / dt);
                hzCounter = 0;
                hzWindowStartMs = now;
            }
        }

        @Override
        public void onSensorChanged(SensorEvent event) {
            if (stopped || socket == null || event.values == null || event.values.length < 3) return;
            final int type = event.sensor.getType();
            if (type == Sensor.TYPE_GYROSCOPE) {
                latestGyroZ = -event.values[2];
                return;
            }
            if (type != Sensor.TYPE_LINEAR_ACCELERATION && type != Sensor.TYPE_ACCELEROMETER) return;
            long sensorMs = SensorClock.toElapsedMs(event.timestamp);
            double wireGz = latestGyroZ;
            double detectGz = sensorsAreIdle ? 0.0 : wireGz;
            StreamOutput.Out out = output.onSample(
                sensorMs,
                event.values[0], event.values[1], event.values[2],
                detectGz, wireGz,
                landscape, probe.samplesEnabled(), stillStopMs
            );
            applyOut(out, sensorMs);
        }

        private void applyOut(StreamOutput.Out out, long sensorMs) {
            if (stopped || out == null) return;
            if (out.kind == StreamOutput.Kind.STOP_STILL) {
                teardownOnWorker();
                requestSessionStop(this, null, "已靜止 20 分鐘，自動停止串流省電");
                return;
            }
            if (out.kind == StreamOutput.Kind.STOP_SENSOR) {
                teardownOnWorker();
                requestSessionStop(this, out.error, out.error);
                return;
            }
            motionIdle = out.motionIdle;
            syncSensorMode(out.wantIdleSensors);
            if (stopped) return;
            publishLinkState(motionIdle, probe.samplesEnabled());
            if (out.kind != StreamOutput.Kind.EMIT) return;
            if (!probe.samplesEnabled() || nextRetryMs > sensorMs) return;
            emit(sensorMs, out.ax, out.ay, out.az, out.gz, out.idle);
        }

        private void emit(long now, double rawX, double rawY, double rawZ, double gz, boolean idle) {
            if (socket == null || samplePacket == null) return;
            final boolean land = landscape;
            final double ax = land ? -rawY : rawX;
            final double ay = land ? rawX : rawY;
            final double az = rawZ;

            TelemetryState.ax = ax;
            TelemetryState.ay = ay;
            TelemetryState.az = az;
            TelemetryState.gz = gz;

            final int len = SampleJson.encode(sendBuf, jsonSb, ax, ay, az, gz, gravityIncluded, idle);
            if (len <= 0) return;
            samplePacket.setData(sendBuf, 0, len);
            try {
                socket.send(samplePacket);
                output.noteSent(now);
                TelemetryState.packetsSent++;
                hzCounter++;
                if (firstErrorMs != 0L) {
                    firstErrorMs = 0L;
                    nextRetryMs = 0L;
                    backoffMs = 1000L;
                    if (!UNREACHABLE.equals(TelemetryState.lastError)) TelemetryState.lastError = "";
                }
            } catch (Exception e) {
                if (firstErrorMs == 0L) {
                    firstErrorMs = now;
                    backoffMs = 1000L;
                } else if (now - firstErrorMs >= DesktopProbe.UNREACHABLE_STOP_MS) {
                    teardownOnWorker();
                    requestSessionStop(this, "連續 5 分鐘無法連線，已自動停止", "無法連線到電腦，已自動停止省電");
                    return;
                } else {
                    backoffMs = Math.min(backoffMs * 2L, 5000L);
                }
                nextRetryMs = now + backoffMs;
                TelemetryState.lastError = "UDP 傳送重試中 (" + (backoffMs / 1000L) + "s)";
            }
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
        }
    }
}
