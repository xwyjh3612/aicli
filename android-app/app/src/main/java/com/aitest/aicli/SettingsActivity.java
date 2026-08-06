package com.aitest.aicli;

import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

public final class SettingsActivity extends AppCompatActivity {
    private static final String CONNECTION_PREFS = "connection";
    private static final String BASE_URL_KEY = "baseUrl";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);
        setTitle("设置");

        EditText baseUrlInput = findViewById(R.id.baseUrlInput);
        baseUrlInput.setText(getSharedPreferences(CONNECTION_PREFS, MODE_PRIVATE)
                .getString(BASE_URL_KEY, ""));
        findViewById(R.id.saveConnectionButton).setOnClickListener(view -> {
            String value = baseUrlInput.getText().toString().trim();
            if (!value.startsWith("http://") && !value.startsWith("https://")) {
                Toast.makeText(this, "地址必须以 http:// 或 https:// 开头", Toast.LENGTH_LONG).show();
                return;
            }
            getSharedPreferences(CONNECTION_PREFS, MODE_PRIVATE)
                    .edit().putString(BASE_URL_KEY, value).apply();
            Toast.makeText(this, "电脑地址已保存", Toast.LENGTH_SHORT).show();
        });

        Button addComputer = findViewById(R.id.addComputerButton);
        addComputer.setOnClickListener(view -> startQrBinding());
    }

    private void startQrBinding() {
        Toast.makeText(this, "扫码模块将在下一步接入 Android 相机和二维码解析", Toast.LENGTH_LONG).show();
        // TODO: launch an in-app camera scanner, never the external system camera/web browser.
    }
}
