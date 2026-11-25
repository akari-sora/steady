package dev.omarchy.steadysender;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.Locale;
import java.util.regex.Pattern;

public class MainActivity extends Activity {
    private static final String PREFS = "steady_sender_prefs";
    private static final String KEY_HOST = "host";
    private static final String KEY_PORT = "port";
    private static final String KEY_LAN_IP = "lan_ip";
    private static final String KEY_TS_IP = "ts_ip";
    private static final String KEY_LANDSCAPE = "landscape";

    private static final Pattern HOST_PATTERN = Pattern.compile("^[a-zA-Z0-9._:-]{1,128}$");

    private static final int COLOR_BG = 0xFF0C0B0C;
    private static final int COLOR_SURFACE = 0xFF181618;
    private static final int COLOR_BORDER = 0xFF2E2A2D;
    private static final int COLOR_ACCENT = 0xFFB59790;
    private static final int COLOR_DANGER = 0xFFD9534F;
    private static final int COLOR_TEXT = 0xFFFAFCFB;
    private static final int COLOR_MUTED = 0xFFA8A0A3;
    private static final int COLOR_BTN_DARK = 0xFF2A2629;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;

    private TextView statusBadge;
    private TextView telemetryText;
    private TextView hzText;
    private TextView errorText;
    private Button toggleBtn;
    private Button calibrateBtn;
    private Button lanChipBtn;
    private Button tsChipBtn;
    private Button discoverBtn;
    private Button batteryBtn;
    private EditText hostInput;
    private EditText portInput;
    private CheckBox landscapeCheck;

    // Pre-allocated drawables so refreshUi() never allocates drawables or forces idle redraws
    private GradientDrawable badgeRunningBg;
    private GradientDrawable badgeIdleBg;
    private GradientDrawable btnStartBg;
    private GradientDrawable btnStopBg;
    private int lastUiState = -1; // -1=uninit, 0=stopped, 1=running-active, 2=running-idle
    private long startPendingUntilMs = 0L;

    private final Runnable pollTask = new Runnable() {
        @Override
        public void run() {
            if (isFinishing() || isDestroyed()) return;
            refreshUi();
            // Poll while running OR briefly during service startup (e.g. cold DNS), zero wakeups while idle!
            if (TelemetryState.running || android.os.SystemClock.elapsedRealtime() < startPendingUntilMs) {
                uiHandler.postDelayed(this, 400L);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        badgeRunningBg = makeRoundedDrawable(0xFF1F3323, dp(99), 0);
        badgeIdleBg = makeRoundedDrawable(0xFF262225, dp(99), 0);
        btnStartBg = makeRoundedDrawable(COLOR_ACCENT, dp(12), 0);
        btnStopBg = makeRoundedDrawable(COLOR_DANGER, dp(12), 0);

        setContentView(buildUi());
        loadPrefs();
        handleDeepLink(getIntent());
        refreshUi();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleDeepLink(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshBatteryButton();
        refreshUi();
        uiHandler.removeCallbacks(pollTask);
        if (TelemetryState.running) {
            uiHandler.postDelayed(pollTask, 400L);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        uiHandler.removeCallbacks(pollTask);
    }

    private void handleDeepLink(Intent intent) {
        if (intent == null) return;
        Uri data = intent.getData();
        if (data == null || !"steady".equals(data.getScheme())) return;

        String host = sanitizeHost(data.getQueryParameter("host"));
        String lan = sanitizeHost(data.getQueryParameter("lan"));
        String ts = sanitizeHost(data.getQueryParameter("ts"));
        String portStr = data.getQueryParameter("port");

        boolean updated = false;
        SharedPreferences.Editor ed = prefs.edit();
        if (lan != null) {
            ed.putString(KEY_LAN_IP, lan);
            updated = true;
        }
        if (ts != null) {
            ed.putString(KEY_TS_IP, ts);
            updated = true;
        }
        if (host != null) {
            hostInput.setText(host);
            ed.putString(KEY_HOST, host);
            updated = true;
        }
        if (portStr != null) {
            try {
                int p = Integer.parseInt(portStr.trim());
                if (p >= 1 && p <= 65535) {
                    portInput.setText(String.valueOf(p));
                    ed.putInt(KEY_PORT, p);
                    updated = true;
                }
            } catch (NumberFormatException ignored) {}
        }

        if (updated) {
            ed.apply();
            updateChipLabels();
            Toast.makeText(this, "已套用連線位址：" + hostInput.getText(), Toast.LENGTH_SHORT).show();
        }
    }

    private static String sanitizeHost(String raw) {
        if (raw == null) return null;
        String s = raw.trim();
        if (s.isEmpty() || !HOST_PATTERN.matcher(s).matches()) return null;
        return s;
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(COLOR_BG);
        scroll.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(22), dp(18), dp(28));
        scroll.addView(root, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        statusBadge = new TextView(this);
        statusBadge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        statusBadge.setTypeface(Typeface.DEFAULT_BOLD);
        statusBadge.setPadding(dp(12), dp(6), dp(12), dp(6));
        LinearLayout.LayoutParams badgeLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        badgeLp.bottomMargin = dp(10);
        root.addView(statusBadge, badgeLp);

        TextView title = new TextView(this);
        title.setText("Steady Sender");
        title.setTextColor(COLOR_TEXT);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("省電背景 IMU 串流（活躍 25Hz · 靜止自動降 1Hz · 20 分鐘自動停止）");
        subtitle.setTextColor(COLOR_MUTED);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        LinearLayout.LayoutParams subLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        subLp.topMargin = dp(4);
        subLp.bottomMargin = dp(16);
        root.addView(subtitle, subLp);

        // Card 1: Main controls + Telemetry
        LinearLayout controlCard = makeCard();
        root.addView(controlCard, cardParams());

        toggleBtn = makeButton("開始背景串流", COLOR_ACCENT, COLOR_BG);
        toggleBtn.setOnClickListener(v -> onToggleClicked());
        controlCard.addView(toggleBtn, buttonParams(dp(10)));

        calibrateBtn = makeButton("一鍵校正原點", COLOR_BTN_DARK, COLOR_TEXT);
        calibrateBtn.setOnClickListener(v -> onCalibrateClicked());
        controlCard.addView(calibrateBtn, buttonParams(dp(12)));

        telemetryText = new TextView(this);
        telemetryText.setTextColor(0xFFCFD3CD);
        telemetryText.setTypeface(Typeface.MONOSPACE);
        telemetryText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        telemetryText.setGravity(Gravity.CENTER);
        controlCard.addView(telemetryText);

        hzText = new TextView(this);
        hzText.setTextColor(COLOR_MUTED);
        hzText.setTypeface(Typeface.MONOSPACE);
        hzText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        hzText.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams hzLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        hzLp.topMargin = dp(4);
        controlCard.addView(hzText, hzLp);

        errorText = new TextView(this);
        errorText.setTextColor(0xFFFF7A7A);
        errorText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        errorText.setGravity(Gravity.CENTER);
        errorText.setVisibility(View.GONE);
        LinearLayout.LayoutParams errLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        errLp.topMargin = dp(6);
        controlCard.addView(errorText, errLp);

        // Card 2: Target Omarchy Connection
        LinearLayout netCard = makeCard();
        root.addView(netCard, cardParams());

        TextView netTitle = new TextView(this);
        netTitle.setText("目標 Omarchy 筆電（UDP）");
        netTitle.setTextColor(COLOR_TEXT);
        netTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        netTitle.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams ntLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        ntLp.bottomMargin = dp(10);
        netCard.addView(netTitle, ntLp);

        LinearLayout chipRow = new LinearLayout(this);
        chipRow.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams chipRowLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        chipRowLp.bottomMargin = dp(10);
        netCard.addView(chipRow, chipRowLp);

        lanChipBtn = makeSmallButton("區網 Wi-Fi", COLOR_BTN_DARK, COLOR_TEXT);
        lanChipBtn.setOnClickListener(v -> {
            String lan = prefs.getString(KEY_LAN_IP, "");
            if (lan.isEmpty()) {
                discoverOmarchy();
            } else {
                hostInput.setText(lan);
                savePrefs();
            }
        });
        LinearLayout.LayoutParams c1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        c1.rightMargin = dp(8);
        chipRow.addView(lanChipBtn, c1);

        tsChipBtn = makeSmallButton("Tailscale", COLOR_BTN_DARK, COLOR_TEXT);
        tsChipBtn.setOnClickListener(v -> {
            String ts = prefs.getString(KEY_TS_IP, "");
            if (ts.isEmpty()) {
                Toast.makeText(this, "尚未記錄 Tailscale IP，請先按自動搜尋或輸入 IP", Toast.LENGTH_SHORT).show();
            } else {
                hostInput.setText(ts);
                savePrefs();
            }
        });
        LinearLayout.LayoutParams c2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        chipRow.addView(tsChipBtn, c2);

        discoverBtn = makeSmallButton("自動搜尋同區網 Omarchy", COLOR_BTN_DARK, COLOR_ACCENT);
        discoverBtn.setOnClickListener(v -> discoverOmarchy());
        netCard.addView(discoverBtn, buttonParams(dp(12)));

        LinearLayout inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        netCard.addView(inputRow, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        hostInput = makeEditText("輸入 Omarchy IP 或點自動搜尋", InputType.TYPE_CLASS_TEXT);
        LinearLayout.LayoutParams hLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2.2f);
        hLp.rightMargin = dp(8);
        inputRow.addView(hostInput, hLp);

        portInput = makeEditText("Port", InputType.TYPE_CLASS_NUMBER);
        LinearLayout.LayoutParams pLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        inputRow.addView(portInput, pLp);

        landscapeCheck = new CheckBox(this);
        landscapeCheck.setText("手機橫放（頂端朝左、充電孔朝右）");
        landscapeCheck.setTextColor(COLOR_MUTED);
        landscapeCheck.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        landscapeCheck.setOnCheckedChangeListener((b, checked) -> {
            savePrefs();
            if (TelemetryState.running) {
                Intent upd = new Intent(this, ImuService.class);
                upd.setAction(ImuService.ACTION_UPDATE_LANDSCAPE);
                upd.putExtra(ImuService.EXTRA_LANDSCAPE, checked);
                startService(upd);
            }
        });
        LinearLayout.LayoutParams cbLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        cbLp.topMargin = dp(10);
        netCard.addView(landscapeCheck, cbLp);

        // Card 3: Battery optimization
        LinearLayout batCard = makeCard();
        root.addView(batCard, cardParams());

        TextView batTitle = new TextView(this);
        batTitle.setText("省電機制與三星 One UI 背景保活");
        batTitle.setTextColor(COLOR_TEXT);
        batTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        batTitle.setTypeface(Typeface.DEFAULT_BOLD);
        batCard.addView(batTitle);

        TextView batDesc = new TextView(this);
        batDesc.setText("行車時以 25Hz + 感應器硬體批次傳送；停紅燈或靜止 4 秒自動降頻至 1Hz（省下 96% 無線電喚醒）；靜止 20 分鐘自動關閉。若擔心三星提前休眠，可點下方放行。");
        batDesc.setTextColor(COLOR_MUTED);
        batDesc.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        LinearLayout.LayoutParams bdLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        bdLp.topMargin = dp(6);
        bdLp.bottomMargin = dp(10);
        batCard.addView(batDesc, bdLp);

        batteryBtn = makeSmallButton("允許背景不受電池最佳化限制", COLOR_BTN_DARK, COLOR_TEXT);
        batteryBtn.setOnClickListener(v -> requestBatteryExclusion());
        batCard.addView(batteryBtn, buttonParams(0));

        return scroll;
    }

    private void loadPrefs() {
        String host = prefs.getString(KEY_HOST, "");
        int port = prefs.getInt(KEY_PORT, 8766);
        boolean land = prefs.getBoolean(KEY_LANDSCAPE, false);
        hostInput.setText(host);
        portInput.setText(String.valueOf(port));
        landscapeCheck.setChecked(land);
        updateChipLabels();
    }

    private void savePrefs() {
        String host = hostInput.getText().toString().trim();
        int port = parsePort();
        prefs.edit()
            .putString(KEY_HOST, host)
            .putInt(KEY_PORT, port)
            .putBoolean(KEY_LANDSCAPE, landscapeCheck.isChecked())
            .apply();
    }

    private void updateChipLabels() {
        String lan = prefs.getString(KEY_LAN_IP, "");
        String ts = prefs.getString(KEY_TS_IP, "");
        lanChipBtn.setText(lan.isEmpty() ? "區網 Wi-Fi\n(點自動搜尋)" : ("區網\n" + lan));
        tsChipBtn.setText(ts.isEmpty() ? "Tailscale\n(尚未記錄)" : ("Tailscale\n" + ts));
    }

    private int parsePort() {
        try {
            int p = Integer.parseInt(portInput.getText().toString().trim());
            return (p >= 1 && p <= 65535) ? p : 8766;
        } catch (Exception e) {
            return 8766;
        }
    }

    private void onToggleClicked() {
        if (TelemetryState.running) {
            Intent stop = new Intent(this, ImuService.class);
            stop.setAction(ImuService.ACTION_STOP);
            try {
                startService(stop);
            } catch (Exception ignored) {}
            uiHandler.removeCallbacks(pollTask);
            uiHandler.postDelayed(this::refreshUi, 150L);
            return;
        }

        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 101);
        }

        String host = sanitizeHost(hostInput.getText().toString());
        if (host == null) {
            Toast.makeText(this, "請先輸入有效的 Omarchy IP，或點「自動搜尋」", Toast.LENGTH_SHORT).show();
            return;
        }
        savePrefs();

        Intent start = new Intent(this, ImuService.class);
        start.setAction(ImuService.ACTION_START);
        start.putExtra(ImuService.EXTRA_HOST, host);
        start.putExtra(ImuService.EXTRA_PORT, parsePort());
        start.putExtra(ImuService.EXTRA_LANDSCAPE, landscapeCheck.isChecked());
        try {
            startForegroundService(start);
        } catch (Exception e) {
            TelemetryState.lastError = "無法啟動背景服務：" + e.getMessage();
            refreshUi();
            return;
        }
        startPendingUntilMs = android.os.SystemClock.elapsedRealtime() + 2500L;
        uiHandler.removeCallbacks(pollTask);
        uiHandler.postDelayed(pollTask, 180L);
    }

    private void onCalibrateClicked() {
        if (TelemetryState.running) {
            Intent cal = new Intent(this, ImuService.class);
            cal.setAction(ImuService.ACTION_CALIBRATE);
            try {
                startService(cal);
                Toast.makeText(this, "已送出原點校正", Toast.LENGTH_SHORT).show();
            } catch (Exception e) {
                Toast.makeText(this, "校正失敗：" + e.getMessage(), Toast.LENGTH_SHORT).show();
            }
            return;
        }
        final String host = sanitizeHost(hostInput.getText().toString());
        final int port = parsePort();
        if (host == null) return;
        new Thread(() -> {
            try (DatagramSocket sock = new DatagramSocket()) {
                byte[] data = "{\"cmd\":\"calibrate\"}".getBytes(StandardCharsets.UTF_8);
                sock.send(new DatagramPacket(data, data.length, InetAddress.getByName(host), port));
                uiHandler.post(() -> {
                    if (!isFinishing() && !isDestroyed()) {
                        Toast.makeText(this, "已送出原點校正", Toast.LENGTH_SHORT).show();
                    }
                });
            } catch (Exception e) {
                uiHandler.post(() -> {
                    if (!isFinishing() && !isDestroyed()) {
                        Toast.makeText(this, "送出失敗：" + e.getMessage(), Toast.LENGTH_SHORT).show();
                    }
                });
            }
        }).start();
    }

    private void discoverOmarchy() {
        discoverBtn.setEnabled(false);
        discoverBtn.setText("搜尋中...");
        final int port = parsePort();

        new Thread(() -> {
            String foundLan = null;
            String foundTs = null;
            try (DatagramSocket sock = new DatagramSocket()) {
                sock.setBroadcast(true);
                sock.setSoTimeout(1200);
                byte[] probe = "{\"cmd\":\"discover\"}".getBytes(StandardCharsets.UTF_8);

                try {
                    sock.send(new DatagramPacket(probe, probe.length, InetAddress.getByName("255.255.255.255"), port));
                } catch (Exception ignored) {}

                Enumeration<NetworkInterface> ifaces = NetworkInterface.getNetworkInterfaces();
                while (ifaces != null && ifaces.hasMoreElements()) {
                    NetworkInterface nif = ifaces.nextElement();
                    if (!nif.isUp() || nif.isLoopback()) continue;
                    for (InterfaceAddress ia : nif.getInterfaceAddresses()) {
                        InetAddress bcast = ia.getBroadcast();
                        if (bcast != null) {
                            try {
                                sock.send(new DatagramPacket(probe, probe.length, bcast, port));
                            } catch (Exception ignored) {}
                        }
                    }
                }

                byte[] buf = new byte[512];
                DatagramPacket resp = new DatagramPacket(buf, buf.length);
                sock.receive(resp);
                String body = new String(resp.getData(), 0, resp.getLength(), StandardCharsets.UTF_8);
                JSONObject obj = new JSONObject(body);
                if (obj.optBoolean("steady", false)) {
                    String lan = sanitizeHost(obj.optString("lan", ""));
                    String replyIp = sanitizeHost(resp.getAddress().getHostAddress());
                    foundLan = (lan != null) ? lan : replyIp;
                    foundTs = sanitizeHost(obj.optString("ts", ""));
                }
            } catch (Exception ignored) {
            }

            final String fLan = foundLan;
            final String fTs = foundTs;
            uiHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                discoverBtn.setEnabled(true);
                discoverBtn.setText("自動搜尋同區網 Omarchy");
                if (fLan != null) {
                    SharedPreferences.Editor ed = prefs.edit().putString(KEY_LAN_IP, fLan);
                    if (fTs != null) {
                        ed.putString(KEY_TS_IP, fTs);
                    }
                    ed.apply();
                    hostInput.setText(fLan);
                    savePrefs();
                    updateChipLabels();
                    Toast.makeText(this, "已找到 Omarchy：" + fLan, Toast.LENGTH_SHORT).show();
                } else {
                    Toast.makeText(this, "區網未收到回應（跨網請用 Tailscale，或確認防火牆放行 UDP 8766）", Toast.LENGTH_LONG).show();
                }
            });
        }).start();
    }

    private void refreshBatteryButton() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        boolean ignoring = pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
        if (ignoring) {
            batteryBtn.setText("✓ 已允許背景不受電池最佳化限制");
            batteryBtn.setTextColor(0xFF8FD694);
        } else {
            batteryBtn.setText("允許背景不受電池最佳化限制");
            batteryBtn.setTextColor(COLOR_TEXT);
        }
    }

    private void requestBatteryExclusion() {
        try {
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            i.setData(Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            } catch (Exception ignored) {}
        }
    }

    private void refreshUi() {
        final boolean running = TelemetryState.running;
        final boolean idle = running && TelemetryState.powerSavingIdle;
        final int uiState = !running ? 0 : (idle ? 2 : 1);

        if (uiState != lastUiState) {
            lastUiState = uiState;
            if (running) {
                statusBadge.setBackground(badgeRunningBg);
                statusBadge.setTextColor(0xFF8FD694);
                statusBadge.setText(idle
                    ? "● 靜止省電中 (1Hz) · 移動即刻恢復"
                    : "● 背景串流中 (25Hz) · 可縮小或關螢幕");
                toggleBtn.setText("停止背景串流");
                toggleBtn.setBackground(btnStopBg);
                toggleBtn.setTextColor(Color.WHITE);
            } else {
                statusBadge.setBackground(badgeIdleBg);
                statusBadge.setTextColor(COLOR_ACCENT);
                statusBadge.setText("○ 待機中");
                toggleBtn.setText("開始背景串流");
                toggleBtn.setBackground(btnStartBg);
                toggleBtn.setTextColor(COLOR_BG);
            }
        }

        telemetryText.setText(String.format(
            Locale.US,
            "ax: %+.2f  |  ay: %+.2f  |  gz: %+.2f",
            TelemetryState.ax, TelemetryState.ay, TelemetryState.gz
        ));

        if (running) {
            hzText.setText(String.format(
                Locale.US,
                "%d Hz · %s · %s · %d 封包",
                TelemetryState.hz, TelemetryState.powerModeLabel,
                TelemetryState.sensorLabel, TelemetryState.packetsSent
            ));
        } else {
            hzText.setText("0 Hz · 按上方按鈕啟動省電前景服務");
        }

        if (TelemetryState.lastError != null && !TelemetryState.lastError.isEmpty()) {
            errorText.setText(TelemetryState.lastError);
            errorText.setVisibility(View.VISIBLE);
        } else {
            errorText.setVisibility(View.GONE);
        }
    }

    private LinearLayout makeCard() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(16), dp(16), dp(16));
        card.setBackground(makeRoundedDrawable(COLOR_SURFACE, dp(14), COLOR_BORDER));
        return card;
    }

    private LinearLayout.LayoutParams cardParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        lp.bottomMargin = dp(14);
        return lp;
    }

    private LinearLayout.LayoutParams buttonParams(int bottomMarginPx) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        lp.bottomMargin = bottomMarginPx;
        return lp;
    }

    private Button makeButton(String text, int bgColor, int textColor) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextColor(textColor);
        b.setPadding(dp(16), dp(14), dp(16), dp(14));
        b.setBackground(makeRoundedDrawable(bgColor, dp(12), 0));
        return b;
    }

    private Button makeSmallButton(String text, int bgColor, int textColor) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextColor(textColor);
        b.setPadding(dp(10), dp(10), dp(10), dp(10));
        b.setBackground(makeRoundedDrawable(bgColor, dp(10), COLOR_BORDER));
        return b;
    }

    private EditText makeEditText(String hint, int inputType) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setHintTextColor(COLOR_MUTED);
        e.setTextColor(COLOR_TEXT);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        e.setTypeface(Typeface.MONOSPACE);
        e.setInputType(inputType);
        e.setSingleLine(true);
        e.setPadding(dp(12), dp(11), dp(12), dp(11));
        e.setBackground(makeRoundedDrawable(COLOR_BG, dp(10), COLOR_BORDER));
        return e;
    }

    private GradientDrawable makeRoundedDrawable(int fillColor, int radiusPx, int strokeColor) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fillColor);
        d.setCornerRadius(radiusPx);
        if (strokeColor != 0) {
            d.setStroke(dp(1), strokeColor);
        }
        return d;
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()
        ));
    }
}
