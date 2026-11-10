package com.aitest.aicli;

import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.PopupMenu;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.journeyapps.barcodescanner.ScanContract;
import com.journeyapps.barcodescanner.ScanIntentResult;
import com.journeyapps.barcodescanner.ScanOptions;

import java.text.DateFormat;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/** Connection, multi-desktop management, QR pairing and Android settings. */
public final class SettingsActivity extends AppCompatActivity {
    public static final String EXTRA_OPEN_SCANNER = "openScanner";
    private static final String APP_PREFS = "app_settings";
    private static final int MENU_SWITCH = 1;
    private static final int MENU_DETAILS = 2;
    private static final int MENU_REMARK = 3;
    private static final int MENU_DELETE = 4;
    private static final long AVAILABILITY_REFRESH_MS = 15_000L;

    private TextInputEditText deviceNameInput;
    private TextView deviceNameStatus;
    private TextView computerStatus;
    private TextView computerListCount;
    private TextView computerListEmpty;
    private Button saveDeviceNameButton;
    private ComputerConnectionAdapter computerAdapter;
    private ComputerConnectionStore connectionStore;
    private AicliClient client;
    private boolean scannerOpened;
    private final Handler availabilityHandler = new Handler(Looper.getMainLooper());
    private final Map<String, AicliClient> availabilityClients = new HashMap<>();
    private long availabilityGeneration;
    private boolean availabilityMonitoring;
    private final Runnable availabilityRefresh = new Runnable() {
        @Override public void run() {
            if (!availabilityMonitoring) return;
            probeAllComputers();
            availabilityHandler.postDelayed(this, AVAILABILITY_REFRESH_MS);
        }
    };

    private final ActivityResultLauncher<ScanOptions> scanner =
            registerForActivityResult(new ScanContract(), this::onScanResult);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);
        findViewById(R.id.settingsBackButton).setOnClickListener(view -> finish());

        deviceNameInput = findViewById(R.id.deviceNameInput);
        deviceNameStatus = findViewById(R.id.deviceNameStatus);
        computerStatus = findViewById(R.id.computerStatus);
        computerListCount = findViewById(R.id.computerListCount);
        computerListEmpty = findViewById(R.id.computerListEmpty);
        saveDeviceNameButton = findViewById(R.id.saveDeviceNameButton);

        connectionStore = new ComputerConnectionStore(this);
        client = new AicliClient(this, "");
        deviceNameInput.setText(client.deviceName());

        RecyclerView computerList = findViewById(R.id.computerList);
        computerList.setLayoutManager(new LinearLayoutManager(this));
        computerAdapter = new ComputerConnectionAdapter(new ComputerConnectionAdapter.Listener() {
            @Override public void onSelect(ComputerConnectionStore.Computer computer) {
                switchComputer(computer);
            }

            @Override public void onManage(ComputerConnectionStore.Computer computer, View anchor) {
                showComputerMenu(computer, anchor);
            }
        });
        computerList.setAdapter(computerAdapter);
        refreshComputerList();
        updateStatus();

        saveDeviceNameButton.setOnClickListener(view -> saveDeviceName());
        View addComputerButton = findViewById(R.id.addComputerButton);
        addComputerButton.setOnClickListener(view -> startQrBinding());

        SharedPreferences appSettings = getSharedPreferences(APP_PREFS, MODE_PRIVATE);
        MaterialSwitch notifications = findViewById(R.id.notificationSwitch);
        MaterialSwitch vibration = findViewById(R.id.vibrationSwitch);
        notifications.setChecked(appSettings.getBoolean("notifications", true));
        vibration.setChecked(appSettings.getBoolean("vibration", true));
        notifications.setOnCheckedChangeListener((button, checked) ->
                appSettings.edit().putBoolean("notifications", checked).apply());
        vibration.setOnCheckedChangeListener((button, checked) ->
                appSettings.edit().putBoolean("vibration", checked).apply());

        AppUpdater updater = UpdateApplication.updater(this);
        MaterialSwitch automaticUpdates = findViewById(R.id.automaticUpdateSwitch);
        automaticUpdates.setChecked(updater.automatic());
        automaticUpdates.setOnCheckedChangeListener((button, checked) -> updater.setAutomatic(checked));
        findViewById(R.id.checkUpdateButton).setOnClickListener(view -> updater.check(true));
        ((TextView) findViewById(R.id.versionText)).setText(
                "版本 " + BuildConfig.VERSION_NAME + " · 私钥由 Android Keystore 管理");

        if (getIntent().getBooleanExtra(EXTRA_OPEN_SCANNER, false)) {
            addComputerButton.postDelayed(this::startQrBinding, 180L);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        availabilityMonitoring = true;
        scheduleAvailabilityRefresh(true);
    }

    @Override
    protected void onPause() {
        stopAvailabilityMonitoring();
        super.onPause();
    }

    private void saveDeviceName() {
        String name = deviceNameInput.getText().toString().trim();
        if (!client.setDeviceName(name)) {
            Toast.makeText(this, "手机名称不能为空且不能超过 64 个字符", Toast.LENGTH_LONG).show();
            deviceNameInput.requestFocus();
            return;
        }
        List<ComputerConnectionStore.Computer> computers = connectionStore.all();
        deviceNameStatus.setVisibility(View.VISIBLE);
        if (computers.isEmpty()) {
            deviceNameStatus.setText("名称已保存在本机，绑定电脑时会自动提交。 ");
            return;
        }

        saveDeviceNameButton.setEnabled(false);
        deviceNameStatus.setText("正在同步到 " + computers.size() + " 台电脑…");
        AtomicInteger finished = new AtomicInteger();
        AtomicInteger succeeded = new AtomicInteger();
        for (ComputerConnectionStore.Computer computer : computers) {
            AicliClient syncClient = new AicliClient(this, computer);
            syncClient.updateDeviceName(name, new AicliClient.ActionCallback() {
                @Override public void onSuccess() {
                    succeeded.incrementAndGet();
                    syncClient.close();
                    finishDeviceNameSync(computers.size(), finished.incrementAndGet(),
                            succeeded.get());
                }

                @Override public void onError(String message) {
                    syncClient.close();
                    finishDeviceNameSync(computers.size(), finished.incrementAndGet(),
                            succeeded.get());
                }
            });
        }
    }

    private void finishDeviceNameSync(int total, int finished, int succeeded) {
        if (finished < total) return;
        runOnUiThread(() -> {
            saveDeviceNameButton.setEnabled(true);
            if (succeeded == total) {
                deviceNameStatus.setText("已同步到全部 " + total + " 台电脑");
            } else {
                deviceNameStatus.setText("名称已保存，已同步 " + succeeded + "/" + total
                        + " 台；离线电脑将在下次连接时自动同步。");
            }
        });
    }

    private void startQrBinding() {
        if (scannerOpened) return;
        scannerOpened = true;
        ScanOptions options = new ScanOptions();
        options.setDesiredBarcodeFormats(ScanOptions.QR_CODE);
        options.setPrompt("对准电脑端 AI CLI 的二维码");
        options.setBeepEnabled(false);
        options.setOrientationLocked(true);
        options.setCaptureActivity(AicliCaptureActivity.class);
        scanner.launch(options);
    }

    private void onScanResult(ScanIntentResult result) {
        scannerOpened = false;
        if (result == null || result.getContents() == null || result.getContents().isEmpty()) {
            Toast.makeText(this, "已取消扫码", Toast.LENGTH_SHORT).show();
            return;
        }
        final String payload = result.getContents();
        try {
            JsonObject qr = JsonParser.parseString(payload).getAsJsonObject();
            if (!qr.has("pairingId") || !qr.has("challenge") || !qr.has("computerId") ||
                    !qr.has("certificateSha256")) {
                throw new IllegalArgumentException("不是 AI CLI 配对二维码");
            }
        } catch (Exception error) {
            Toast.makeText(this, "二维码无效：" + error.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }

        computerStatus.setText("正在提交手机公钥，等待电脑确认…");
        client.pairFromQr(payload, new AicliClient.PairingCallback() {
            @Override public void onStatus(String status, String message) {
                runOnUiThread(() -> computerStatus.setText(status + "\n" + message));
            }

            @Override public void onComplete() {
                runOnUiThread(() -> {
                    replaceClientFromActive();
                    Toast.makeText(SettingsActivity.this,
                            "新服务端已加入列表并切换为当前服务端", Toast.LENGTH_LONG).show();
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    replaceClientFromActive();
                    Toast.makeText(SettingsActivity.this, message, Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    private void switchComputer(ComputerConnectionStore.Computer computer) {
        if (computer == null) return;
        ComputerConnectionStore.Computer active = connectionStore.active();
        if (active != null && active.computerId.equals(computer.computerId)) return;
        if (!connectionStore.activate(computer.computerId)) return;
        replaceClientFromActive();
        Toast.makeText(this, "已切换到 " + computer.displayName(), Toast.LENGTH_SHORT).show();
    }

    private void showComputerMenu(ComputerConnectionStore.Computer computer, View anchor) {
        PopupMenu menu = new PopupMenu(this, anchor);
        ComputerConnectionStore.Computer active = connectionStore.active();
        if (active == null || !active.computerId.equals(computer.computerId)) {
            menu.getMenu().add(0, MENU_SWITCH, 0, "切换到此服务端");
        }
        menu.getMenu().add(0, MENU_DETAILS, 1, "查看与编辑服务端");
        menu.getMenu().add(0, MENU_REMARK, 2, "修改备注");
        menu.getMenu().add(0, MENU_DELETE, 3, "删除服务端");
        menu.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == MENU_SWITCH) {
                switchComputer(computer);
                return true;
            }
            if (item.getItemId() == MENU_DETAILS) {
                showComputerDetails(computer);
                return true;
            }
            if (item.getItemId() == MENU_REMARK) {
                showRemarkDialog(computer);
                return true;
            }
            if (item.getItemId() == MENU_DELETE) {
                confirmDelete(computer);
                return true;
            }
            return false;
        });
        menu.show();
    }

    private void showComputerDetails(ComputerConnectionStore.Computer computer) {
        View content = getLayoutInflater().inflate(R.layout.dialog_computer_details, null);
        TextView status = content.findViewById(R.id.computerDetailStatus);
        TextView metadata = content.findViewById(R.id.computerDetailMetadata);
        TextInputLayout localLayout = content.findViewById(R.id.computerLocalAddressLayout);
        TextInputLayout remoteLayout = content.findViewById(R.id.computerRemoteAddressLayout);
        TextInputEditText localInput = content.findViewById(R.id.computerLocalAddressInput);
        TextInputEditText remoteInput = content.findViewById(R.id.computerRemoteAddressInput);
        MaterialButtonToggleGroup routeGroup = content.findViewById(
                R.id.computerRoutePreferenceGroup);
        TextView routeHint = content.findViewById(R.id.computerRoutePreferenceHint);

        ComputerConnectionStore.Computer active = connectionStore.active();
        boolean isActive = active != null && active.computerId.equals(computer.computerId);
        status.setText(isActive ? "当前服务端 · 自动连接" : "已绑定 · 自动连接");
        String desktopName = computer.name.isEmpty() ? "未提供" : computer.name;
        String pairedTime = computer.pairedAt <= 0L ? "未知" : DateFormat
                .getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
                .format(new Date(computer.pairedAt));
        metadata.setText("电脑名称：" + desktopName
                + "\n服务端 ID：" + computer.computerId
                + "\n绑定时间：" + pairedTime
                + "\n上次可用地址：" + (computer.baseUrl.isEmpty() ? "尚未记录" : computer.baseUrl)
                + "\n证书指纹：" + (computer.certificateFingerprint.isEmpty()
                ? "未固定" : computer.certificateFingerprint));
        localInput.setText(computer.localUrl);
        remoteInput.setText(computer.remoteUrl);
        routeGroup.check(computer.remotePreferred()
                ? R.id.computerPreferRemoteButton : R.id.computerPreferLocalButton);
        updateRoutePreferenceHint(routeHint, computer.routePreference);
        routeGroup.addOnButtonCheckedListener((group, checkedId, isChecked) -> {
            if (!isChecked) return;
            updateRoutePreferenceHint(routeHint,
                    checkedId == R.id.computerPreferRemoteButton
                            ? ComputerConnectionStore.ROUTE_REMOTE
                            : ComputerConnectionStore.ROUTE_LOCAL);
        });

        AlertDialog dialog = new MaterialAlertDialogBuilder(this)
                .setTitle(computer.displayName())
                .setView(content)
                .setNegativeButton("取消", null)
                .setPositiveButton("保存", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                .setOnClickListener(view -> {
                    localLayout.setError(null);
                    remoteLayout.setError(null);
                    String local;
                    String remote;
                    try {
                        local = normalizeSecureAddress(textOf(localInput));
                    } catch (IllegalArgumentException error) {
                        localLayout.setError(error.getMessage());
                        localInput.requestFocus();
                        return;
                    }
                    try {
                        remote = normalizeSecureAddress(textOf(remoteInput));
                    } catch (IllegalArgumentException error) {
                        remoteLayout.setError(error.getMessage());
                        remoteInput.requestFocus();
                        return;
                    }
                    if (local.isEmpty() && remote.isEmpty()) {
                        localLayout.setError("局域网地址和公网地址至少填写一个");
                        localInput.requestFocus();
                        return;
                    }
                    String routePreference = routeGroup.getCheckedButtonId()
                            == R.id.computerPreferRemoteButton
                            ? ComputerConnectionStore.ROUTE_REMOTE
                            : ComputerConnectionStore.ROUTE_LOCAL;
                    if (!connectionStore.updateConnectionSettings(
                            computer.computerId, local, remote, routePreference)) {
                        Toast.makeText(this, "服务端地址保存失败", Toast.LENGTH_LONG).show();
                        return;
                    }
                    if (isActive) replaceClientFromActive();
                    else {
                        refreshComputerList();
                        updateStatus();
                        scheduleAvailabilityRefresh(true);
                    }
                    dialog.dismiss();
                    Toast.makeText(this, "服务端连接设置已保存", Toast.LENGTH_SHORT).show();
                }));
        dialog.show();
    }

    private void showRemarkDialog(ComputerConnectionStore.Computer computer) {
        View content = getLayoutInflater().inflate(R.layout.dialog_edit_computer_remark, null);
        TextInputEditText input = content.findViewById(R.id.computerRemarkInput);
        input.setText(computer.remark);
        input.setSelection(input.length());
        String desktopName = computer.name.isEmpty() ? computer.displayName() : computer.name;
        new MaterialAlertDialogBuilder(this)
                .setTitle("修改服务端备注")
                .setMessage("电脑名称：" + desktopName + "\n留空可恢复显示电脑名称。")
                .setView(content)
                .setNegativeButton("取消", null)
                .setPositiveButton("保存", (dialog, which) -> {
                    String remark = input.getText() == null
                            ? "" : input.getText().toString().trim();
                    if (connectionStore.updateRemark(computer.computerId, remark)) {
                        refreshComputerList();
                        updateStatus();
                        Toast.makeText(this, remark.isEmpty() ? "已清除备注" : "备注已保存",
                                Toast.LENGTH_SHORT).show();
                    }
                })
                .show();
    }

    private void confirmDelete(ComputerConnectionStore.Computer computer) {
        ComputerConnectionStore.Computer active = connectionStore.active();
        boolean deletingActive = active != null && active.computerId.equals(computer.computerId);
        String consequence = deletingActive
                ? connectionStore.count() > 1
                    ? "删除后会自动切换到另一台电脑。"
                    : "删除后手机将回到未绑定状态。"
                : "当前正在使用的电脑不会受影响。";
        new MaterialAlertDialogBuilder(this)
                .setTitle("删除服务端")
                .setMessage("确定从本手机删除“" + computer.displayName() + "”吗？\n\n"
                        + consequence + "\n这不会自动删除电脑端的绑定记录。")
                .setNegativeButton("取消", null)
                .setPositiveButton("删除", (dialog, which) -> deleteComputer(computer))
                .show();
    }

    private void deleteComputer(ComputerConnectionStore.Computer computer) {
        ComputerConnectionStore.Computer active = connectionStore.active();
        boolean deletingActive = active != null && active.computerId.equals(computer.computerId);
        ComputerConnectionStore.Computer removed = connectionStore.remove(computer.computerId);
        if (removed == null) {
            Toast.makeText(this, "服务端已经不存在", Toast.LENGTH_SHORT).show();
            return;
        }
        if (deletingActive) replaceClientFromActive();
        else {
            refreshComputerList();
            updateStatus();
            scheduleAvailabilityRefresh(true);
        }
        Toast.makeText(this, "已删除 " + removed.displayName(), Toast.LENGTH_SHORT).show();
    }

    private void refreshComputerList() {
        List<ComputerConnectionStore.Computer> computers = connectionStore.all();
        ComputerConnectionStore.Computer active = connectionStore.active();
        computerAdapter.replace(computers, active == null ? "" : active.computerId);
        computerListCount.setText(computers.size() + " 台");
        computerListEmpty.setVisibility(computers.isEmpty() ? View.VISIBLE : View.GONE);
        findViewById(R.id.computerList).setVisibility(
                computers.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void scheduleAvailabilityRefresh(boolean immediate) {
        availabilityHandler.removeCallbacks(availabilityRefresh);
        if (!availabilityMonitoring) return;
        availabilityHandler.postDelayed(availabilityRefresh,
                immediate ? 0L : AVAILABILITY_REFRESH_MS);
    }

    private void probeAllComputers() {
        closeAvailabilityClients();
        long generation = ++availabilityGeneration;
        for (ComputerConnectionStore.Computer computer : connectionStore.all()) {
            if (!hasConfiguredAddress(computer)) {
                computerAdapter.setHealth(computer.computerId,
                        ComputerConnectionAdapter.Health.NOT_CONNECTED);
                continue;
            }
            computerAdapter.setHealth(computer.computerId,
                    ComputerConnectionAdapter.Health.CONNECTING);
            AicliClient probeClient = new AicliClient(this, computer, false);
            availabilityClients.put(computer.computerId, probeClient);
            probeClient.probeAvailability(online -> runOnUiThread(() -> {
                AicliClient current = availabilityClients.get(computer.computerId);
                if (!availabilityMonitoring || generation != availabilityGeneration ||
                        current != probeClient) {
                    probeClient.close();
                    return;
                }
                availabilityClients.remove(computer.computerId);
                probeClient.close();
                computerAdapter.setHealth(computer.computerId, online
                        ? ComputerConnectionAdapter.Health.CONNECTED
                        : ComputerConnectionAdapter.Health.FAILED);
            }));
        }
    }

    private static boolean hasConfiguredAddress(ComputerConnectionStore.Computer computer) {
        return computer != null && (!computer.baseUrl.isEmpty() ||
                !computer.localUrl.isEmpty() || !computer.remoteUrl.isEmpty());
    }

    private void stopAvailabilityMonitoring() {
        availabilityMonitoring = false;
        availabilityHandler.removeCallbacks(availabilityRefresh);
        availabilityGeneration++;
        closeAvailabilityClients();
    }

    private void closeAvailabilityClients() {
        for (AicliClient probeClient : availabilityClients.values()) {
            probeClient.close();
        }
        availabilityClients.clear();
    }

    private void updateStatus() {
        ComputerConnectionStore.Computer active = connectionStore.active();
        if (active == null) {
            computerStatus.setText("暂无已绑定服务端\n手机 ID：" + client.phoneId()
                    + "\n手机私钥仍安全保存在 Android Keystore 中");
            return;
        }
        String actualName = !active.remark.isEmpty() && !active.name.isEmpty()
                ? "\n电脑名称：" + active.name : "";
        String preferredAddress = ComputerConnectionStore.preferredAddress(
                active.localUrl, active.remoteUrl, active.routePreference);
        String preferenceLabel = active.remotePreferred() ? "广域网优先" : "局域网优先";
        computerStatus.setText("当前服务端：" + active.displayName()
                + actualName
                + "\n连接策略：" + preferenceLabel + "，失败时自动切换"
                + "\n首选地址：" + preferredAddress
                + "\n上次可用：" + (active.baseUrl.isEmpty() ? "尚未记录" : active.baseUrl)
                + (active.localUrl.isEmpty() ? "" : "\n局域网地址：" + active.localUrl)
                + (active.remoteUrl.isEmpty() ? "" : "\n公网/Relay：" + active.remoteUrl)
                + "\n证书指纹：" + (client.certificateFingerprint().isEmpty()
                ? "未固定（请优先使用扫码绑定）" : client.certificateFingerprint())
                + "\n手机 ID：" + client.phoneId());
    }

    private static void updateRoutePreferenceHint(TextView view, String preference) {
        if (ComputerConnectionStore.ROUTE_REMOTE.equals(
                ComputerConnectionStore.normalizeRoutePreference(preference))) {
            view.setText("优先使用公网；公网不可用时切到局域网，并在后台探测公网，恢复后自动切回。");
        } else {
            view.setText("优先使用局域网；局域网不可用时切到公网，并在后台探测局域网，恢复后自动切回。");
        }
    }

    private static String textOf(TextInputEditText input) {
        return input.getText() == null ? "" : input.getText().toString().trim();
    }

    private static String normalizeSecureAddress(String value) {
        String result = value == null ? "" : value.trim();
        if (result.isEmpty()) return "";
        if (!result.contains("://")) result = "https://" + result;
        while (result.endsWith("/")) result = result.substring(0, result.length() - 1);
        Uri parsed = Uri.parse(result);
        if (!"https".equalsIgnoreCase(parsed.getScheme()) || parsed.getHost() == null ||
                parsed.getHost().trim().isEmpty()) {
            throw new IllegalArgumentException("请输入有效的 HTTPS 地址");
        }
        return result;
    }

    private void replaceClientFromActive() {
        if (client != null) client.close();
        client = new AicliClient(this, "");
        deviceNameInput.setText(client.deviceName());
        refreshComputerList();
        updateStatus();
        scheduleAvailabilityRefresh(true);
    }

    @Override
    protected void onDestroy() {
        stopAvailabilityMonitoring();
        if (client != null) client.close();
        super.onDestroy();
    }
}
