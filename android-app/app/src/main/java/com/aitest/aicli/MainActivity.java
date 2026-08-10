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
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.OnBackPressedCallback;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.GravityCompat;
import androidx.drawerlayout.widget.DrawerLayout;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.google.android.material.button.MaterialButton;
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
    private static final String LAST_CONVERSATION_PREFIX = "last_conversation.";
    private static final long[] POST_SEND_REPLAY_DELAYS_MS = {220L, 900L};

    private final List<AicliClient.Conversation> allConversations = new ArrayList<>();
    private final List<String> displayedGroups = new ArrayList<>();
    private final List<ComputerConnectionStore.Computer> displayedComputers = new ArrayList<>();
    private ComputerConnectionStore connectionStore;
    private AicliClient client;
    private ConversationAdapter conversationAdapter;
    private String selectedGroup = ALL_GROUPS;
    private String selectedConversationId;
    private String pendingConversationId;
    private AicliClient.Conversation selectedConversation;
    private long lastEventId;
    private String configuredConnectionKey = "";
    private boolean rebuildingGroups;
    private boolean rebuildingComputers;
    private long serverGeneration;

    private DrawerLayout drawerLayout;
    private View computerSelectorLayout;
    private MaterialAutoCompleteTextView computerSelector;
    private View groupSelectorLayout;
    private MaterialAutoCompleteTextView groupSelector;
    private View emptyState;
    private TextView emptyStateTitle;
    private TextView emptyStateMessage;
    private TextView currentProjectTitle;
    private TextView currentProjectMeta;
    private TextView drawerConnectionStatus;
    private TextView drawerGroupLabel;
    private TerminalView terminalView;
    private TerminalTextRenderer terminalRenderer;
    private View terminalKeyBar;
    private View inputBar;
    private MaterialButton startProjectButton;
    private MaterialButton wrapButton;
    private EditText messageInput;
    private RecyclerView conversationList;
    private boolean terminalReconnectScheduled;
    private long postSendReplayGeneration;

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
        connectionStore = new ComputerConnectionStore(this);

        drawerLayout = findViewById(R.id.drawerLayout);
        computerSelectorLayout = findViewById(R.id.computerSelectorLayout);
        computerSelector = findViewById(R.id.computerSelector);
        groupSelectorLayout = findViewById(R.id.groupSelectorLayout);
        groupSelector = findViewById(R.id.groupSelector);
        emptyState = findViewById(R.id.emptyState);
        emptyStateTitle = findViewById(R.id.emptyStateTitle);
        emptyStateMessage = findViewById(R.id.emptyStateMessage);
        currentProjectTitle = findViewById(R.id.currentProjectTitle);
        currentProjectMeta = findViewById(R.id.currentProjectMeta);
        drawerConnectionStatus = findViewById(R.id.drawerConnectionStatus);
        drawerGroupLabel = findViewById(R.id.drawerGroupLabel);
        terminalView = findViewById(R.id.terminalView);
        terminalRenderer = new TerminalTextRenderer(terminalView);
        terminalKeyBar = findViewById(R.id.terminalKeyBar);
        inputBar = findViewById(R.id.inputBar);
        startProjectButton = findViewById(R.id.startProjectButton);
        wrapButton = findViewById(R.id.wrapButton);
        messageInput = findViewById(R.id.messageInput);
        conversationList = findViewById(R.id.conversationList);

        selectedGroup = getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getString("selected_group", ALL_GROUPS);
        terminalView.setSoftWrapEnabled(getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getBoolean("terminal_soft_wrap", true));
        updateWrapButton();
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

        computerSelector.setOnItemClickListener((parent, view, position, id) -> {
            if (rebuildingComputers || position < 0 ||
                    position >= displayedComputers.size()) return;
            switchComputer(displayedComputers.get(position));
        });

        findViewById(R.id.settingsButton).setOnClickListener(view ->
                startActivity(new Intent(this, SettingsActivity.class)));
        findViewById(R.id.scanButton).setOnClickListener(view -> openScannerInSettings());
        findViewById(R.id.conversationsButton).setOnClickListener(view -> toggleConversationList());
        findViewById(R.id.closeDrawerButton).setOnClickListener(view ->
                drawerLayout.closeDrawer(GravityCompat.START));
        findViewById(R.id.sendButton).setOnClickListener(view -> sendInput());
        startProjectButton.setOnClickListener(view -> startSelectedProject());
        wrapButton.setOnClickListener(view -> toggleSoftWrap());
        findViewById(R.id.keyLeftButton).setOnClickListener(view -> sendTerminalKey("left"));
        findViewById(R.id.keyUpButton).setOnClickListener(view -> sendTerminalKey("up"));
        findViewById(R.id.keyDownButton).setOnClickListener(view -> sendTerminalKey("down"));
        findViewById(R.id.keyRightButton).setOnClickListener(view -> sendTerminalKey("right"));
        findViewById(R.id.keyEnterButton).setOnClickListener(view -> sendTerminalKey("enter"));

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
        ComputerConnectionStore.Computer activeComputer = connectionStore.active();
        refreshComputerSelector(activeComputer);
        String baseUrl = activeComputer == null
                ? preferences.getString(BASE_URL_KEY, "") : activeComputer.baseUrl;
        String connectionKey = activeComputer == null
                ? preferences.getString(BASE_URL_KEY, "") + "\n"
                    + preferences.getString("localAddress", "") + "\n"
                    + preferences.getString("remoteAddress", "") + "\n"
                    + preferences.getString("routePreference",
                            ComputerConnectionStore.ROUTE_LOCAL) + "\n"
                    + preferences.getString("certificateSha256", "") + "\n"
                    + preferences.getString("computerId", "")
                : activeComputer.connectionKey();
        if (client == null || !connectionKey.equals(configuredConnectionKey)) {
            if (client != null) client.close();
            client = new AicliClient(this, "");
            configuredConnectionKey = connectionKey;
            serverGeneration++;
            resetForComputerChange();
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
        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        final String computerName = activeComputerName();
        setConnectionStatus(computerName + " · 正在连接…");
        requestClient.listConversations(new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> conversations) {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration) return;
                    allConversations.clear();
                    allConversations.addAll(conversations);
                    refreshComputerSelector(connectionStore.active());
                    prepareLastConversationRestore(conversations);
                    rebuildGroupSelector(conversations);
                    setConnectionStatus(activeComputerName() + " · 已连接 · "
                            + conversations.size() + " 个项目");
                    renderSelectedGroup(false);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration) return;
                    setConnectionStatus(computerName + " · 连接失败");
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
            pendingConversationId = null;
            selectedConversationId = null;
            selectedConversation = null;
            conversationAdapter.select(null);
            hideConversation();
            showEmptyState("这个分组还没有项目", "请先在电脑端创建项目，手机会自动同步最新分组和会话。");
            return;
        }

        AicliClient.Conversation selected = null;
        String desiredConversationId = selectFirst ? null : selectedConversationId;
        if (desiredConversationId == null && !selectFirst) {
            desiredConversationId = pendingConversationId;
        }
        if (desiredConversationId != null) {
            for (AicliClient.Conversation value : filtered) {
                if (value.id.equals(desiredConversationId)) {
                    selected = value;
                    break;
                }
            }
        }
        pendingConversationId = null;
        if (selected == null) selected = filtered.get(0);
        updateProjectHeader(selected);
        if (!selected.id.equals(selectedConversationId) || selectedConversation == null) {
            selectConversation(selected);
        }
        else {
            selectedConversation = selected;
            conversationAdapter.select(selected.id);
            updateProjectControls(selected);
            hideEmptyState();
        }
    }

    private void selectConversation(AicliClient.Conversation conversation) {
        if (client == null) return;
        client.closeEvents();
        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        selectedConversationId = conversation.id;
        selectedConversation = conversation;
        rememberSelectedConversation(conversation.id);
        updateProjectHeader(conversation);
        conversationAdapter.select(conversation.id);
        drawerLayout.closeDrawer(GravityCompat.START);
        lastEventId = 0L;
        terminalRenderer.selectSession(conversation.id);
        terminalRenderer.followBottom();
        terminalView.setVisibility(View.VISIBLE);
        updateProjectControls(conversation);
        hideEmptyState();
        client.listEvents(conversation.id, 0, new AicliClient.HistoryCallback() {
            @Override public void onSuccess(List<String> events) {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration ||
                            !conversation.id.equals(selectedConversationId)) return;
                    for (String event : events) consumeEvent(event);
                    terminalRenderer.followBottom();
                    connectRealtime(conversation.id);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration ||
                            !conversation.id.equals(selectedConversationId)) return;
                    appendOutput("[历史记录] " + message);
                    terminalRenderer.followBottom();
                    connectRealtime(conversation.id);
                });
            }
        });
    }

    private void sendInput() {
        if (selectedConversationId == null || client == null) return;
        String input = messageInput.getText().toString();
        if (input.isEmpty()) return;
        String conversationId = selectedConversationId;
        // Sending a new turn is an explicit request to return to the live
        // edge, even if an earlier synchronized Claude repaint temporarily
        // left the viewport above the newest terminal rows.
        terminalRenderer.followBottom();
        messageInput.setText("");
        client.sendMessage(conversationId, input, new AicliClient.ActionCallback() {
            @Override public void onSuccess() {
                runOnUiThread(() -> schedulePostSendTerminalReplay(conversationId));
            }
            @Override public void onError(String message) {
                runOnUiThread(() -> appendOutput("[发送失败] " + message));
            }
        });
    }

    /**
     * Terminal data is replayed by sequence over WebSocket rather than by the
     * SQLite history endpoint. If the last live frame is lost there is no
     * later frame that can reveal the sequence gap, so reconnect briefly after
     * an accepted submission and ask the desktop to resume from our last
     * rendered sequence. Two passes also cover output racing with the first
     * WebSocket handshake without duplicating terminal content.
     */
    private void schedulePostSendTerminalReplay(String conversationId) {
        if (conversationId == null || !conversationId.equals(selectedConversationId)) return;
        final long generation = ++postSendReplayGeneration;
        for (long delay : POST_SEND_REPLAY_DELAYS_MS) {
            terminalView.postDelayed(() -> {
                if (generation != postSendReplayGeneration || client == null ||
                        !conversationId.equals(selectedConversationId)) return;
                connectRealtime(conversationId);
            }, delay);
        }
    }

    private void sendTerminalKey(String key) {
        if (selectedConversationId == null || client == null) return;
        client.sendKey(selectedConversationId, key, new AicliClient.ActionCallback() {
            @Override public void onSuccess() { }
            @Override public void onError(String message) {
                runOnUiThread(() -> Toast.makeText(
                        MainActivity.this, "按键发送失败：" + message, Toast.LENGTH_SHORT).show());
            }
        });
    }

    private void startSelectedProject() {
        if (selectedConversationId == null || client == null) return;
        String conversationId = selectedConversationId;
        terminalRenderer.followBottom();
        startProjectButton.setEnabled(false);
        startProjectButton.setText("正在启动…");
        client.control(conversationId, "start", new AicliClient.ActionCallback() {
            @Override public void onSuccess() {
                runOnUiThread(() -> terminalView.postDelayed(() -> {
                    if (!conversationId.equals(selectedConversationId)) return;
                    terminalRenderer.followBottom();
                    loadConversations();
                }, 350L));
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    startProjectButton.setEnabled(true);
                    updateProjectControls(selectedConversation);
                    Toast.makeText(MainActivity.this,
                            "项目启动失败：" + message, Toast.LENGTH_SHORT).show();
                });
            }
        });
    }

    private void toggleSoftWrap() {
        boolean enabled = !terminalView.softWrapEnabled();
        terminalView.setSoftWrapEnabled(enabled);
        getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                .putBoolean("terminal_soft_wrap", enabled).apply();
        updateWrapButton();
    }

    private void updateWrapButton() {
        if (wrapButton == null) return;
        boolean enabled = terminalView != null && terminalView.softWrapEnabled();
        wrapButton.setText(enabled ? "换行" : "原始");
        wrapButton.setAlpha(enabled ? 1f : 0.72f);
        wrapButton.setContentDescription(enabled ? "自动换行已开启" : "原始终端布局已开启");
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
        JsonObject event = null;
        try {
            event = JsonParser.parseString(eventJson).getAsJsonObject();
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
        if ("terminal.reset".equals(type) && event != null && payload != null) {
            String sessionId = stringValue(event, "terminalSessionId");
            long baseSequence = longValue(event, "baseSequence", 0L);
            int rows = intValue(payload, "rows", 32);
            int columns = intValue(payload, "columns", 120);
            terminalRenderer.reset(sessionId, baseSequence, rows, columns);
            return true;
        }
        if (("terminal.data".equals(type) || "terminal.resize".equals(type)) &&
                event != null && payload != null) {
            String sessionId = stringValue(event, "terminalSessionId");
            long sequence = longValue(event, "sequence", 0L);
            TerminalTextRenderer.StreamResult result;
            if ("terminal.data".equals(type)) {
                result = terminalRenderer.data(sessionId, sequence,
                        stringValue(payload, "data"));
            } else {
                result = terminalRenderer.resize(sessionId, sequence,
                        intValue(payload, "rows", 32),
                        intValue(payload, "columns", 120));
            }
            if (result == TerminalTextRenderer.StreamResult.GAP) {
                requestTerminalReplay();
            }
            return true;
        }
        // Plain snapshots remain an HTTP recovery artifact for SQLite and old
        // desktop builds. The native terminal must never mix them into the VT
        // screen or the same Codex repaint appears twice.
        if ("terminal.snapshot".equals(type) || "terminal.output".equals(type) ||
                "terminal.input".equals(type)
                || type.startsWith("conversation.")) return true;
        appendOutput(formatEvent(eventJson));
        return true;
    }

    private void connectRealtime(String conversationId) {
        if (client == null || conversationId == null || conversationId.isEmpty()) return;
        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        requestClient.connectEvents(conversationId, new AicliClient.EventCallback() {
            @Override public void onEvent(String eventJson) {
                runOnUiThread(() -> {
                    if (client == requestClient && serverGeneration == requestGeneration) {
                        handleEvent(eventJson);
                    }
                });
            }

            @Override public void onClosed() {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration) return;
                    setConnectionStatus("实时连接断开 · 正在重连");
                    recoverMissedEvents();
                });
            }

            @Override public String terminalSessionId() {
                return terminalRenderer.currentTerminalSessionId();
            }

            @Override public long terminalSequence() {
                return terminalRenderer.currentSequence();
            }
        });
    }

    private void requestTerminalReplay() {
        if (terminalReconnectScheduled) return;
        terminalReconnectScheduled = true;
        String conversationId = selectedConversationId;
        setConnectionStatus("终端数据补发中…");
        terminalView.postDelayed(() -> {
            terminalReconnectScheduled = false;
            if (conversationId != null && conversationId.equals(selectedConversationId)) {
                connectRealtime(conversationId);
            }
        }, 180L);
    }

    private void recoverMissedEvents() {
        String conversationId = selectedConversationId;
        long after = lastEventId;
        if (client == null || conversationId == null) return;
        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        requestClient.listEvents(conversationId, after, new AicliClient.HistoryCallback() {
            @Override public void onSuccess(List<String> events) {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration ||
                            !conversationId.equals(selectedConversationId)) return;
                    for (String event : events) consumeEvent(event);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    if (client == requestClient && serverGeneration == requestGeneration) {
                        setConnectionStatus("补发失败 · 正在继续重连");
                    }
                });
            }
        });
    }

    private void appendOutput(String line) {
        terminalRenderer.appendLine(line);
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

    private void toggleConversationList() {
        if (drawerLayout.isDrawerOpen(GravityCompat.START)) {
            drawerLayout.closeDrawer(GravityCompat.START);
        } else {
            drawerLayout.openDrawer(GravityCompat.START);
        }
    }

    private void setConnectionStatus(String value) {
        drawerConnectionStatus.setText(value);
    }

    private void switchComputer(ComputerConnectionStore.Computer computer) {
        if (computer == null) return;
        ComputerConnectionStore.Computer active = connectionStore.active();
        if (active != null && active.computerId.equals(computer.computerId)) return;
        if (!connectionStore.activate(computer.computerId)) return;
        configuredConnectionKey = "";
        setConnectionStatus(computer.displayName() + " · 正在切换…");
        configureClient();
    }

    private void refreshComputerSelector(ComputerConnectionStore.Computer active) {
        displayedComputers.clear();
        displayedComputers.addAll(connectionStore.all());
        List<String> labels = new ArrayList<>();
        int selectedIndex = -1;
        for (int index = 0; index < displayedComputers.size(); index++) {
            ComputerConnectionStore.Computer computer = displayedComputers.get(index);
            labels.add(computerLabel(computer));
            if (active != null && active.computerId.equals(computer.computerId)) {
                selectedIndex = index;
            }
        }
        rebuildingComputers = true;
        computerSelector.setAdapter(new ArrayAdapter<>(
                this, R.layout.item_group_dropdown, labels));
        computerSelector.setEnabled(!labels.isEmpty());
        computerSelector.setText(selectedIndex >= 0
                ? labels.get(selectedIndex) : "未绑定服务端", false);
        computerSelectorLayout.setVisibility(View.VISIBLE);
        rebuildingComputers = false;
    }

    private void resetForComputerChange() {
        postSendReplayGeneration++;
        terminalReconnectScheduled = false;
        selectedConversationId = null;
        pendingConversationId = null;
        selectedConversation = null;
        lastEventId = 0L;
        allConversations.clear();
        displayedGroups.clear();
        conversationAdapter.replace(new ArrayList<>());
        conversationAdapter.select(null);
        groupSelector.setAdapter(null);
        groupSelector.setText("", false);
        groupSelectorLayout.setVisibility(View.GONE);
        drawerGroupLabel.setText("暂无项目");
        terminalRenderer.clearAll();
        hideConversation();
    }

    private void prepareLastConversationRestore(
            List<AicliClient.Conversation> conversations) {
        pendingConversationId = null;
        if (selectedConversationId != null || conversations == null || conversations.isEmpty()) {
            return;
        }
        String savedId = getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getString(lastConversationPreferenceKey(), "");
        if (savedId == null || savedId.isEmpty()) return;
        for (AicliClient.Conversation conversation : conversations) {
            if (!savedId.equals(conversation.id)) continue;
            pendingConversationId = savedId;
            selectedGroup = conversation.group.isEmpty()
                    ? "默认分组" : conversation.group;
            getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                    .putString("selected_group", selectedGroup).apply();
            return;
        }
    }

    private void rememberSelectedConversation(String conversationId) {
        if (conversationId == null || conversationId.isEmpty()) return;
        getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                .putString(lastConversationPreferenceKey(), conversationId).apply();
    }

    private String lastConversationPreferenceKey() {
        ComputerConnectionStore.Computer active = connectionStore == null
                ? null : connectionStore.active();
        String computerId = active == null ? "legacy" : active.computerId;
        if (computerId == null || computerId.isEmpty()) computerId = "legacy";
        return LAST_CONVERSATION_PREFIX + computerId;
    }

    private String activeComputerName() {
        ComputerConnectionStore.Computer active = connectionStore.active();
        return active == null ? "电脑" : active.displayName();
    }

    private static String computerLabel(ComputerConnectionStore.Computer computer) {
        return computer.displayName();
    }

    private void updateProjectHeader(AicliClient.Conversation conversation) {
        if (conversation == null) {
            currentProjectTitle.setText("未选择项目");
            currentProjectMeta.setText("项目分组 · 项目类型");
            return;
        }
        String group = conversation.group.isEmpty() ? "默认分组" : conversation.group;
        String type = conversation.type.isEmpty() ? conversation.provider : conversation.type;
        currentProjectTitle.setText(conversation.name.isEmpty() ? "未命名项目" : conversation.name);
        currentProjectMeta.setText(group + " · " + (type.isEmpty() ? "未设置类型" : type));
    }

    private void updateProjectControls(AicliClient.Conversation conversation) {
        boolean running = conversation != null &&
                "running".equalsIgnoreCase(conversation.status);
        terminalKeyBar.setVisibility(running ? View.VISIBLE : View.GONE);
        inputBar.setVisibility(running ? View.VISIBLE : View.GONE);
        startProjectButton.setVisibility(conversation != null && !running
                ? View.VISIBLE : View.GONE);
        startProjectButton.setEnabled(true);
        if (!running && conversation != null) {
            boolean resumable = "paused".equalsIgnoreCase(conversation.status);
            startProjectButton.setText(resumable ? "继续项目" : "开始项目");
        }
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
        selectedConversation = null;
        updateProjectHeader(null);
        terminalRenderer.detach();
        terminalView.setVisibility(View.GONE);
        terminalKeyBar.setVisibility(View.GONE);
        startProjectButton.setVisibility(View.GONE);
        inputBar.setVisibility(View.GONE);
    }

    private static String stringValue(JsonObject object, String name) {
        return object != null && object.has(name) && !object.get(name).isJsonNull()
                ? object.get(name).getAsString() : "";
    }

    private static long longValue(JsonObject object, String name, long fallback) {
        try {
            return object != null && object.has(name) ? object.get(name).getAsLong() : fallback;
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private static int intValue(JsonObject object, String name, int fallback) {
        try {
            return object != null && object.has(name) ? object.get(name).getAsInt() : fallback;
        } catch (Exception ignored) {
            return fallback;
        }
    }
}
