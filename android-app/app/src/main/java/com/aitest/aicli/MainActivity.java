package com.aitest.aicli;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.View;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.tabs.TabLayout;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.util.List;

public final class MainActivity extends AppCompatActivity {
    private static final String[] PROVIDERS = {"Codex", "Claude"};
    private static final String CONNECTION_PREFS = "connection";
    private static final String BASE_URL_KEY = "baseUrl";

    private final StringBuilder terminalText = new StringBuilder();
    private AicliClient client;
    private ConversationAdapter conversationAdapter;
    private String provider = "codex";
    private String selectedConversationId;
    private TextView emptyState;
    private TextView connectionStatus;
    private TextView terminalOutput;
    private View inputBar;
    private EditText messageInput;
    private RecyclerView conversationList;
    private boolean listExpanded;

    private void initializeDeviceIdentity() {
        SharedPreferences preferences = getSharedPreferences("device_identity", MODE_PRIVATE);
        DeviceKeyManager keyManager = new DeviceKeyManager();
        try {
            String phoneId = keyManager.getOrCreatePhoneId(preferences);
            keyManager.getOrCreateKeyPair();
            preferences.edit().putString("keyAlias", keyManager.keyAlias()).apply();
            android.util.Log.i("AicliDevice", "device identity ready: " + phoneId);
        } catch (Exception error) {
            Toast.makeText(this, "设备密钥初始化失败", Toast.LENGTH_LONG).show();
            android.util.Log.e("AicliDevice", "device identity initialization failed", error);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        initializeDeviceIdentity();

        emptyState = findViewById(R.id.emptyState);
        connectionStatus = findViewById(R.id.connectionStatus);
        terminalOutput = findViewById(R.id.terminalOutput);
        inputBar = findViewById(R.id.inputBar);
        messageInput = findViewById(R.id.messageInput);
        conversationList = findViewById(R.id.conversationList);
        conversationList.setLayoutManager(new LinearLayoutManager(this));
        conversationAdapter = new ConversationAdapter(this::selectConversation);
        conversationList.setAdapter(conversationAdapter);

        TabLayout providerTabs = findViewById(R.id.providerTabs);
        providerTabs.removeAllTabs();
        for (String providerName : PROVIDERS) {
            providerTabs.addTab(providerTabs.newTab().setText(providerName));
        }
        providerTabs.addOnTabSelectedListener(new TabLayout.OnTabSelectedListener() {
            @Override
            public void onTabSelected(TabLayout.Tab tab) {
                provider = tab.getPosition() == 1 ? "claude" : "codex";
                selectedConversationId = null;
                loadConversations();
            }

            @Override public void onTabUnselected(TabLayout.Tab tab) { }
            @Override public void onTabReselected(TabLayout.Tab tab) { loadConversations(); }
        });

        findViewById(R.id.settingsButton).setOnClickListener(view ->
                startActivity(new Intent(this, SettingsActivity.class)));
        findViewById(R.id.scanButton).setOnClickListener(view ->
                startActivity(new Intent(this, SettingsActivity.class)));
        findViewById(R.id.conversationsButton).setOnClickListener(view -> toggleConversationList());
        findViewById(R.id.sendButton).setOnClickListener(view -> sendInput());

        configureClient();
    }

    @Override
    protected void onResume() {
        super.onResume();
        configureClient();
    }

    @Override
    protected void onDestroy() {
        if (client != null) {
            client.closeEvents();
        }
        super.onDestroy();
    }

    private void configureClient() {
        String baseUrl = getSharedPreferences(CONNECTION_PREFS, MODE_PRIVATE)
                .getString(BASE_URL_KEY, "");
        if (client == null) {
            client = new AicliClient(baseUrl);
        } else {
            client.setBaseUrl(baseUrl);
        }
        if (baseUrl.isEmpty()) {
            connectionStatus.setText("未设置电脑地址，请在设置中填写内网或 Relay 地址");
            emptyState.setVisibility(View.VISIBLE);
            conversationList.setVisibility(View.GONE);
            terminalOutput.setVisibility(View.GONE);
            inputBar.setVisibility(View.GONE);
            return;
        }
        client.connectEvents(new AicliClient.EventCallback() {
            @Override
            public void onEvent(String eventJson) {
                runOnUiThread(() -> handleEvent(eventJson));
            }

            @Override
            public void onClosed() {
                runOnUiThread(() -> connectionStatus.setText("事件连接已断开，正在等待重连"));
            }
        });
        loadConversations();
    }

    private void loadConversations() {
        if (client == null || client.baseUrl().isEmpty()) {
            return;
        }
        connectionStatus.setText("正在连接电脑...");
        client.listConversations(provider, new AicliClient.ConversationCallback() {
            @Override
            public void onSuccess(List<AicliClient.Conversation> conversations) {
                runOnUiThread(() -> {
                    conversationAdapter.replace(conversations);
                    connectionStatus.setText("已连接 · " + conversations.size() + " 个会话");
                    boolean hasConversations = !conversations.isEmpty();
                    emptyState.setVisibility(hasConversations ? View.GONE : View.VISIBLE);
                    if (!hasConversations) {
                        emptyState.setText("电脑上暂无此提供方会话");
                        terminalOutput.setVisibility(View.GONE);
                        inputBar.setVisibility(View.GONE);
                    } else if (selectedConversationId == null) {
                        selectConversation(conversations.get(0));
                    }
                });
            }

            @Override
            public void onError(String message) {
                runOnUiThread(() -> {
                    connectionStatus.setText("连接失败：" + message);
                    Toast.makeText(MainActivity.this, message, Toast.LENGTH_SHORT).show();
                });
            }
        });
    }

    private void selectConversation(AicliClient.Conversation conversation) {
        selectedConversationId = conversation.id;
        terminalText.setLength(0);
        terminalOutput.setVisibility(View.VISIBLE);
        inputBar.setVisibility(View.VISIBLE);
        appendOutput("[" + conversation.name + "] 状态：" + conversation.status);
        client.listEvents(conversation.id, 0, new AicliClient.HistoryCallback() {
            @Override
            public void onSuccess(List<String> events) {
                runOnUiThread(() -> {
                    if (!conversation.id.equals(selectedConversationId)) {
                        return;
                    }
                    for (String event : events) {
                        appendOutput(formatEvent(event));
                    }
                });
            }

            @Override
            public void onError(String message) {
                runOnUiThread(() -> appendOutput("[历史记录] " + message));
            }
        });
    }

    private void sendInput() {
        String input = messageInput.getText().toString().trim();
        if (selectedConversationId == null || input.isEmpty()) {
            return;
        }
        messageInput.setText("");
        appendOutput("> " + input);
        client.sendMessage(selectedConversationId, input, new AicliClient.ActionCallback() {
            @Override public void onSuccess() { }
            @Override public void onError(String message) {
                runOnUiThread(() -> appendOutput("[发送失败] " + message));
            }
        });
    }

    private void handleEvent(String eventJson) {
        try {
            JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
            String eventConversationId = event.has("conversationId")
                    ? event.get("conversationId").getAsString() : "";
            if (selectedConversationId != null && !selectedConversationId.equals(eventConversationId)) {
                return;
            }
            appendOutput(formatEvent(eventJson));
            if (event.has("type") && event.get("type").getAsString().startsWith("conversation.")) {
                loadConversations();
            }
        } catch (Exception ignored) {
            appendOutput(eventJson);
        }
    }

    private void appendOutput(String line) {
        if (terminalText.length() > 64 * 1024) {
            terminalText.delete(0, terminalText.length() - 48 * 1024);
        }
        if (terminalText.length() > 0) {
            terminalText.append('\n');
        }
        terminalText.append(line);
        terminalOutput.setText(terminalText.toString());
    }

    private String formatEvent(String eventJson) {
        try {
            JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
            String type = event.has("event_type")
                    ? event.get("event_type").getAsString()
                    : event.has("type") ? event.get("type").getAsString() : "";
            JsonObject payload = event.has("payload") && event.get("payload").isJsonObject()
                    ? event.getAsJsonObject("payload") : null;
            if (payload != null) {
                if (payload.has("text")) {
                    return payload.get("text").getAsString();
                }
                if (payload.has("message")) {
                    return "> " + payload.get("message").getAsString();
                }
                if (payload.has("input")) {
                    return "> " + payload.get("input").getAsString();
                }
            }
            return type.isEmpty() ? eventJson : "[" + type + "]";
        } catch (Exception ignored) {
            return eventJson;
        }
    }

    private void toggleConversationList() {
        listExpanded = !listExpanded;
        conversationList.setVisibility(listExpanded ? View.VISIBLE : View.GONE);
    }
}
