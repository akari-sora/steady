package dev.omarchy.steadysender;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
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
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.Locale;

public class MainActivity extends Activity {
    private static final String PREFS = "steady_sender_prefs";
    private static final String KEY_HOST = "host";
    private static final String KEY_PORT = "port";
    private static final String KEY_LAN_IP = "lan_ip";
    private static final String KEY_TS_IP = "ts_ip";
    private static final String KEY_LANDSCAPE = "landscape";

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
    private AlertDialog confirmDialog;
    private DeepLinkRequest pendingConfirm;
    private String linkNote = "";

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
        showPendingConfirm();
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

    @Override
    protected void onDestroy() {
        uiHandler.removeCallbacks(pollTask);
        if (confirmDialog != null) {
            confirmDialog.dismiss();
            confirmDialog = null;
        }
        super.onDestroy();
    }

    private void handleDeepLink(Intent intent) {
        if (intent == null || isFinishing()) return;
        try {
            applyDeepLink(intent);
        } catch (RuntimeException e) {
            pendingConfirm = null;
            linkNote = getString(R.string.toast_link_rejected);
            try {
                Toast.makeText(this, linkNote, Toast.LENGTH_LONG).show();
                refreshUi();
            } catch (RuntimeException ignored) {}
        }
    }

    private void applyDeepLink(Intent intent) {
        Uri data = intent.getData();
        if (data == null) return;
        boolean hierarchical = false;
        String scheme = null;
        String uriHost = null;
        String host = null;
        String port = null;
        String lan = null;
        String ts = null;
        boolean queryUnreadable = false;
        try {
            scheme = data.getScheme();
            hierarchical = data.isHierarchical();
            if (!hierarchical) {
                queryUnreadable = true;
            } else {
                uriHost = data.getHost();
                host = data.getQueryParameter("host");
                port = data.getQueryParameter("port");
                lan = data.getQueryParameter("lan");
                ts = data.getQueryParameter("ts");
            }
        } catch (RuntimeException e) {
            queryUnreadable = true;
            hierarchical = false;
        }
        final DeepLinkRequest req = DeepLinkRequest.parse(
            hierarchical, scheme, uriHost, host, port, lan, ts, queryUnreadable
        );
        if (req == null) return;
        if (!req.ok()) {
            pendingConfirm = null;
            linkNote = getString(R.string.toast_link_rejected_named, linkErrorText(req.error));
            Toast.makeText(this, linkNote, Toast.LENGTH_LONG).show();
            refreshUi();
            return;
        }
        pendingConfirm = req;
        if (hasWindowFocus()) showPendingConfirm();
    }

    /** Localised text for a DeepLinkRequest rejection code ("bad uri"/"bad host"/"bad port"). */
    private String linkErrorText(String code) {
        if ("bad host".equals(code)) return getString(R.string.error_link_bad_host);
        if ("bad port".equals(code)) return getString(R.string.error_link_bad_port);
        if ("bad uri".equals(code)) return getString(R.string.error_link_bad_uri);
        return code;
    }

    private void showPendingConfirm() {
        final DeepLinkRequest req = pendingConfirm;
        if (req == null || isFinishing()) return;
        String curHost = hostInput.getText().toString().trim();
        String curPort = portInput.getText().toString().trim();
        String curLan = prefs.getString(KEY_LAN_IP, "");
        String curTs = prefs.getString(KEY_TS_IP, "");
        if (!req.applyAllowed(curHost, curPort, curLan, curTs)) {
            pendingConfirm = null;
            linkNote = getString(R.string.toast_link_incomplete);
            Toast.makeText(this, linkNote, Toast.LENGTH_LONG).show();
            refreshUi();
            return;
        }
        pendingConfirm = null;
        final String msg = req.confirmMessage(curHost, curPort, curLan, curTs,
            getString(R.string.empty_label));
        if (confirmDialog != null) confirmDialog.dismiss();
        confirmDialog = new AlertDialog.Builder(this)
            .setTitle(getString(R.string.dialog_confirm_target))
            .setMessage(msg)
            .setPositiveButton(getString(R.string.dialog_confirm), (d, w) -> {
                if (req.disclosed(msg)) applyConfirmed(req);
            })
            .setNegativeButton(getString(R.string.dialog_cancel), (d, w) -> { })
            .create();
        confirmDialog.show();
    }

    private void applyConfirmed(DeepLinkRequest req) {
        if (isFinishing() || isDestroyed()) return;
        if (req.host != null) hostInput.setText(req.host);
        if (req.port > 0) portInput.setText(String.valueOf(req.port));
        SharedPreferences.Editor ed = prefs.edit();
        if (req.lan != null) ed.putString(KEY_LAN_IP, req.lan);
        if (req.ts != null) ed.putString(KEY_TS_IP, req.ts);
        ed.apply();
        savePrefs();
        updateChipLabels();
        linkNote = "";
        Toast.makeText(this, getString(R.string.toast_changed_to, hostInput.getText()), Toast.LENGTH_SHORT).show();
        if (TelemetryState.running && (req.host != null || req.port > 0)) {
            restartStream();
        }
        refreshUi();
    }

    /** Chip switches and discovery both show the destination before writing it. */
    private void confirmHost(String host, String label) {
        if (host == null) return;
        final String next = host.trim();
        if (next.isEmpty()) return;
        final String cur = hostInput.getText().toString().trim();
        final String port = portInput.getText().toString().trim();
        final String msg = label
            + "\nhost: " + (cur.isEmpty() ? getString(R.string.empty_label) : cur) + " → " + next
            + "\nport: " + port;
        new AlertDialog.Builder(this)
            .setTitle(getString(R.string.dialog_confirm_target))
            .setMessage(msg)
            .setPositiveButton(getString(R.string.dialog_confirm), (d, w) -> {
                hostInput.setText(next);
                savePrefs();
                updateChipLabels();
                if (TelemetryState.running) restartStream();
            })
            .setNegativeButton(getString(R.string.dialog_cancel), null)
            .show();
    }

    private void restartStream() {
        String host = sanitizeHost(hostInput.getText().toString());
        if (host == null) return;
        Intent start = new Intent(this, ImuService.class);
        start.setAction(ImuService.ACTION_START);
        start.putExtra(ImuService.EXTRA_HOST, host);
        start.putExtra(ImuService.EXTRA_PORT, parsePort());
        start.putExtra(ImuService.EXTRA_LANDSCAPE, landscapeCheck.isChecked());
        try {
            startForegroundService(start);
        } catch (Exception e) {
            TelemetryState.lastError = getString(R.string.error_retarget, e.getMessage());
        }
    }

    private static String sanitizeHost(String raw) {
        if (raw == null) return null;
        String s = raw.trim();
        if (!HostCheck.isUsableSource(s)) return null;
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
        subtitle.setText(getString(R.string.app_subtitle));
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

        toggleBtn = makeButton(getString(R.string.btn_start_stream), COLOR_ACCENT, COLOR_BG);
        toggleBtn.setOnClickListener(v -> onToggleClicked());
        controlCard.addView(toggleBtn, buttonParams(dp(10)));

        calibrateBtn = makeButton(getString(R.string.btn_calibrate), COLOR_BTN_DARK, COLOR_TEXT);
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
        netTitle.setText(getString(R.string.net_title));
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

        lanChipBtn = makeSmallButton(getString(R.string.chip_lan_btn), COLOR_BTN_DARK, COLOR_TEXT);
        lanChipBtn.setOnClickListener(v -> {
            String lan = prefs.getString(KEY_LAN_IP, "");
            if (lan.isEmpty()) {
                discoverOmarchy();
            } else {
                confirmHost(lan, getString(R.string.confirm_lan_label));
            }
        });
        LinearLayout.LayoutParams c1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        c1.rightMargin = dp(8);
        chipRow.addView(lanChipBtn, c1);

        tsChipBtn = makeSmallButton(getString(R.string.chip_tailscale), COLOR_BTN_DARK, COLOR_TEXT);
        tsChipBtn.setOnClickListener(v -> {
            String ts = prefs.getString(KEY_TS_IP, "");
            if (ts.isEmpty()) {
                Toast.makeText(this, getString(R.string.toast_no_tailscale), Toast.LENGTH_SHORT).show();
            } else {
                confirmHost(ts, getString(R.string.chip_tailscale));
            }
        });
        LinearLayout.LayoutParams c2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        chipRow.addView(tsChipBtn, c2);

        discoverBtn = makeSmallButton(getString(R.string.btn_discover), COLOR_BTN_DARK, COLOR_ACCENT);
        discoverBtn.setOnClickListener(v -> discoverOmarchy());
        netCard.addView(discoverBtn, buttonParams(dp(12)));

        LinearLayout inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        netCard.addView(inputRow, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        hostInput = makeEditText(getString(R.string.hint_host), InputType.TYPE_CLASS_TEXT);
        LinearLayout.LayoutParams hLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2.2f);
        hLp.rightMargin = dp(8);
        inputRow.addView(hostInput, hLp);

        portInput = makeEditText(getString(R.string.hint_port), InputType.TYPE_CLASS_NUMBER);
        LinearLayout.LayoutParams pLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        inputRow.addView(portInput, pLp);

        landscapeCheck = new CheckBox(this);
        landscapeCheck.setText(getString(R.string.check_landscape));
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
        batTitle.setText(getString(R.string.bat_title));
        batTitle.setTextColor(COLOR_TEXT);
        batTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        batTitle.setTypeface(Typeface.DEFAULT_BOLD);
        batCard.addView(batTitle);

        TextView batDesc = new TextView(this);
        batDesc.setText(getString(R.string.bat_desc));
        batDesc.setTextColor(COLOR_MUTED);
        batDesc.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        LinearLayout.LayoutParams bdLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        );
        bdLp.topMargin = dp(6);
        bdLp.bottomMargin = dp(10);
        batCard.addView(batDesc, bdLp);

        batteryBtn = makeSmallButton(getString(R.string.btn_battery_allow), COLOR_BTN_DARK, COLOR_TEXT);
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
        lanChipBtn.setText(lan.isEmpty() ? getString(R.string.chip_lan_empty) : getString(R.string.chip_lan, lan));
        tsChipBtn.setText(ts.isEmpty() ? getString(R.string.chip_ts_empty) : getString(R.string.chip_ts, ts));
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
        linkNote = "";
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
            Toast.makeText(this, getString(R.string.toast_need_host), Toast.LENGTH_SHORT).show();
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
            TelemetryState.lastError = getString(R.string.error_start_service, e.getMessage());
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
                Toast.makeText(this, getString(R.string.toast_calibrated), Toast.LENGTH_SHORT).show();
            } catch (Exception e) {
                Toast.makeText(this, getString(R.string.toast_calibrate_failed, e.getMessage()), Toast.LENGTH_SHORT).show();
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
                        Toast.makeText(this, getString(R.string.toast_calibrated), Toast.LENGTH_SHORT).show();
                    }
                });
            } catch (Exception e) {
                uiHandler.post(() -> {
                    if (!isFinishing() && !isDestroyed()) {
                        Toast.makeText(this, getString(R.string.toast_send_failed, e.getMessage()), Toast.LENGTH_SHORT).show();
                    }
                });
            }
        }).start();
    }

    private void discoverOmarchy() {
        discoverBtn.setEnabled(false);
        discoverBtn.setText(getString(R.string.btn_searching));
        final int port = parsePort();

        new Thread(() -> {
            ArrayList<DiscoveryCommit> found = new ArrayList<>();
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

                long deadline = System.currentTimeMillis() + 1200L;
                byte[] buf = new byte[512];
                while (System.currentTimeMillis() < deadline) {
                    int left = (int) (deadline - System.currentTimeMillis());
                    if (left <= 0) break;
                    sock.setSoTimeout(left);
                    DatagramPacket resp = new DatagramPacket(buf, buf.length);
                    try {
                        sock.receive(resp);
                    } catch (Exception end) {
                        break;
                    }
                    try {
                        String body = new String(resp.getData(), 0, resp.getLength(), StandardCharsets.UTF_8);
                        JSONObject obj = new JSONObject(body);
                        if (!obj.optBoolean("steady", false)) continue;
                        String source = resp.getAddress() == null ? null : resp.getAddress().getHostAddress();
                        DiscoveryCommit offer = DiscoveryCommit.offer(
                            DiscoveryChoice.target(source, obj.optString("lan", "")),
                            obj.optString("ts", "")
                        );
                        if (offer == null || offer.persist()) continue;
                        boolean dup = false;
                        for (int i = 0; i < found.size(); i++) {
                            if (found.get(i).host.equals(offer.host)) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) found.add(offer);
                    } catch (Exception ignored) {}
                }
            } catch (Exception ignored) {
            }

            final ArrayList<DiscoveryCommit> offers = found;
            uiHandler.post(() -> presentDiscovery(offers));
        }).start();
    }

    private void presentDiscovery(ArrayList<DiscoveryCommit> offers) {
        if (isFinishing() || isDestroyed()) return;
        discoverBtn.setEnabled(true);
        discoverBtn.setText(getString(R.string.btn_discover));
        if (offers.isEmpty()) {
            Toast.makeText(this, getString(R.string.toast_no_response), Toast.LENGTH_LONG).show();
            return;
        }
        if (offers.size() == 1) {
            confirmDiscovery(offers.get(0));
            return;
        }
        final String[] labels = new String[offers.size()];
        for (int i = 0; i < offers.size(); i++) {
            DiscoveryCommit c = offers.get(i);
            labels[i] = c.ts == null ? c.host : (c.host + "  ·  " + c.ts);
        }
        new AlertDialog.Builder(this)
            .setTitle(getString(R.string.dialog_found_computer))
            .setSingleChoiceItems(labels, 0, null)
            .setPositiveButton(getString(R.string.dialog_confirm), (d, w) -> {
                int which = ((AlertDialog) d).getListView().getCheckedItemPosition();
                if (which >= 0 && which < offers.size()) confirmDiscovery(offers.get(which));
            })
            .setNegativeButton(getString(R.string.dialog_cancel), null)
            .show();
    }

    private void confirmDiscovery(DiscoveryCommit offer) {
        if (offer == null || offer.persist()) return;
        final String cur = hostInput.getText().toString().trim();
        final String port = portInput.getText().toString().trim();
        StringBuilder msg = new StringBuilder();
        msg.append("host: ").append(cur.isEmpty() ? getString(R.string.empty_label) : cur).append(" → ").append(offer.host);
        msg.append("\nport: ").append(port);
        if (offer.ts != null) {
            String curTs = prefs.getString(KEY_TS_IP, "");
            msg.append("\nts: ").append(curTs.isEmpty() ? getString(R.string.empty_label) : curTs).append(" → ").append(offer.ts);
        }
        final String shown = msg.toString();
        new AlertDialog.Builder(this)
            .setTitle(getString(R.string.dialog_confirm_target))
            .setMessage(shown)
            .setPositiveButton(getString(R.string.dialog_confirm), (d, w) -> {
                if (!shown.contains("→ " + offer.host)) return;
                DiscoveryCommit kept = offer.confirm();
                if (!kept.persist()) return;
                SharedPreferences.Editor ed = prefs.edit().putString(KEY_LAN_IP, kept.host);
                if (kept.ts != null) ed.putString(KEY_TS_IP, kept.ts);
                ed.apply();
                hostInput.setText(kept.host);
                savePrefs();
                updateChipLabels();
                Toast.makeText(this, getString(R.string.toast_changed_to, kept.host), Toast.LENGTH_SHORT).show();
                if (TelemetryState.running) restartStream();
            })
            .setNegativeButton(getString(R.string.dialog_cancel), null)
            .show();
    }

    private void refreshBatteryButton() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        boolean ignoring = pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
        if (ignoring) {
            batteryBtn.setText(getString(R.string.btn_battery_allowed));
            batteryBtn.setTextColor(0xFF8FD694);
        } else {
            batteryBtn.setText(getString(R.string.btn_battery_allow));
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
        final boolean reachable = TelemetryState.desktopReachable;
        final boolean idle = running && reachable && TelemetryState.powerSavingIdle;
        final int uiState = !running ? 0 : (!reachable ? 3 : (idle ? 2 : 1));

        if (uiState != lastUiState) {
            lastUiState = uiState;
            if (uiState == 3) {
                statusBadge.setBackground(badgeIdleBg);
                statusBadge.setTextColor(COLOR_DANGER);
                statusBadge.setText(getString(R.string.badge_unreachable));
                toggleBtn.setText(getString(R.string.btn_stop_stream));
                toggleBtn.setBackground(btnStopBg);
                toggleBtn.setTextColor(Color.WHITE);
            } else if (running) {
                statusBadge.setBackground(badgeRunningBg);
                statusBadge.setTextColor(0xFF8FD694);
                statusBadge.setText(idle
                    ? getString(R.string.badge_idle_power)
                    : getString(R.string.badge_streaming));
                toggleBtn.setText(getString(R.string.btn_stop_stream));
                toggleBtn.setBackground(btnStopBg);
                toggleBtn.setTextColor(Color.WHITE);
            } else {
                statusBadge.setBackground(badgeIdleBg);
                statusBadge.setTextColor(COLOR_ACCENT);
                statusBadge.setText(getString(R.string.badge_idle));
                toggleBtn.setText(getString(R.string.btn_start_stream));
                toggleBtn.setBackground(btnStartBg);
                toggleBtn.setTextColor(COLOR_BG);
            }
        }

        telemetryText.setText(String.format(
            Locale.US,
            "ax: %+.2f  |  ay: %+.2f  |  gz: %+.2f",
            TelemetryState.ax, TelemetryState.ay, TelemetryState.gz
        ));

        if (running && !reachable) {
            hzText.setText(getString(R.string.hz_unreachable, TelemetryState.packetsSent));
        } else if (running) {
            hzText.setText(getString(R.string.hz_running, TelemetryState.hz, TelemetryState.powerModeLabel,
                TelemetryState.sensorLabel, TelemetryState.packetsSent));
        } else {
            hzText.setText(getString(R.string.hz_idle_hint));
        }

        String err = (linkNote != null && !linkNote.isEmpty()) ? linkNote : TelemetryState.lastError;
        if (err != null && !err.isEmpty()) {
            errorText.setText(err);
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
