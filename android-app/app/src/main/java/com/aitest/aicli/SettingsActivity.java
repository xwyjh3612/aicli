package com.aitest.aicli;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.Switch;

import androidx.activity.result.ActivityResultLauncher;
import androidx.appcompat.app.AppCompatActivity;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.journeyapps.barcodescanner.ScanContract;
import com.journeyapps.barcodescanner.ScanIntentResult;
import com.journeyapps.barcodescanner.ScanOptions;

/** Connection, QR pairing and local Android settings. */
public final class SettingsActivity extends AppCompatActivity {
    public static final String EXTRA_OPEN_SCANNER = "openScanner";
    private static final String CONNECTION_PREFS = "connection";
    private static final String BASE_URL_KEY = "baseUrl";
    private static final String APP_PREFS = "app_settings";

    private EditText baseUrlInput;
    private TextView computerStatus;
    private AicliClient client;
    private boolean scannerOpened;

    private final ActivityResultLauncher<ScanOptions> scanner =
            registerForActivityResult(new ScanContract(), this::onScanResult);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);
        setTitle("设置");

        baseUrlInput = findViewById(R.id.baseUrlInput);
        computerStatus = findViewById(R.id.computerStatus);
        SharedPreferences preferences = getSharedPreferences(CONNECTION_PREFS, MODE_PRIVATE);
        baseUrlInput.setText(preferences.getString(BASE_URL_KEY, ""));

        client = new AicliClient(this, baseUrlInput.getText().toString());
        updateStatus();

        findViewById(R.id.saveConnectionButton).setOnClickListener(view -> saveManualAddress());
        Button addComputer = findViewById(R.id.addComputerButton);
        addComputer.setOnClickListener(view -> startQrBinding());
        findViewById(R.id.clearComputerButton).setOnClickListener(view -> clearComputer());

        SharedPreferences appSettings = getSharedPreferences(APP_PREFS, MODE_PRIVATE);
        Switch notifications = findViewById(R.id.notificationSwitch);
        Switch vibration = findViewById(R.id.vibrationSwitch);
        notifications.setChecked(appSettings.getBoolean("notifications", true));
        vibration.setChecked(appSettings.getBoolean("vibration", true));
        notifications.setOnCheckedChangeListener((button, checked) ->
                appSettings.edit().putBoolean("notifications", checked).apply());
        vibration.setOnCheckedChangeListener((button, checked) ->
                appSettings.edit().putBoolean("vibration", checked).apply());

        if (getIntent().getBooleanExtra(EXTRA_OPEN_SCANNER, false)) {
            // Wait until the settings view is attached so the camera permission
            // prompt appears above this activity rather than the main screen.
            baseUrlInput.postDelayed(this::startQrBinding, 180L);
        }
    }

    private void saveManualAddress() {
        String value = baseUrlInput.getText().toString().trim();
        if (!value.startsWith("https://")) {
            Toast.makeText(this, "公网和自签名连接必须使用 https://", Toast.LENGTH_LONG).show();
            return;
        }
        getSharedPreferences(CONNECTION_PREFS, MODE_PRIVATE).edit()
                .putString(BASE_URL_KEY, value).apply();
        client.setBaseUrl(value);
        updateStatus();
        Toast.makeText(this, "电脑地址已保存，正在连接", Toast.LENGTH_SHORT).show();
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
                    baseUrlInput.setText(client.baseUrl());
                    updateStatus();
                    Toast.makeText(SettingsActivity.this, "手机已绑定，可以返回主界面使用", Toast.LENGTH_LONG).show();
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    computerStatus.setText("绑定失败\n" + message);
                    Toast.makeText(SettingsActivity.this, message, Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    private void clearComputer() {
        client.clearConnection();
        baseUrlInput.setText("");
        computerStatus.setText("暂无已绑定电脑\n手机身份仍保留在 Android Keystore 中");
        Toast.makeText(this, "已清除当前电脑连接信息", Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onDestroy() {
        if (client != null) client.close();
        super.onDestroy();
    }

    private void updateStatus() {
        String address = client.baseUrl();
        if (address.isEmpty()) {
            computerStatus.setText("暂无已绑定电脑\n手机 ID：" + client.phoneId());
            return;
        }
        computerStatus.setText("当前电脑：" + (client.computerId().isEmpty() ? "已保存地址" : client.computerId())
                + "\n当前地址：" + address
                + (client.localUrl().isEmpty() ? "" : "\n内网地址：" + client.localUrl())
                + (client.remoteUrl().isEmpty() ? "" : "\n公网/Relay：" + client.remoteUrl())
                + "\n证书指纹：" + (client.certificateFingerprint().isEmpty()
                ? "未固定（请优先使用扫码绑定）" : client.certificateFingerprint())
                + "\n手机 ID：" + client.phoneId());
    }
}
