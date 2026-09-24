package com.aitest.aicli;

import android.Manifest;
import android.content.ContentResolver;
import android.content.res.ColorStateList;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Bundle;
import androidx.core.content.FileProvider;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.view.animation.AlphaAnimation;
import android.view.animation.Animation;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.OnBackPressedCallback;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.core.view.GravityCompat;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.drawerlayout.widget.DrawerLayout;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.google.android.material.textfield.TextInputLayout;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.bottomsheet.BottomSheetBehavior;
import com.google.android.material.bottomsheet.BottomSheetDialog;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;

import java.util.ArrayList;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Native Android companion for the desktop-owned AI CLI sessions. */
public final class MainActivity extends AppCompatActivity {
    private static final String CONNECTION_PREFS = "connection";
    private static final String BASE_URL_KEY = "baseUrl";
    private static final String APP_PREFS = "app_settings";
    private static final String CONVERSATION_CACHE_PREFS = "conversation_cache";
    private static final String CONVERSATION_CACHE_PREFIX = "items.";
    private static final String ALL_GROUPS = "__all__";
    private static final String RUNNING_GROUP = "__running__";
    private static final String LAST_CONVERSATION_PREFIX = "last_conversation.";
    private static final String LAST_IME_HEIGHT_KEY = "last_ime_height";
    private static final long[] POST_SEND_REPLAY_DELAYS_MS = {220L, 900L};

    private final List<AicliClient.Conversation> allConversations = new ArrayList<>();
    private final List<String> displayedGroups = new ArrayList<>();
    private final List<ComputerConnectionStore.Computer> displayedComputers = new ArrayList<>();
    private ComputerConnectionStore connectionStore;
    private AicliClient client;
    private ConversationAdapter conversationAdapter;
    private String selectedGroup = ALL_GROUPS;
    private String conversationSearchQuery = "";
    private String selectedConversationId;
    private String pendingConversationId;
    private AicliClient.Conversation selectedConversation;
    private long lastEventId;
    private String configuredConnectionKey = "";
    private boolean rebuildingGroups;
    private boolean rebuildingComputers;
    private long serverGeneration;

    private DrawerLayout drawerLayout;
    private View mainHeader;
    private View conversationDrawer;
    private View conversationContent;
    private View computerSelectorLayout;
    private MaterialAutoCompleteTextView computerSelector;
    private View groupSelectorLayout;
    private MaterialAutoCompleteTextView groupSelector;
    private View emptyState;
    private TextView emptyStateTitle;
    private TextView emptyStateMessage;
    private TextView currentComputerTitle;
    private TextView currentProjectMeta;
    private ImageView localConnectionIcon;
    private ImageView remoteConnectionIcon;
    private ImageView localCurrentRouteIcon;
    private ImageView remoteCurrentRouteIcon;
    private TextView drawerConnectionStatus;
    private TextView drawerGroupLabel;
    private EditText conversationSearchInput;
    private View conversationSearchEmpty;
    private TerminalView terminalView;
    private TerminalTextRenderer terminalRenderer;
    private View terminalKeyBar;
    private View inputBar;
    private MaterialButton startProjectButton;
    private MaterialButton wrapButton;
    private MaterialButton sendButton;
    private View closeProjectButton;
    private EditText messageInput;
    private RecyclerView conversationList;
    private boolean terminalReconnectScheduled;
    private long postSendReplayGeneration;
    private boolean terminalKeyPanelVisible;
    private boolean imeVisible;
    private int systemBottomInsetPx;
    private final Rect visibleWindowFrame = new Rect();
    private ViewTreeObserver.OnGlobalLayoutListener visibleWindowLayoutListener;
    private int conversationBasePaddingBottom;
    private int conversationKeyboardInsetPx = Integer.MIN_VALUE;
    private int headerBasePaddingLeft;
    private int headerBasePaddingRight;
    private int headerBasePaddingBottom;
    private int headerTopInsetPx = Integer.MIN_VALUE;
    private int drawerBasePaddingLeft;
    private int drawerBasePaddingTop;
    private int drawerBasePaddingRight;
    private int drawerBasePaddingBottom;
    private int drawerTopInsetPx = Integer.MIN_VALUE;
    private int lastImeHeightPx;
    private ActivityResultLauncher<String> galleryLauncher;
    private ActivityResultLauncher<Uri> cameraLauncher;
    private ActivityResultLauncher<String> cameraPermissionLauncher;
    private Uri pendingCameraUri;
    private File pendingCameraFile;

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
        // API 27 devices ignore ADJUST_RESIZE while the decor remains in
        // layout-fullscreen mode. Material/AppCompat can retain those flags
        // even when the manifest requests resize, so clear them explicitly.
        WindowCompat.setDecorFitsSystemWindows(getWindow(), true);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        registerImageLaunchers();
        setContentView(R.layout.activity_main);
        enforceResizableWindow();
        getWindow().getDecorView().post(this::enforceResizableWindow);
        initializeDeviceIdentity();
        connectionStore = new ComputerConnectionStore(this);

        drawerLayout = findViewById(R.id.drawerLayout);
        mainHeader = findViewById(R.id.mainHeader);
        headerBasePaddingLeft = mainHeader.getPaddingLeft();
        headerBasePaddingRight = mainHeader.getPaddingRight();
        headerBasePaddingBottom = mainHeader.getPaddingBottom();
        conversationDrawer = findViewById(R.id.conversationDrawer);
        drawerBasePaddingLeft = conversationDrawer.getPaddingLeft();
        drawerBasePaddingTop = conversationDrawer.getPaddingTop();
        drawerBasePaddingRight = conversationDrawer.getPaddingRight();
        drawerBasePaddingBottom = conversationDrawer.getPaddingBottom();
        conversationContent = findViewById(R.id.conversationContent);
        conversationBasePaddingBottom = conversationContent.getPaddingBottom();
        computerSelectorLayout = findViewById(R.id.computerSelectorLayout);
        computerSelector = findViewById(R.id.computerSelector);
        groupSelectorLayout = findViewById(R.id.groupSelectorLayout);
        groupSelector = findViewById(R.id.groupSelector);
        emptyState = findViewById(R.id.emptyState);
        emptyStateTitle = findViewById(R.id.emptyStateTitle);
        emptyStateMessage = findViewById(R.id.emptyStateMessage);
        currentComputerTitle = findViewById(R.id.currentComputerTitle);
        currentProjectMeta = findViewById(R.id.currentProjectMeta);
        localConnectionIcon = findViewById(R.id.localConnectionIcon);
        remoteConnectionIcon = findViewById(R.id.remoteConnectionIcon);
        localCurrentRouteIcon = findViewById(R.id.localCurrentRouteIcon);
        remoteCurrentRouteIcon = findViewById(R.id.remoteCurrentRouteIcon);
        drawerConnectionStatus = findViewById(R.id.drawerConnectionStatus);
        drawerGroupLabel = findViewById(R.id.drawerGroupLabel);
        conversationSearchInput = findViewById(R.id.conversationSearchInput);
        conversationSearchEmpty = findViewById(R.id.conversationSearchEmpty);
        terminalView = findViewById(R.id.terminalView);
        terminalRenderer = new TerminalTextRenderer(terminalView);
        terminalView.setHistoryLoadListener(this::loadOlderTerminalHistory);
        terminalView.setUserScrollListener(terminalRenderer::releaseFollowBottom);
        terminalKeyBar = findViewById(R.id.terminalKeyBar);
        inputBar = findViewById(R.id.inputBar);
        startProjectButton = findViewById(R.id.startProjectButton);
        wrapButton = findViewById(R.id.wrapButton);
        sendButton = findViewById(R.id.sendButton);
        closeProjectButton = findViewById(R.id.closeProjectButton);
        messageInput = findViewById(R.id.messageInput);
        conversationList = findViewById(R.id.conversationList);
        lastImeHeightPx = getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getInt(LAST_IME_HEIGHT_KEY, dpToPx(260));

        selectedGroup = getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getString("selected_group", ALL_GROUPS);
        terminalView.setSoftWrapEnabled(getSharedPreferences(APP_PREFS, MODE_PRIVATE)
                .getBoolean("terminal_soft_wrap", true));
        updateWrapButton();
        conversationList.setLayoutManager(new LinearLayoutManager(this));
        conversationAdapter = new ConversationAdapter(this::selectConversation);
        conversationList.setAdapter(conversationAdapter);

        conversationSearchInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence value, int start, int before, int count) {
                conversationSearchQuery = value == null ? "" : value.toString().trim();
                renderSelectedGroup(false);
            }
            @Override public void afterTextChanged(Editable value) {}
        });

        groupSelector.setOnItemClickListener((parent, view, position, id) -> {
            if (rebuildingGroups || position < 0 || position >= displayedGroups.size()) return;
            selectedGroup = displayedGroups.get(position);
            getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                    .putString("selected_group", selectedGroup).apply();
            renderSelectedGroup(true, false);
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
        findViewById(R.id.createProjectButton).setOnClickListener(view ->
                showCreateProjectSheet());
        findViewById(R.id.closeDrawerButton).setOnClickListener(view ->
                drawerLayout.closeDrawer(GravityCompat.START));
        sendButton.setOnClickListener(view -> handleComposerAction());
        startProjectButton.setOnClickListener(view -> startSelectedProject());
        closeProjectButton.setOnClickListener(view -> confirmCloseSelectedProject());
        wrapButton.setOnClickListener(view -> toggleSoftWrap());
        findViewById(R.id.keyLeftButton).setOnClickListener(view -> sendTerminalKey("left"));
        findViewById(R.id.keyUpButton).setOnClickListener(view -> sendTerminalKey("up"));
        findViewById(R.id.keyDownButton).setOnClickListener(view -> sendTerminalKey("down"));
        findViewById(R.id.keyRightButton).setOnClickListener(view -> sendTerminalKey("right"));
        findViewById(R.id.keyEnterButton).setOnClickListener(view -> sendTerminalKey("enter"));
        findViewById(R.id.keyCtrlCButton).setOnClickListener(view -> sendTerminalInterrupt());
        findViewById(R.id.cameraButton).setOnClickListener(view -> openCamera());
        findViewById(R.id.galleryButton).setOnClickListener(view ->
                galleryLauncher.launch("image/*"));

        messageInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start,
                    int count, int after) { }
            @Override public void onTextChanged(CharSequence value, int start,
                    int before, int count) {
                updateComposerAction();
            }
            @Override public void afterTextChanged(Editable value) { }
        });
        messageInput.setOnTouchListener((view, event) -> {
            if (event.getAction() == MotionEvent.ACTION_DOWN && terminalKeyPanelVisible) {
                switchPanelToKeyboard();
            }
            return false;
        });
        messageInput.setOnFocusChangeListener((view, hasFocus) -> {
            if (hasFocus && !terminalKeyPanelVisible) {
                terminalRenderer.followBottom();
                terminalView.post(terminalView::scrollToBottom);
            }
        });
        observeImeHeight();
        updateComposerAction();

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
                if (terminalKeyPanelVisible) {
                    hideTerminalKeyPanel(false);
                } else if (drawerLayout.isDrawerOpen(GravityCompat.START)) {
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
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enforceResizableWindow();
    }

    @SuppressWarnings("deprecation")
    private void enforceResizableWindow() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN |
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS |
                WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS |
                WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
    }

    @Override
    protected void onResume() {
        super.onResume();
        configureClient();
    }

    @Override
    protected void onDestroy() {
        if (visibleWindowLayoutListener != null && drawerLayout != null &&
                drawerLayout.getViewTreeObserver().isAlive()) {
            drawerLayout.getViewTreeObserver().removeOnGlobalLayoutListener(
                    visibleWindowLayoutListener);
        }
        if (client != null) {
            client.setRouteStatusListener(null);
            client.close();
        }
        super.onDestroy();
    }

    private void openScannerInSettings() {
        Intent intent = new Intent(this, SettingsActivity.class);
        intent.putExtra(SettingsActivity.EXTRA_OPEN_SCANNER, true);
        startActivity(intent);
    }

    private void showCreateProjectSheet() {
        if (client == null || client.baseUrl().isEmpty()) {
            Toast.makeText(this, "请先连接一台电脑", Toast.LENGTH_SHORT).show();
            return;
        }

        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        BottomSheetDialog dialog = new BottomSheetDialog(this);
        View content = getLayoutInflater().inflate(R.layout.dialog_create_project, null);
        dialog.setContentView(content);
        dialog.setOnShowListener(ignored -> {
            FrameLayout bottomSheet = dialog.findViewById(
                    com.google.android.material.R.id.design_bottom_sheet);
            if (bottomSheet == null) return;
            bottomSheet.setBackgroundColor(android.graphics.Color.TRANSPARENT);
            BottomSheetBehavior<FrameLayout> behavior = BottomSheetBehavior.from(bottomSheet);
            behavior.setSkipCollapsed(true);
            behavior.setState(BottomSheetBehavior.STATE_EXPANDED);
        });

        TextInputLayout nameLayout = content.findViewById(R.id.projectNameLayout);
        EditText nameInput = content.findViewById(R.id.projectNameInput);
        MaterialAutoCompleteTextView typeSelector =
                content.findViewById(R.id.projectTypeSelector);
        MaterialAutoCompleteTextView groupSelector =
                content.findViewById(R.id.projectGroupSelector);
        TextView pathPreview = content.findViewById(R.id.projectPathPreview);
        TextView status = content.findViewById(R.id.projectCreationStatus);
        MaterialButton cancel = content.findViewById(R.id.cancelCreateProjectButton);
        MaterialButton confirm = content.findViewById(R.id.confirmCreateProjectButton);
        final String[] rootDirectory = {""};
        final long[] selectedTypeId = {0L};
        final long[] selectedGroupId = {0L};

        cancel.setOnClickListener(view -> dialog.dismiss());
        nameInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start,
                    int count, int after) { }

            @Override public void onTextChanged(CharSequence value, int start,
                    int before, int count) {
                nameLayout.setError(null);
                updateProjectPathPreview(pathPreview, rootDirectory[0], value.toString());
            }

            @Override public void afterTextChanged(Editable value) { }
        });

        requestClient.loadProjectCreationOptions(
                new AicliClient.ProjectCreationOptionsCallback() {
                    @Override public void onSuccess(
                            AicliClient.ProjectCreationOptions options) {
                        runOnUiThread(() -> {
                            if (client != requestClient ||
                                    serverGeneration != requestGeneration ||
                                    !dialog.isShowing()) return;
                            rootDirectory[0] = options.rootDirectory;
                            updateProjectPathPreview(
                                    pathPreview, rootDirectory[0],
                                    nameInput.getText().toString());
                            if (options.types.isEmpty() || options.groups.isEmpty()) {
                                status.setText(options.types.isEmpty()
                                        ? "电脑端还没有可用的项目类型"
                                        : "电脑端还没有可用的项目分组");
                                return;
                            }

                            List<String> typeNames = new ArrayList<>();
                            int defaultTypeIndex = 0;
                            for (int index = 0; index < options.types.size(); index++) {
                                AicliClient.ProjectChoice choice = options.types.get(index);
                                typeNames.add(choice.name);
                                if (choice.id == options.defaultTypeId) defaultTypeIndex = index;
                            }
                            List<String> groupNames = new ArrayList<>();
                            int defaultGroupIndex = 0;
                            for (int index = 0; index < options.groups.size(); index++) {
                                AicliClient.ProjectChoice choice = options.groups.get(index);
                                groupNames.add(choice.name);
                                if (choice.id == options.defaultGroupId) defaultGroupIndex = index;
                            }
                            selectedTypeId[0] = options.types.get(defaultTypeIndex).id;
                            selectedGroupId[0] = options.groups.get(defaultGroupIndex).id;
                            typeSelector.setAdapter(new ArrayAdapter<>(
                                    MainActivity.this, R.layout.item_group_dropdown, typeNames));
                            groupSelector.setAdapter(new ArrayAdapter<>(
                                    MainActivity.this, R.layout.item_group_dropdown, groupNames));
                            typeSelector.setText(typeNames.get(defaultTypeIndex), false);
                            groupSelector.setText(groupNames.get(defaultGroupIndex), false);
                            typeSelector.setOnItemClickListener((parent, view, position, id) -> {
                                if (position >= 0 && position < options.types.size()) {
                                    selectedTypeId[0] = options.types.get(position).id;
                                }
                            });
                            groupSelector.setOnItemClickListener((parent, view, position, id) -> {
                                if (position >= 0 && position < options.groups.size()) {
                                    selectedGroupId[0] = options.groups.get(position).id;
                                }
                            });
                            status.setText("");
                            confirm.setEnabled(true);
                        });
                    }

                    @Override public void onError(String message) {
                        runOnUiThread(() -> {
                            if (!dialog.isShowing()) return;
                            status.setText(message);
                        });
                    }
                });

        confirm.setOnClickListener(view -> {
            String name = nameInput.getText().toString().trim();
            if (name.isEmpty()) {
                nameLayout.setError("请输入项目名称");
                nameInput.requestFocus();
                return;
            }
            if (selectedTypeId[0] <= 0L || selectedGroupId[0] <= 0L) {
                status.setText("项目类型或分组尚未加载完成");
                return;
            }
            confirm.setEnabled(false);
            cancel.setEnabled(false);
            nameInput.setEnabled(false);
            typeSelector.setEnabled(false);
            groupSelector.setEnabled(false);
            status.setTextColor(ContextCompat.getColor(
                    MainActivity.this, R.color.aicli_text_secondary));
            status.setText("正在电脑上创建项目…");
            requestClient.createProject(name, selectedTypeId[0], selectedGroupId[0],
                    new AicliClient.ProjectCreationCallback() {
                        @Override public void onSuccess(AicliClient.Conversation project) {
                            runOnUiThread(() -> {
                                if (client != requestClient ||
                                        serverGeneration != requestGeneration) return;
                                dialog.dismiss();
                                selectNewlyCreatedProject(project);
                                Toast.makeText(MainActivity.this,
                                        "项目已创建，尚未启动", Toast.LENGTH_SHORT).show();
                                loadConversations();
                            });
                        }

                        @Override public void onError(String message) {
                            runOnUiThread(() -> {
                                if (!dialog.isShowing()) {
                                    Toast.makeText(MainActivity.this, message,
                                            Toast.LENGTH_SHORT).show();
                                    return;
                                }
                                confirm.setEnabled(true);
                                cancel.setEnabled(true);
                                nameInput.setEnabled(true);
                                typeSelector.setEnabled(true);
                                groupSelector.setEnabled(true);
                                status.setTextColor(ContextCompat.getColor(
                                        MainActivity.this,
                                        R.color.aicli_connection_failed));
                                status.setText(message);
                            });
                        }
                    });
        });
        dialog.show();
    }

    private void updateProjectPathPreview(TextView view, String root, String name) {
        if (root == null || root.trim().isEmpty()) {
            view.setText("电脑端尚未配置手机项目目录");
            return;
        }
        String normalizedRoot = root.trim();
        String separator = normalizedRoot.endsWith("\\") || normalizedRoot.endsWith("/")
                ? "" : "\\";
        String leaf = name == null || name.trim().isEmpty()
                ? "项目名称" : name.trim();
        view.setText(normalizedRoot + separator + leaf);
    }

    private void selectNewlyCreatedProject(AicliClient.Conversation project) {
        if (project == null || project.id == null || project.id.isEmpty()) return;
        for (int index = allConversations.size() - 1; index >= 0; index--) {
            if (project.id.equals(allConversations.get(index).id)) {
                allConversations.remove(index);
            }
        }
        allConversations.add(project);
        cacheConversations(allConversations);
        selectedConversationId = null;
        selectedConversation = null;
        pendingConversationId = project.id;
        selectedGroup = project.group == null || project.group.isEmpty()
                ? "默认分组" : project.group;
        getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                .putString("selected_group", selectedGroup).apply();
        rebuildGroupSelector(allConversations);
        renderSelectedGroup(false);
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
            if (client != null) {
                client.setRouteStatusListener(null);
                client.close();
            }
            client = new AicliClient(this, "");
            client.setRouteStatusListener(this::handleRouteStatus);
            configuredConnectionKey = connectionKey;
            serverGeneration++;
            resetForComputerChange();
        }
        if (baseUrl.isEmpty()) {
            updateComputerTitle();
            renderRouteStatus(localConnectionIcon, localCurrentRouteIcon,
                    "本地", AicliClient.RouteHealth.UNAVAILABLE, false);
            renderRouteStatus(remoteConnectionIcon, remoteCurrentRouteIcon,
                    "远程", AicliClient.RouteHealth.UNAVAILABLE, false);
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
        if (allConversations.isEmpty()) restoreCachedConversations();
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
                    cacheConversations(conversations);
                    refreshComputerSelector(connectionStore.active());
                    updateComputerTitle();
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
                    if (!allConversations.isEmpty()) {
                        setConnectionStatus(computerName + " · 连接失败 · 显示上次项目列表");
                    } else {
                        setConnectionStatus(computerName + " · 连接失败");
                        showEmptyState("暂时无法连接电脑", "请确认电脑端在线且已授权此手机；也可以在设置中重新扫码。");
                    }
                    Toast.makeText(MainActivity.this, message, Toast.LENGTH_SHORT).show();
                });
            }
        });
    }

    private void restoreCachedConversations() {
        List<AicliClient.Conversation> cached = readCachedConversations();
        if (cached.isEmpty()) return;
        allConversations.clear();
        allConversations.addAll(cached);
        prepareLastConversationRestore(cached);
        rebuildGroupSelector(cached);
        setConnectionStatus(activeComputerName() + " · 正在连接 · 显示上次项目列表");
        renderSelectedGroup(false);
    }

    private void cacheConversations(List<AicliClient.Conversation> conversations) {
        JsonArray items = new JsonArray();
        for (AicliClient.Conversation conversation : conversations) {
            JsonObject value = new JsonObject();
            value.addProperty("id", conversation.id);
            value.addProperty("name", conversation.name);
            value.addProperty("provider", conversation.provider);
            value.addProperty("type", conversation.type);
            value.addProperty("group", conversation.group);
            value.addProperty("folder", conversation.folder);
            value.addProperty("status", conversation.status);
            items.add(value);
        }
        getSharedPreferences(CONVERSATION_CACHE_PREFS, MODE_PRIVATE).edit()
                .putString(conversationCacheKey(), items.toString()).apply();
    }

    private List<AicliClient.Conversation> readCachedConversations() {
        List<AicliClient.Conversation> result = new ArrayList<>();
        SharedPreferences preferences = getSharedPreferences(
                CONVERSATION_CACHE_PREFS, MODE_PRIVATE);
        String key = conversationCacheKey();
        String raw = preferences.getString(key, "");
        if (raw == null || raw.isEmpty()) return result;
        try {
            JsonElement parsed = JsonParser.parseString(raw);
            if (!parsed.isJsonArray()) throw new IllegalStateException("cache is not an array");
            for (JsonElement item : parsed.getAsJsonArray()) {
                if (!item.isJsonObject()) continue;
                JsonObject value = item.getAsJsonObject();
                String id = stringValue(value, "id");
                if (id.isEmpty()) continue;
                result.add(new AicliClient.Conversation(id,
                        stringValue(value, "name"),
                        stringValue(value, "provider"),
                        stringValue(value, "type"),
                        stringValue(value, "group"),
                        stringValue(value, "folder"),
                        stringValue(value, "status")));
            }
        } catch (RuntimeException error) {
            preferences.edit().remove(key).apply();
            android.util.Log.w("AicliCache", "invalid conversation cache", error);
            result.clear();
        }
        return result;
    }

    private String conversationCacheKey() {
        ComputerConnectionStore.Computer active = connectionStore == null
                ? null : connectionStore.active();
        String computerId = active == null ? "" : active.computerId;
        if (computerId == null || computerId.isEmpty()) {
            SharedPreferences preferences = getSharedPreferences(
                    CONNECTION_PREFS, MODE_PRIVATE);
            computerId = preferences.getString("computerId", "");
            if (computerId == null || computerId.isEmpty()) {
                String address = preferences.getString(BASE_URL_KEY, "");
                computerId = "legacy-" + Integer.toHexString(
                        address == null ? 0 : address.hashCode());
            }
        }
        return CONVERSATION_CACHE_PREFIX + computerId;
    }

    private static boolean isRunning(AicliClient.Conversation conversation) {
        return conversation != null &&
                "running".equalsIgnoreCase(conversation.status);
    }

    private void rebuildGroupSelector(List<AicliClient.Conversation> conversations) {
        Set<String> unique = new LinkedHashSet<>();
        for (AicliClient.Conversation conversation : conversations) {
            unique.add(conversation.group.isEmpty() ? "默认分组" : conversation.group);
        }
        List<String> next = new ArrayList<>();
        next.add(ALL_GROUPS);
        next.add(RUNNING_GROUP);
        next.addAll(unique);
        displayedGroups.clear();
        displayedGroups.addAll(next);
        if (!displayedGroups.contains(selectedGroup)) selectedGroup = ALL_GROUPS;

        rebuildingGroups = true;
        List<String> labels = new ArrayList<>();
        String selectedLabel = "全部";
        for (int index = 0; index < displayedGroups.size(); index++) {
            String group = displayedGroups.get(index);
            int count = 0;
            for (AicliClient.Conversation conversation : conversations) {
                String itemGroup = conversation.group.isEmpty() ? "默认分组" : conversation.group;
                if (ALL_GROUPS.equals(group) ||
                        (RUNNING_GROUP.equals(group) && isRunning(conversation)) ||
                        group.equals(itemGroup)) count++;
            }
            String label = ALL_GROUPS.equals(group) ? "全部"
                    : RUNNING_GROUP.equals(group) ? "正在运行" : group;
            labels.add(label + " (" + count + ")");
            if (group.equals(selectedGroup)) {
                selectedLabel = label;
            }
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, R.layout.item_group_dropdown, labels);
        groupSelector.setAdapter(adapter);
        if (!labels.isEmpty()) groupSelector.setText(selectedLabel, false);
        groupSelectorLayout.setVisibility(View.VISIBLE);
        rebuildingGroups = false;
    }

    private void renderSelectedGroup(boolean selectFirst) {
        renderSelectedGroup(selectFirst, true);
    }

    private void renderSelectedGroup(boolean selectFirst, boolean closeDrawerOnSelection) {
        List<AicliClient.Conversation> groupItems = new ArrayList<>();
        for (AicliClient.Conversation conversation : allConversations) {
            String group = conversation.group.isEmpty() ? "默认分组" : conversation.group;
            if (ALL_GROUPS.equals(selectedGroup) ||
                    (RUNNING_GROUP.equals(selectedGroup) && isRunning(conversation)) ||
                    selectedGroup.equals(group)) groupItems.add(conversation);
        }
        List<AicliClient.Conversation> filtered = new ArrayList<>();
        for (AicliClient.Conversation conversation : groupItems) {
            if (matchesConversationSearch(conversation)) filtered.add(conversation);
        }
        conversationAdapter.replace(filtered);
        String selectedGroupLabel = ALL_GROUPS.equals(selectedGroup) ? "全部分组"
                : RUNNING_GROUP.equals(selectedGroup) ? "正在运行" : selectedGroup;
        boolean hasSearch = !conversationSearchQuery.isEmpty();
        drawerGroupLabel.setText(hasSearch
                ? selectedGroupLabel + " · 找到 " + filtered.size() + " 个项目"
                : selectedGroupLabel + " · " + filtered.size() + " 个项目");
        boolean searchHasNoMatches = hasSearch && !groupItems.isEmpty() && filtered.isEmpty();
        conversationSearchEmpty.setVisibility(searchHasNoMatches ? View.VISIBLE : View.GONE);
        conversationList.setVisibility(searchHasNoMatches ? View.GONE : View.VISIBLE);
        if (searchHasNoMatches) {
            conversationAdapter.select(selectedConversationId);
            return;
        }
        if (groupItems.isEmpty()) {
            pendingConversationId = null;
            selectedConversationId = null;
            selectedConversation = null;
            conversationAdapter.select(null);
            hideConversation();
            showEmptyState("这个分组还没有项目", "点击项目列表右上角的 +，即可在当前电脑上创建项目。");
            return;
        }
        if (hasSearch) {
            conversationAdapter.select(selectedConversationId);
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
            selectConversation(selected, closeDrawerOnSelection);
        }
        else {
            selectedConversation = selected;
            conversationAdapter.select(selected.id);
            updateProjectControls(selected);
            hideEmptyState();
        }
    }

    private boolean matchesConversationSearch(AicliClient.Conversation conversation) {
        if (conversationSearchQuery.isEmpty()) return true;
        String query = conversationSearchQuery.toLowerCase(Locale.ROOT);
        return containsSearchText(conversation.name, query)
                || containsSearchText(conversation.folder, query)
                || containsSearchText(conversation.type, query)
                || containsSearchText(conversation.provider, query)
                || containsSearchText(conversation.group, query);
    }

    private static boolean containsSearchText(String value, String query) {
        return value != null && value.toLowerCase(Locale.ROOT).contains(query);
    }

    private void selectConversation(AicliClient.Conversation conversation) {
        selectConversation(conversation, true);
    }

    private void selectConversation(AicliClient.Conversation conversation,
                                    boolean closeDrawerOnSelection) {
        if (client == null) return;
        client.closeEvents();
        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        selectedConversationId = conversation.id;
        selectedConversation = conversation;
        rememberSelectedConversation(conversation.id);
        updateProjectHeader(conversation);
        conversationAdapter.select(conversation.id);
        if (closeDrawerOnSelection) {
            drawerLayout.closeDrawer(GravityCompat.START);
        }
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

    private void handleComposerAction() {
        if (messageInput.getText().length() > 0) {
            sendInput();
            return;
        }
        if (terminalKeyPanelVisible) {
            hideTerminalKeyPanel(true);
        } else {
            showTerminalKeyPanel();
        }
    }

    private void registerImageLaunchers() {
        galleryLauncher = registerForActivityResult(
                new ActivityResultContracts.GetContent(), uri -> {
                    if (uri != null) importImage(uri);
                });
        cameraLauncher = registerForActivityResult(
                new ActivityResultContracts.TakePicture(), saved -> {
                    if (saved && pendingCameraUri != null) importImage(pendingCameraUri);
                    else deletePendingCameraFile();
                });
        cameraPermissionLauncher = registerForActivityResult(
                new ActivityResultContracts.RequestPermission(), granted -> {
                    if (granted) launchCameraCapture();
                    else Toast.makeText(this, "需要相机权限才能拍照", Toast.LENGTH_SHORT).show();
                });
    }

    private void openCamera() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED) {
            launchCameraCapture();
        } else {
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA);
        }
    }

    private void launchCameraCapture() {
        try {
            File directory = new File(getCacheDir(), "camera");
            if (!directory.exists() && !directory.mkdirs()) {
                throw new IOException("无法创建拍照缓存目录");
            }
            pendingCameraFile = File.createTempFile("aicli-camera-", ".jpg", directory);
            pendingCameraUri = FileProvider.getUriForFile(
                    this, getPackageName() + ".fileprovider", pendingCameraFile);
            cameraLauncher.launch(pendingCameraUri);
        } catch (Exception error) {
            deletePendingCameraFile();
            Toast.makeText(this, "无法打开相机：" + error.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void deletePendingCameraFile() {
        if (pendingCameraFile != null && pendingCameraFile.exists()) {
            //noinspection ResultOfMethodCallIgnored
            pendingCameraFile.delete();
        }
        pendingCameraFile = null;
        pendingCameraUri = null;
    }

    private void importImage(Uri uri) {
        Toast.makeText(this, "正在处理图片…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                ContentResolver resolver = getContentResolver();
                Bitmap bitmap;
                if (android.os.Build.VERSION.SDK_INT >= 28) {
                    bitmap = ImageDecoder.decodeBitmap(
                            ImageDecoder.createSource(resolver, uri),
                            (decoder, info, source) -> {
                                decoder.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
                                int width = info.getSize().getWidth();
                                int height = info.getSize().getHeight();
                                int largest = Math.max(width, height);
                                if (largest > 2048) {
                                    float scale = 2048f / largest;
                                    decoder.setTargetSize(
                                            Math.max(1, Math.round(width * scale)),
                                            Math.max(1, Math.round(height * scale)));
                                }
                            });
                } else {
                    BitmapFactory.Options bounds = new BitmapFactory.Options();
                    bounds.inJustDecodeBounds = true;
                    try (InputStream input = resolver.openInputStream(uri)) {
                        BitmapFactory.decodeStream(input, null, bounds);
                    }
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    int largest = Math.max(bounds.outWidth, bounds.outHeight);
                    while (largest / options.inSampleSize > 2048) options.inSampleSize *= 2;
                    try (InputStream input = resolver.openInputStream(uri)) {
                        bitmap = BitmapFactory.decodeStream(input, null, options);
                    }
                }
                if (bitmap == null) throw new IOException("无法读取所选图片");
                uploadBitmap(bitmap);
            } catch (Exception error) {
                runOnUiThread(() -> Toast.makeText(this,
                        "图片读取失败：" + error.getMessage(), Toast.LENGTH_LONG).show());
            } finally {
                if (pendingCameraUri != null && pendingCameraUri.equals(uri)) {
                    deletePendingCameraFile();
                }
            }
        }, "aicli-image-import").start();
    }

    private void uploadBitmap(Bitmap original) {
        if (selectedConversationId == null || client == null) return;
        new Thread(() -> {
            Bitmap prepared = original;
            try {
                final int maximumDimension = 2048;
                int width = original.getWidth();
                int height = original.getHeight();
                int largest = Math.max(width, height);
                if (largest > maximumDimension) {
                    float scale = maximumDimension / (float) largest;
                    prepared = Bitmap.createScaledBitmap(original,
                            Math.max(1, Math.round(width * scale)),
                            Math.max(1, Math.round(height * scale)), true);
                }
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                int quality = 88;
                prepared.compress(Bitmap.CompressFormat.JPEG, quality, output);
                while (output.size() > 5 * 1024 * 1024 && quality > 55) {
                    output.reset();
                    quality -= 8;
                    prepared.compress(Bitmap.CompressFormat.JPEG, quality, output);
                }
                if (output.size() > 5 * 1024 * 1024) throw new IOException("图片压缩后仍超过 5MB");
                byte[] bytes = output.toByteArray();
                String conversationId = selectedConversationId;
                runOnUiThread(() -> Toast.makeText(this,
                        "正在上传图片…", Toast.LENGTH_SHORT).show());
                client.uploadImage(conversationId, bytes, "image/jpeg",
                        new AicliClient.AttachmentCallback() {
                    @Override public void onSuccess(String path) {
                        runOnUiThread(() -> insertUploadedImagePath(path));
                    }
                    @Override public void onError(String message) {
                        runOnUiThread(() -> Toast.makeText(MainActivity.this,
                                "图片上传失败：" + message, Toast.LENGTH_LONG).show());
                    }
                });
            } catch (Exception error) {
                runOnUiThread(() -> Toast.makeText(this,
                        "图片处理失败：" + error.getMessage(), Toast.LENGTH_LONG).show());
            } finally {
                if (!prepared.isRecycled()) prepared.recycle();
                if (prepared != original && !original.isRecycled()) original.recycle();
            }
        }, "aicli-image-upload").start();
    }

    private void insertUploadedImagePath(String path) {
        if (path == null || path.isEmpty()) {
            Toast.makeText(this, "电脑没有返回图片路径", Toast.LENGTH_LONG).show();
            return;
        }
        String current = messageInput.getText().toString();
        String reference = '"' + path + '"';
        if (!current.isEmpty() && !Character.isWhitespace(current.charAt(current.length() - 1))) {
            current += " ";
        }
        messageInput.setText(current + reference + " ");
        messageInput.setSelection(messageInput.length());
        hideTerminalKeyPanel(true);
        Toast.makeText(this, "图片已上传，可补充问题后发送", Toast.LENGTH_SHORT).show();
    }

    private void showTerminalKeyPanel() {
        if (selectedConversation == null ||
                !"running".equalsIgnoreCase(selectedConversation.status)) return;
        int availableHeight = drawerLayout.getHeight() - dpToPx(180);
        int panelHeight = lastImeHeightPx;
        if (panelHeight < dpToPx(160)) panelHeight = dpToPx(260);
        if (availableHeight > 0) panelHeight = Math.min(panelHeight, availableHeight);
        android.view.ViewGroup.LayoutParams params = terminalKeyBar.getLayoutParams();
        if (params.height != panelHeight) {
            params.height = panelHeight;
            terminalKeyBar.setLayoutParams(params);
        }
        terminalKeyPanelVisible = true;
        messageInput.clearFocus();
        setConversationKeyboardInset(0);
        terminalKeyBar.setVisibility(View.VISIBLE);
        terminalKeyBar.setAlpha(1f);
        if (imeVisible) {
            hideSoftKeyboard();
        }
        terminalView.post(terminalView::scrollToBottom);
        updateComposerAction();
    }

    private void hideTerminalKeyPanel(boolean reopenKeyboard) {
        if (reopenKeyboard) {
            switchPanelToKeyboard();
            return;
        }
        terminalKeyPanelVisible = false;
        terminalKeyBar.setAlpha(1f);
        terminalKeyBar.setVisibility(View.GONE);
        setConversationKeyboardInset(0);
        updateComposerAction();
    }

    private void switchPanelToKeyboard() {
        terminalKeyPanelVisible = false;
        messageInput.requestFocus();
        updateComposerAction();
        showSoftKeyboard();
        terminalKeyBar.postDelayed(() -> {
            if (!terminalKeyPanelVisible && !imeVisible) {
                terminalKeyBar.setVisibility(View.GONE);
                terminalKeyBar.setAlpha(1f);
                terminalView.post(terminalView::scrollToBottom);
            }
        }, 400L);
    }

    private void showSoftKeyboard() {
        messageInput.requestFocus();
        WindowInsetsControllerCompat controller =
                ViewCompat.getWindowInsetsController(messageInput);
        if (controller != null) {
            controller.show(WindowInsetsCompat.Type.ime());
            return;
        }
        InputMethodManager manager =
                (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (manager != null) manager.showSoftInput(messageInput, InputMethodManager.SHOW_IMPLICIT);
    }

    private void hideSoftKeyboard() {
        WindowInsetsControllerCompat controller =
                ViewCompat.getWindowInsetsController(messageInput);
        if (controller != null) {
            controller.hide(WindowInsetsCompat.Type.ime());
            return;
        }
        InputMethodManager manager =
                (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (manager != null) manager.hideSoftInputFromWindow(messageInput.getWindowToken(), 0);
    }

    private void observeImeHeight() {
        ViewCompat.setOnApplyWindowInsetsListener(drawerLayout, (view, insets) -> {
            int statusBarInset = insets.getInsets(
                    WindowInsetsCompat.Type.statusBars()).top;
            applyHeaderTopInset(statusBarInset);
            applyDrawerTopInset(statusBarInset);
            systemBottomInsetPx = insets.getInsets(
                    WindowInsetsCompat.Type.navigationBars()).bottom;
            int imeBottom = insets.getInsets(WindowInsetsCompat.Type.ime()).bottom;
            if (insets.isVisible(WindowInsetsCompat.Type.ime()) && imeBottom > 0) {
                rememberImeHeight(Math.max(0, imeBottom - systemBottomInsetPx));
            }
            return insets;
        });
        visibleWindowLayoutListener = this::updateVisibleWindowLayout;
        drawerLayout.getViewTreeObserver().addOnGlobalLayoutListener(
                visibleWindowLayoutListener);
        ViewCompat.requestApplyInsets(drawerLayout);
    }

    private void updateVisibleWindowLayout() {
        if (drawerLayout == null || conversationContent == null ||
                !drawerLayout.isAttachedToWindow()) return;
        View root = drawerLayout.getRootView();
        root.getWindowVisibleDisplayFrame(visibleWindowFrame);
        int[] rootLocation = new int[2];
        int[] contentLocation = new int[2];
        root.getLocationOnScreen(rootLocation);
        conversationContent.getLocationOnScreen(contentLocation);
        int statusBarInset = Math.max(0, visibleWindowFrame.top - rootLocation[1]);
        applyHeaderTopInset(statusBarInset);
        applyDrawerTopInset(statusBarInset);
        int rootBottom = rootLocation[1] + root.getHeight();
        int coveredHeight = Math.max(0, rootBottom - visibleWindowFrame.bottom);
        boolean keyboardVisible = coveredHeight >= dpToPx(120);
        imeVisible = keyboardVisible;
        if (!keyboardVisible) {
            if (!terminalKeyPanelVisible) setConversationKeyboardInset(0);
            return;
        }

        rememberImeHeight(Math.max(0, coveredHeight - systemBottomInsetPx));
        if (!terminalKeyPanelVisible && terminalKeyBar.getVisibility() == View.VISIBLE) {
            terminalKeyBar.setVisibility(View.GONE);
            terminalKeyBar.setAlpha(1f);
        }
        if (!terminalKeyPanelVisible) {
            int contentBottom = contentLocation[1] + conversationContent.getHeight();
            int keyboardOverlap = Math.max(0, contentBottom - visibleWindowFrame.bottom);
            setConversationKeyboardInset(keyboardOverlap);
            terminalView.post(terminalView::scrollToBottom);
        }
    }

    private void setConversationKeyboardInset(int inset) {
        int safeInset = Math.max(0, inset);
        if (conversationKeyboardInsetPx == safeInset) return;
        conversationKeyboardInsetPx = safeInset;
        conversationContent.setPadding(conversationContent.getPaddingLeft(),
                conversationContent.getPaddingTop(), conversationContent.getPaddingRight(),
                conversationBasePaddingBottom + safeInset);
    }

    private void applyHeaderTopInset(int inset) {
        if (mainHeader == null) return;
        int safeInset = Math.max(0, inset);
        if (headerTopInsetPx == safeInset) return;
        headerTopInsetPx = safeInset;
        mainHeader.setPadding(headerBasePaddingLeft, safeInset,
                headerBasePaddingRight, headerBasePaddingBottom);
    }

    private void applyDrawerTopInset(int inset) {
        if (conversationDrawer == null) return;
        int safeInset = Math.max(0, inset);
        if (drawerTopInsetPx == safeInset) return;
        drawerTopInsetPx = safeInset;
        conversationDrawer.setPadding(drawerBasePaddingLeft,
                drawerBasePaddingTop + safeInset,
                drawerBasePaddingRight, drawerBasePaddingBottom);
    }

    private void rememberImeHeight(int height) {
        if (height < dpToPx(160) || height == lastImeHeightPx) return;
        lastImeHeightPx = height;
        getSharedPreferences(APP_PREFS, MODE_PRIVATE).edit()
                .putInt(LAST_IME_HEIGHT_KEY, height).apply();
    }

    private void updateComposerAction() {
        if (sendButton == null || messageInput == null) return;
        boolean hasInput = messageInput.getText().length() > 0;
        int color;
        if (hasInput) {
            sendButton.setIconResource(R.drawable.ic_send);
            sendButton.setContentDescription("发送");
            color = ContextCompat.getColor(this, R.color.aicli_primary);
            sendButton.setStrokeWidth(0);
            sendButton.setIconTint(ColorStateList.valueOf(
                    ContextCompat.getColor(this, R.color.aicli_text)));
        } else {
            sendButton.setIconResource(terminalKeyPanelVisible
                    ? R.drawable.ic_panel_return : R.drawable.ic_add);
            sendButton.setContentDescription(terminalKeyPanelVisible
                    ? "返回输入键盘" : "展开终端按键");
            color = ContextCompat.getColor(this, R.color.aicli_surface_high);
            sendButton.setStrokeWidth(dpToPx(1));
            sendButton.setStrokeColor(ColorStateList.valueOf(
                    ContextCompat.getColor(this, R.color.aicli_outline_strong)));
            sendButton.setIconTint(ColorStateList.valueOf(
                    ContextCompat.getColor(this, R.color.aicli_text_secondary)));
        }
        sendButton.setBackgroundTintList(ColorStateList.valueOf(color));
    }

    private int dpToPx(int dp) {
        return Math.round(dp * getResources().getDisplayMetrics().density);
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

    private void sendTerminalInterrupt() {
        if (selectedConversationId == null || client == null) return;
        client.control(selectedConversationId, "ctrl_c", new AicliClient.ActionCallback() {
            @Override public void onSuccess() { }
            @Override public void onError(String message) {
                runOnUiThread(() -> Toast.makeText(
                        MainActivity.this, "终止信号发送失败：" + message,
                        Toast.LENGTH_SHORT).show());
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

    private void confirmCloseSelectedProject() {
        if (selectedConversation == null || selectedConversationId == null || client == null ||
                !"running".equalsIgnoreCase(selectedConversation.status)) return;
        String projectName = selectedConversation.name.isEmpty()
                ? "当前项目" : selectedConversation.name;
        new MaterialAlertDialogBuilder(this)
                .setTitle("关闭项目")
                .setMessage("确定关闭“" + projectName + "”的终端会话吗？\n\n项目不会被删除，之后仍可重新开始。")
                .setNegativeButton("取消", null)
                .setPositiveButton("关闭", (dialog, which) -> closeSelectedProject())
                .show();
    }

    private void closeSelectedProject() {
        if (selectedConversationId == null || client == null) return;
        String conversationId = selectedConversationId;
        closeProjectButton.setEnabled(false);
        client.control(conversationId, "cancel", new AicliClient.ActionCallback() {
            @Override public void onSuccess() {
                runOnUiThread(() -> {
                    if (!conversationId.equals(selectedConversationId)) return;
                    terminalKeyPanelVisible = false;
                    terminalKeyBar.setVisibility(View.GONE);
                    inputBar.setVisibility(View.GONE);
                    closeProjectButton.setVisibility(View.GONE);
                    closeProjectButton.setEnabled(true);
                    Toast.makeText(MainActivity.this,
                            "项目已关闭，可随时重新开始", Toast.LENGTH_SHORT).show();
                    loadConversations();
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> {
                    closeProjectButton.setEnabled(true);
                    Toast.makeText(MainActivity.this,
                            "关闭项目失败：" + message, Toast.LENGTH_SHORT).show();
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
        if ("terminal.bootstrap".equals(type) && event != null && payload != null) {
            String sessionId = stringValue(event, "terminalSessionId");
            long sequence = longValue(event, "sequence", 0L);
            List<String> lines = new ArrayList<>();
            JsonArray values = payload.has("lines") && payload.get("lines").isJsonArray()
                    ? payload.getAsJsonArray("lines") : new JsonArray();
            for (JsonElement value : values) {
                lines.add(value == null || value.isJsonNull() ? "" : value.getAsString());
            }
            TerminalTextRenderer.StreamResult result = terminalRenderer.bootstrap(
                    sessionId, sequence, intValue(payload, "rows", 32),
                    intValue(payload, "columns", 120), lines,
                    stringValue(payload, "historyToken"),
                    intValue(payload, "startLine", 0),
                    payload.has("hasMore") && payload.get("hasMore").getAsBoolean());
            if (result == TerminalTextRenderer.StreamResult.GAP) requestTerminalReplay();
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
            @Override public void onOpen() {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration ||
                            !conversationId.equals(selectedConversationId)) return;
                    terminalView.setVisibility(View.VISIBLE);
                    terminalRenderer.restoreSelectedSession(conversationId);
                    conversationAdapter.select(conversationId);
                    updateProjectHeader(selectedConversation);
                    hideEmptyState();
                    updateProjectControls(selectedConversation);
                    setConnectionStatus("实时连接在线");
                    recoverMissedEvents();
                    loadConversations();
                });
            }

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

    private void loadOlderTerminalHistory() {
        if (client == null) return;
        TerminalTextRenderer.HistoryRequest request = terminalRenderer.beginHistoryRequest();
        if (request == null) return;
        final AicliClient requestClient = client;
        final long requestGeneration = serverGeneration;
        requestClient.loadTerminalHistory(request.projectId, request.historyToken,
                request.beforeLine, 200, new AicliClient.TerminalHistoryCallback() {
            @Override public void onSuccess(AicliClient.TerminalHistoryPage page) {
                runOnUiThread(() -> {
                    if (client != requestClient || serverGeneration != requestGeneration) {
                        terminalRenderer.historyFailed(request.projectId,
                                request.historyToken);
                        return;
                    }
                    String token = page.historyToken.isEmpty()
                            ? request.historyToken : page.historyToken;
                    terminalRenderer.historyLoaded(request.projectId, token,
                            page.startLine, page.lines, page.hasMore, page.expired);
                });
            }

            @Override public void onError(String message) {
                runOnUiThread(() -> terminalRenderer.historyFailed(
                        request.projectId, request.historyToken));
            }
        });
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

    private void handleRouteStatus(AicliClient.RouteHealth local,
                                   AicliClient.RouteHealth remote,
                                   boolean usingLocal, boolean usingRemote) {
        runOnUiThread(() -> {
            renderRouteStatus(localConnectionIcon, localCurrentRouteIcon,
                    "本地", local, usingLocal);
            renderRouteStatus(remoteConnectionIcon, remoteCurrentRouteIcon,
                    "远程", remote, usingRemote);
        });
    }

    private void renderRouteStatus(ImageView icon, ImageView currentRouteIcon, String name,
                                   AicliClient.RouteHealth health,
                                   boolean activeRoute) {
        if (icon == null || currentRouteIcon == null) return;
        icon.clearAnimation();
        int iconColor;
        String state;
        if (health == AicliClient.RouteHealth.ONLINE) {
            iconColor = getColor(R.color.aicli_running);
            state = "已连接";
        } else if (health == AicliClient.RouteHealth.CONNECTING) {
            iconColor = getColor(R.color.aicli_connection_connecting);
            state = "连接中";
            AlphaAnimation pulse = new AlphaAnimation(0.28f, 1f);
            pulse.setDuration(520L);
            pulse.setRepeatMode(Animation.REVERSE);
            pulse.setRepeatCount(Animation.INFINITE);
            icon.startAnimation(pulse);
        } else if (health == AicliClient.RouteHealth.OFFLINE) {
            iconColor = getColor(R.color.aicli_connection_failed);
            state = "连接失败";
        } else {
            iconColor = getColor(R.color.aicli_text_tertiary);
            state = "未连接";
        }
        icon.setImageTintList(android.content.res.ColorStateList.valueOf(iconColor));
        icon.setContentDescription(name + "，" + state);
        // The route marker identifies the address currently selected by the
        // client, including its connecting/failure state. Do not wait for a
        // later health probe before showing which route is actually in use.
        boolean current = activeRoute;
        currentRouteIcon.setVisibility(current ? View.VISIBLE : View.GONE);
        currentRouteIcon.setContentDescription(current ? name + "，当前连接" : null);
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
        updateComputerTitle();
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
        conversationSearchQuery = "";
        if (conversationSearchInput != null) conversationSearchInput.setText("");
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
        if (active == null) return "电脑";
        return active.displayName();
    }

    private void updateComputerTitle() {
        if (currentComputerTitle != null) currentComputerTitle.setText(
                connectionStore == null || connectionStore.active() == null
                        ? "未绑定电脑" : activeComputerName());
    }

    private static String computerLabel(ComputerConnectionStore.Computer computer) {
        return computer.displayName();
    }

    private void updateProjectHeader(AicliClient.Conversation conversation) {
        updateComputerTitle();
        if (conversation == null) {
            currentProjectMeta.setText("未选择项目 · 未设置类型");
            return;
        }
        String type = conversation.type.isEmpty() ? conversation.provider : conversation.type;
        String project = conversation.name.isEmpty() ? "未命名项目" : conversation.name;
        currentProjectMeta.setText(project + " · " + (type.isEmpty() ? "未设置类型" : type));
    }

    private void updateProjectControls(AicliClient.Conversation conversation) {
        boolean running = conversation != null &&
                "running".equalsIgnoreCase(conversation.status);
        if (!running) terminalKeyPanelVisible = false;
        terminalKeyBar.setVisibility(running && terminalKeyPanelVisible
                ? View.VISIBLE : View.GONE);
        inputBar.setVisibility(running ? View.VISIBLE : View.GONE);
        closeProjectButton.setVisibility(running ? View.VISIBLE : View.GONE);
        closeProjectButton.setEnabled(true);
        updateComposerAction();
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
        terminalKeyPanelVisible = false;
        terminalKeyBar.setVisibility(View.GONE);
        closeProjectButton.setVisibility(View.GONE);
        closeProjectButton.setEnabled(true);
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
