package com.aitest.aicli;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.OnBackPressedCallback;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.GravityCompat;
import androidx.drawerlayout.widget.DrawerLayout;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/** Native Android companion for the desktop-owned AI CLI sessions. */
public final class MainActivity extends AppCompatActivity {
    private static final String CONNECTION_PREFS = "connection";
    private static final String BASE_URL_KEY = "baseUrl";
    private static final String APP_PREFS = "app_settings";
    private static final String ALL_GROUPS = "__all__";

    private final StringBuilder terminalText = new StringBuilder();
    private final List<AicliClient.Conversation> allConversations = new ArrayList<>();
    private final List<String> displayedGroups = new ArrayList<>();
    private AicliClient client;
    private ConversationAdapter conversationAdapter;
    private String selectedGroup = ALL_GROUPS;
    private String selectedConversationId;
    private long lastEventId;
    private String configuredConnectionKey = "";
    private boolean rebuildingGroups;

    private DrawerLayout drawerLayout;
    private View groupSelectorLayout;
    private MaterialAutoCompleteTextView groupSelector;
    private View emptyState;
    private TextView emptyStateTitle;
    private TextView emptyStateMessage;
    private TextView connectionStatus;
    private TextView drawerConnectionStatus;
    private TextView drawerGroupLabel;
    private TextView terminalOutput;
    private ScrollView terminalScroll;
    private View inputBar;
    private EditText messageInput;
    private RecyclerView conversationList;

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

        drawerLayout = findViewById(R.id.drawerLayout);
        groupSelectorLayout = findViewById(R.id.groupSelectorLayout);
        groupSelector = findViewById(R.id.groupSelector);
        emptyState = findViewById(R.id.emptyState);
        emptyStateTitle = findViewById(R.id.emptyStateTitle);
        emptyStateMessage = findViewById(R.id.emptyStateMessage);
        connectionStatus = findViewById(R.id.connectionStatus);
        drawerConnectionStatus = findViewById(R.id.drawerConnectionStatus);
        drawerGroupLabel = findViewById(R.id.drawerGroupLabel);
        terminalOutput = findViewById(R.id.terminalOutput);
        terminalScroll = findViewById(R.id.terminalScroll);
        inputBar = findViewById(R.id.inputBar);
        messageInput = findViewById(R.id.messageInput);
        conversationList = findViewById(R.id.conversationList);

        selectedGroup = getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getString("selected_group", ALL_GROUPS);
        conversationList.setLayoutManager(new LinearLayoutManager(this));
        conversationAdapter = new ConversationAdapter(this::selectConversation);
        conversationList.setAdapter(conversationAdapter);

        groupSelector.setOnItemClickListener((parent, view, position, id) -> {
            if (rebuildingGroups || position < 0 || position >= displayedGroups.size()) return;
            selectedGroup = displayedGroups.get(position);
            getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                    .putString("selected_group", selectedGroup).apply();
            renderSelectedGroup(true);
        });

        findViewById(R.id.settingsButton).setOnClickListener(view ->
                startActivity(new Intent(this, SettingsActivity.class)));
        findViewById(R.id.scanButton).setOnClickListener(view -> openScannerInSettings());
        findViewById(R.id.conversationsButton).setOnClickListener(view -> toggleConversationList());
        findViewById(R.id.closeDrawerButton).setOnClickListener(view ->
                drawerLayout.closeDrawer(GravityCompat.START));
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

        getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
            @Override public void handleOnBackPressed() {
                if (drawerLayout.isDrawerOpen(GravityCompat.START)) {
                    drawerLayout.closeDrawer(GravityCompat.START);
                } else {
                    setEnabled(false);
                    getOnBackPressedDispatcher().onBackPressed();
                }
            }
        });

        configureClient();
    }

    @Override
    protected void onResume() {
        super.onResume();
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
            setConnectionStatus("未绑定电脑");
            showEmptyState("连接你的 AI 工作台", "扫描电脑端二维码后，即可查看进度并继续当前会话。");
            allConversations.clear();
            displayedGroups.clear();
            groupSelector.setAdapter(null);
            groupSelector.setText("", false);
            groupSelectorLayout.setVisibility(View.GONE);
            hideConversation();
            return;
        }
        loadConversations();
    }

    private void loadConversations() {
        if (client == null || client.baseUrl().isEmpty()) return;
        setConnectionStatus("正在连接电脑…");
        client.listConversations(new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> conversations) {
                runOnUiThread(() -> {
                    allConversations.clear();
                    allConversations.addAll(conversations);
                    rebuildGroupSelector(conversations);
                    setConnectionStatus("已连接 · " + conversations.size() + " 个项目");
                    renderSelectedGroup(false);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    setConnectionStatus("连接失败");
                    showEmptyState("暂时无法连接电脑", "请确认电脑端在线且已授权此手机；也可以在设置中重新扫码。");
                    Toast.makeText(MainActivity.this, message, Toast.LENGTH_SHORT).show();
                });
            }
        });
    }

    private void rebuildGroupSelector(List<AicliClient.Conversation> conversations) {
        Set<String> unique = new LinkedHashSet<>();
        for (AicliClient.Conversation conversation : conversations) {
            unique.add(conversation.group.isEmpty() ? "默认分组" : conversation.group);
        }
        List<String> next = new ArrayList<>();
        next.add(ALL_GROUPS);
        next.addAll(unique);
        displayedGroups.clear();
        displayedGroups.addAll(next);
        if (!displayedGroups.contains(selectedGroup)) selectedGroup = ALL_GROUPS;

        rebuildingGroups = true;
        List<String> labels = new ArrayList<>();
        int selectedIndex = 0;
        for (int index = 0; index < displayedGroups.size(); index++) {
            String group = displayedGroups.get(index);
            int count = 0;
            for (AicliClient.Conversation conversation : conversations) {
                String itemGroup = conversation.group.isEmpty() ? "默认分组" : conversation.group;
                if (ALL_GROUPS.equals(group) || group.equals(itemGroup)) count++;
            }
            labels.add((ALL_GROUPS.equals(group) ? "全部" : group) + " (" + count + ")");
            if (group.equals(selectedGroup)) selectedIndex = index;
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, R.layout.item_group_dropdown, labels);
        groupSelector.setAdapter(adapter);
        if (!labels.isEmpty()) groupSelector.setText(labels.get(selectedIndex), false);
        groupSelectorLayout.setVisibility(View.VISIBLE);
        rebuildingGroups = false;
    }

    private void renderSelectedGroup(boolean selectFirst) {
        List<AicliClient.Conversation> filtered = new ArrayList<>();
        for (AicliClient.Conversation conversation : allConversations) {
            String group = conversation.group.isEmpty() ? "默认分组" : conversation.group;
            if (ALL_GROUPS.equals(selectedGroup) || selectedGroup.equals(group)) filtered.add(conversation);
        }
        conversationAdapter.replace(filtered);
        drawerGroupLabel.setText(ALL_GROUPS.equals(selectedGroup)
                ? "全部分组 · " + filtered.size() + " 个项目"
                : selectedGroup + " · " + filtered.size() + " 个项目");
        if (filtered.isEmpty()) {
            selectedConversationId = null;
            conversationAdapter.select(null);
            hideConversation();
            showEmptyState("这个分组还没有项目", "请先在电脑端创建项目，手机会自动同步最新分组和会话。");
            return;
        }

        AicliClient.Conversation selected = null;
        if (!selectFirst && selectedConversationId != null) {
            for (AicliClient.Conversation value : filtered) {
                if (value.id.equals(selectedConversationId)) {
                    selected = value;
                    break;
                }
            }
        }
        if (selected == null) selected = filtered.get(0);
        if (!selected.id.equals(selectedConversationId)) selectConversation(selected);
        else {
            conversationAdapter.select(selected.id);
            hideEmptyState();
        }
    }

    private void selectConversation(AicliClient.Conversation conversation) {
        if (client == null) return;
        client.closeEvents();
        selectedConversationId = conversation.id;
        conversationAdapter.select(conversation.id);
        drawerLayout.closeDrawer(GravityCompat.START);
        lastEventId = 0L;
        terminalText.setLength(0);
        terminalOutput.setText("");
        terminalScroll.setVisibility(View.VISIBLE);
        inputBar.setVisibility(View.VISIBLE);
        setControlsVisible(true);
        hideEmptyState();
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
        if (selectedConversationId == null || client == null) return;
        String input = messageInput.getText().toString();
        if (input.isEmpty()) return;
        messageInput.setText("");
        client.sendMessage(selectedConversationId, input, new AicliClient.ActionCallback() {
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
                runOnUiThread(() -> setConnectionStatus("操作已发送 · " + action));
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
                setConnectionStatus("实时连接在线");
                return;
            }
            String eventConversationId = event.has("conversationId")
                    ? event.get("conversationId").getAsString() : "";
            if (selectedConversationId != null && !selectedConversationId.equals(eventConversationId)) return;
            if (!consumeEvent(eventJson)) return;
            if (type.startsWith("conversation.")) loadConversations();
        } catch (Exception ignored) {
            appendOutput(eventJson);
        }
    }

    private boolean consumeEvent(String eventJson) {
        String type = "";
        JsonObject payload = null;
        try {
            JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
            type = event.has("event_type") ? event.get("event_type").getAsString()
                    : event.has("type") ? event.get("type").getAsString() : "";
            payload = event.has("payload") && event.get("payload").isJsonObject()
                    ? event.getAsJsonObject("payload") : null;
            if (event.has("id")) {
                long eventId = event.get("id").getAsLong();
                if (eventId > 0 && eventId <= lastEventId) return false;
                lastEventId = Math.max(lastEventId, eventId);
            }
        } catch (Exception ignored) { }
        if ("terminal.snapshot".equals(type) && payload != null && payload.has("text")) {
            setTerminalSnapshot(payload.get("text").getAsString());
            return true;
        }
        // Legacy desktop builds emitted every TUI repaint as a separate text
        // event. They are intentionally ignored once screen snapshots are
        // supported, otherwise Working/composer/status rows accumulate.
        if ("terminal.output".equals(type) || "terminal.input".equals(type)
                || type.startsWith("conversation.")) return true;
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
                    setConnectionStatus("实时连接断开 · 正在重连");
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
                runOnUiThread(() -> setConnectionStatus("补发失败 · 正在继续重连"));
            }
        });
    }

    private void appendOutput(String line) {
        if (line == null || line.isEmpty()) return;
        if (terminalText.length() > 64 * 1024) {
            terminalText.delete(0, terminalText.length() - 48 * 1024);
        }
        if (terminalText.length() > 0) terminalText.append('\n');
        terminalText.append(line);
        terminalOutput.setText(terminalText.toString());
        terminalOutput.post(() -> terminalScroll.fullScroll(View.FOCUS_DOWN));
    }

    private void setTerminalSnapshot(String text) {
        terminalText.setLength(0);
        if (text != null) terminalText.append(text);
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
        } catch (Exception ignored) {
            return eventJson;
        }
    }

    private void setControlsVisible(boolean visible) {
        findViewById(R.id.controlBar).setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    private void toggleConversationList() {
        if (drawerLayout.isDrawerOpen(GravityCompat.START)) {
            drawerLayout.closeDrawer(GravityCompat.START);
        } else {
            drawerLayout.openDrawer(GravityCompat.START);
        }
    }

    private void setConnectionStatus(String value) {
        connectionStatus.setText(value);
        drawerConnectionStatus.setText(value);
    }

    private void showEmptyState(String title, String message) {
        emptyStateTitle.setText(title);
        emptyStateMessage.setText(message);
        emptyState.setVisibility(View.VISIBLE);
    }

    private void hideEmptyState() {
        emptyState.setVisibility(View.GONE);
    }

    private void hideConversation() {
        if (client != null) client.closeEvents();
        terminalScroll.setVisibility(View.GONE);
        inputBar.setVisibility(View.GONE);
        setControlsVisible(false);
    }
}
