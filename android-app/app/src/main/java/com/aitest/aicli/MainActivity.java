package com.aitest.aicli;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.tabs.TabLayout;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.util.List;

/**
 * Android is deliberately a thin remote console.  The CLI process and the
 * PTY always stay on the desktop; this activity only displays events and
 * forwards input/control requests through AicliClient.
 */
public final class MainActivity extends AppCompatActivity {
    private static final String[] PROVIDERS = {"Codex", "Claude"};
    private static final String CONNECTION_PREFS = "connection";
    private static final String BASE_URL_KEY = "baseUrl";

    private final StringBuilder terminalText = new StringBuilder();
    private AicliClient client;
    private ConversationAdapter conversationAdapter;
    private String provider = "codex";
    private String selectedConversationId;
    private long lastEventId;
    /**
     * Includes every persisted transport value, not only the base URL.  The
     * settings screen can replace the certificate pin and computer identity
     * while this activity is still alive; retaining the old AicliClient in
     * that case would make the next request use the previous TLS pin.
     */
    private String configuredConnectionKey = "";

    private TextView emptyState;
    private TextView connectionStatus;
    private TextView terminalOutput;
    private ScrollView terminalScroll;
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
            Toast.makeText(this, "设备密钥初始化失败：" + error.getMessage(), Toast.LENGTH_LONG).show();
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
        terminalScroll = findViewById(R.id.terminalScroll);
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
                lastEventId = 0L;
                loadConversations();
            }

            @Override public void onTabUnselected(TabLayout.Tab tab) { }
            @Override public void onTabReselected(TabLayout.Tab tab) { loadConversations(); }
        });

        findViewById(R.id.settingsButton).setOnClickListener(view ->
                startActivity(new Intent(this, SettingsActivity.class)));
        findViewById(R.id.scanButton).setOnClickListener(view -> openScannerInSettings());
        findViewById(R.id.conversationsButton).setOnClickListener(view -> toggleConversationList());
        findViewById(R.id.sendButton).setOnClickListener(view -> sendInput());
        findViewById(R.id.pauseButton).setOnClickListener(view -> sendControl("pause"));
        findViewById(R.id.continueButton).setOnClickListener(view -> sendControl("continue"));
        findViewById(R.id.cancelButton).setOnClickListener(view -> sendControl("cancel"));
        findViewById(R.id.restartButton).setOnClickListener(view -> sendControl("restart"));

        messageInput.setOnEditorActionListener((view, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_SEND ||
                    (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER &&
                            event.getAction() == KeyEvent.ACTION_DOWN)) {
                sendInput();
                return true;
            }
            return false;
        });

        configureClient();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // SettingsActivity may have completed a pairing while this activity
        // was paused.  Re-read the pinned connection and reconnect once.
        configureClient();
    }

    @Override
    protected void onDestroy() {
        if (client != null) client.close();
        super.onDestroy();
    }

    private void openScannerInSettings() {
        Intent intent = new Intent(this, SettingsActivity.class);
        intent.putExtra(SettingsActivity.EXTRA_OPEN_SCANNER, true);
        startActivity(intent);
    }

    private void configureClient() {
        SharedPreferences preferences = getSharedPreferences(CONNECTION_PREFS, MODE_PRIVATE);
        String baseUrl = preferences.getString(BASE_URL_KEY, "");
        String connectionKey = preferences.getString(BASE_URL_KEY, "") + "\n"
                + preferences.getString("localAddress", "") + "\n"
                + preferences.getString("remoteAddress", "") + "\n"
                + preferences.getString("certificateSha256", "") + "\n"
                + preferences.getString("computerId", "");
        if (client == null || !connectionKey.equals(configuredConnectionKey)) {
            if (client != null) client.close();
            client = new AicliClient(this, baseUrl);
            configuredConnectionKey = connectionKey;
        }
        if (baseUrl.isEmpty()) {
            connectionStatus.setText("未绑定电脑");
            emptyState.setText("扫描电脑端二维码后开始使用");
            emptyState.setVisibility(View.VISIBLE);
            conversationList.setVisibility(View.GONE);
            terminalScroll.setVisibility(View.GONE);
            inputBar.setVisibility(View.GONE);
            setControlsVisible(false);
            return;
        }
        loadConversations();
    }

    private void loadConversations() {
        if (client == null || client.baseUrl().isEmpty()) return;
        connectionStatus.setText("正在连接电脑…");
        client.listConversations(provider, new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> conversations) {
                runOnUiThread(() -> {
                    conversationAdapter.replace(conversations);
                    connectionStatus.setText("已连接 · " + conversations.size() + " 个会话");
                    boolean hasConversations = !conversations.isEmpty();
                    emptyState.setVisibility(hasConversations ? View.GONE : View.VISIBLE);
                    if (!hasConversations) {
                        emptyState.setText("电脑上暂无此提供方会话");
                        terminalScroll.setVisibility(View.GONE);
                        inputBar.setVisibility(View.GONE);
                        setControlsVisible(false);
                    } else if (selectedConversationId == null) {
                        selectConversation(conversations.get(0));
                    }
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    connectionStatus.setText("连接失败：" + message);
                    emptyState.setText("请确认电脑端已授权此手机；可在设置中重新扫码");
                    emptyState.setVisibility(View.VISIBLE);
                    Toast.makeText(MainActivity.this, message, Toast.LENGTH_SHORT).show();
                });
            }
        });
    }

    private void selectConversation(AicliClient.Conversation conversation) {
        client.closeEvents();
        selectedConversationId = conversation.id;
        conversationAdapter.select(conversation.id);
        lastEventId = 0L;
        terminalText.setLength(0);
        terminalScroll.setVisibility(View.VISIBLE);
        inputBar.setVisibility(View.VISIBLE);
        setControlsVisible(true);
        appendOutput("[" + conversation.name + "] 状态：" + conversation.status);
        client.listEvents(conversation.id, 0, new AicliClient.HistoryCallback() {
            @Override public void onSuccess(List<String> events) {
                runOnUiThread(() -> {
                    if (!conversation.id.equals(selectedConversationId)) return;
                    for (String event : events) consumeEvent(event);
                    connectRealtime(conversation.id);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    if (!conversation.id.equals(selectedConversationId)) return;
                    appendOutput("[历史记录] " + message);
                    connectRealtime(conversation.id);
                });
            }
        });
    }

    private void sendInput() {
        if (selectedConversationId == null) return;
        // Do not trim: spaces and control characters are meaningful to a CLI.
        String input = messageInput.getText().toString();
        if (input.isEmpty()) return;
        messageInput.setText("");
        appendOutput("> " + input);
        client.sendInput(selectedConversationId, input + "\r", new AicliClient.ActionCallback() {
            @Override public void onSuccess() { }
            @Override public void onError(String message) {
                runOnUiThread(() -> appendOutput("[发送失败] " + message));
            }
        });
    }

    private void sendControl(String action) {
        if (client == null || selectedConversationId == null) return;
        client.control(selectedConversationId, action, new AicliClient.ActionCallback() {
            @Override public void onSuccess() {
                runOnUiThread(() -> connectionStatus.setText("操作已发送：" + action));
            }
            @Override public void onError(String message) {
                runOnUiThread(() -> Toast.makeText(MainActivity.this, message, Toast.LENGTH_SHORT).show());
            }
        });
    }

    private void handleEvent(String eventJson) {
        try {
            JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
            String type = event.has("type") ? event.get("type").getAsString() : "";
            if ("hello".equals(type)) {
                connectionStatus.setText("已连接 · 实时事件在线");
                return;
            }
            String eventConversationId = event.has("conversationId")
                    ? event.get("conversationId").getAsString() : "";
            if (selectedConversationId != null && !selectedConversationId.equals(eventConversationId)) return;
            if (!consumeEvent(eventJson)) return;
            if (type.startsWith("conversation.")) {
                loadConversations();
            }
        } catch (Exception ignored) {
            appendOutput(eventJson);
        }
    }

    private boolean consumeEvent(String eventJson) {
        try {
            JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
            if (event.has("id")) {
                long eventId = event.get("id").getAsLong();
                if (eventId > 0 && eventId <= lastEventId) return false;
                lastEventId = Math.max(lastEventId, eventId);
            }
        } catch (Exception ignored) { }
        appendOutput(formatEvent(eventJson));
        return true;
    }

    private void connectRealtime(String conversationId) {
        if (client == null || conversationId == null || conversationId.isEmpty()) return;
        client.connectEvents(conversationId, new AicliClient.EventCallback() {
            @Override public void onEvent(String eventJson) {
                runOnUiThread(() -> handleEvent(eventJson));
            }

            @Override public void onClosed() {
                runOnUiThread(() -> {
                    connectionStatus.setText("实时连接断开，正在补发事件并自动重连…");
                    recoverMissedEvents();
                });
            }
        });
    }

    private void recoverMissedEvents() {
        String conversationId = selectedConversationId;
        long after = lastEventId;
        if (client == null || conversationId == null) return;
        client.listEvents(conversationId, after, new AicliClient.HistoryCallback() {
            @Override public void onSuccess(List<String> events) {
                runOnUiThread(() -> {
                    if (!conversationId.equals(selectedConversationId)) return;
                    for (String event : events) consumeEvent(event);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> connectionStatus.setText("补发失败，正在继续重连…"));
            }
        });
    }

    private void appendOutput(String line) {
        if (line == null || line.isEmpty()) return;
        if (terminalText.length() > 64 * 1024) terminalText.delete(0, terminalText.length() - 48 * 1024);
        if (terminalText.length() > 0) terminalText.append('\n');
        terminalText.append(line);
        terminalOutput.setText(terminalText.toString());
        terminalOutput.post(() -> terminalScroll.fullScroll(View.FOCUS_DOWN));
    }

    private String formatEvent(String eventJson) {
        try {
            JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
            String type = event.has("event_type") ? event.get("event_type").getAsString()
                    : event.has("type") ? event.get("type").getAsString() : "";
            JsonObject payload = event.has("payload") && event.get("payload").isJsonObject()
                    ? event.getAsJsonObject("payload") : null;
            if (payload != null) {
                if (payload.has("text")) return payload.get("text").getAsString();
                if (payload.has("message")) return "> " + payload.get("message").getAsString();
                if (payload.has("input")) return "> " + payload.get("input").getAsString();
            }
            return type.isEmpty() ? eventJson : "[" + type + "]";
        } catch (Exception ignored) { return eventJson; }
    }

    private void setControlsVisible(boolean visible) {
        findViewById(R.id.controlBar).setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    private void toggleConversationList() {
        listExpanded = !listExpanded;
        conversationList.setVisibility(listExpanded ? View.VISIBLE : View.GONE);
    }
}
