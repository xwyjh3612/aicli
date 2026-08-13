package com.aitest.aicli;

import android.content.Context;
import android.content.SharedPreferences;

import androidx.annotation.NonNull;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.io.IOException;
import java.net.URI;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;

/**
 * Authenticated desktop client. Every post-pairing request uses TLS, a short
 * bearer capability token, and an Android Keystore ECDSA signature with a
 * timestamp, request id and monotonic sequence number.
 */
public final class AicliClient {
    private static final MediaType JSON = MediaType.get("application/json; charset=utf-8");
    private static final String PREFS = "connection";
    private static final String PHONE_PREFS = "device_identity";
    private static final String PHONE_NAME_KEY = "deviceName";
    private static final Object SEQUENCE_LOCK = new Object();

    public static final class Conversation {
        public final String id;
        public final String name;
        public final String provider;
        public final String type;
        public final String group;
        public final String folder;
        public final String status;

        Conversation(String id, String name, String provider, String type,
                     String group, String folder, String status) {
            this.id = id;
            this.name = name;
            this.provider = provider;
            this.type = type;
            this.group = group;
            this.folder = folder;
            this.status = status;
        }
    }

    public static final class TerminalHistoryPage {
        public final String historyToken;
        public final List<String> lines;
        public final int startLine;
        public final int totalLines;
        public final boolean hasMore;
        public final boolean expired;

        TerminalHistoryPage(String historyToken, List<String> lines,
                            int startLine, int totalLines,
                            boolean hasMore, boolean expired) {
            this.historyToken = historyToken;
            this.lines = lines;
            this.startLine = startLine;
            this.totalLines = totalLines;
            this.hasMore = hasMore;
            this.expired = expired;
        }
    }

    public interface ConversationCallback { void onSuccess(List<Conversation> conversations); void onError(String message); }
    public interface ActionCallback { void onSuccess(); void onError(String message); }
    public interface AttachmentCallback { void onSuccess(String path); void onError(String message); }
    public interface EventCallback {
        default void onOpen() { }
        void onEvent(String eventJson);
        void onClosed();
        default String terminalSessionId() { return ""; }
        default long terminalSequence() { return 0L; }
    }
    public interface HistoryCallback { void onSuccess(List<String> events); void onError(String message); }
    public interface TerminalHistoryCallback {
        void onSuccess(TerminalHistoryPage page);
        void onError(String message);
    }

    public enum RouteHealth {
        UNAVAILABLE,
        CONNECTING,
        ONLINE,
        OFFLINE,
    }

    public interface RouteStatusListener {
        void onRouteStatus(RouteHealth local, RouteHealth remote,
                           boolean usingLocal, boolean usingRemote);
    }

    public interface AvailabilityCallback {
        void onResult(boolean online);
    }

    public interface PairingCallback {
        void onStatus(String status, String message);
        void onComplete();
        void onError(String message);
    }

    private final Gson gson = new Gson();
    private final DeviceKeyManager keyManager = new DeviceKeyManager();
    private final ScheduledExecutorService executor = Executors.newScheduledThreadPool(3);
    private final ExecutorService routeProbeExecutor = Executors.newFixedThreadPool(2);
    private final Object authLock = new Object();
    private final Object routeLock = new Object();
    private final Object routeRecoveryLock = new Object();
    private final SharedPreferences preferences;
    private final SharedPreferences identityPreferences;
    private final ComputerConnectionStore connectionStore;
    private final String phoneId;
    private volatile OkHttpClient http;
    private volatile String baseUrl = "";
    private volatile String localUrl = "";
    private volatile String remoteUrl = "";
    private volatile String routePreference = ComputerConnectionStore.ROUTE_LOCAL;
    private volatile String certificateFingerprint = "";
    private volatile String computerId = "";
    private volatile String token = "";
    private volatile long tokenExpiresAt;
    private volatile String transportError = "";
    private long sequence;
    private volatile boolean persistConnectionState = true;
    private volatile WebSocket eventsSocket;
    private volatile EventCallback eventsCallback;
    private volatile String eventsConversationId = "";
    private final AtomicLong eventsGeneration = new AtomicLong();
    private ScheduledFuture<?> reconnectTask;
    private ScheduledFuture<?> routeStatusProbeTask;
    private volatile RouteStatusListener routeStatusListener;
    private volatile RouteHealth localRouteHealth = RouteHealth.UNAVAILABLE;
    private volatile RouteHealth remoteRouteHealth = RouteHealth.UNAVAILABLE;
    private volatile boolean closed;
    private volatile boolean clientClosed;
    private final boolean monitorRoutes;

    public AicliClient(Context context, String baseUrl) {
        this(context, baseUrl, null, true);
    }

    AicliClient(Context context, ComputerConnectionStore.Computer computer) {
        this(context, "", computer, true);
    }

    AicliClient(Context context, ComputerConnectionStore.Computer computer,
                boolean monitorRoutes) {
        this(context, "", computer, monitorRoutes);
    }

    private AicliClient(Context context, String requestedBaseUrl,
                        ComputerConnectionStore.Computer requestedComputer,
                        boolean monitorRoutes) {
        this.monitorRoutes = monitorRoutes;
        preferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        connectionStore = new ComputerConnectionStore(context);
        identityPreferences = context.getSharedPreferences(PHONE_PREFS, Context.MODE_PRIVATE);
        try {
            phoneId = keyManager.getOrCreatePhoneId(identityPreferences);
            keyManager.getOrCreateKeyPair();
        } catch (Exception exception) {
            throw new IllegalStateException("设备密钥初始化失败", exception);
        }
        sequence = preferences.getLong("sequence", 0L);
        ComputerConnectionStore.Computer active = requestedComputer == null
                ? connectionStore.active() : requestedComputer;
        persistConnectionState = requestedComputer == null;
        String savedBase = active == null ? preferences.getString("baseUrl", "") : active.baseUrl;
        this.localUrl = normalize(active == null
                ? preferences.getString("localAddress", "") : active.localUrl);
        this.remoteUrl = normalize(active == null
                ? preferences.getString("remoteAddress", "") : active.remoteUrl);
        this.routePreference = ComputerConnectionStore.normalizeRoutePreference(active == null
                ? preferences.getString("routePreference", ComputerConnectionStore.ROUTE_LOCAL)
                : active.routePreference);
        String requestedAddress = normalize(requestedBaseUrl == null || requestedBaseUrl.trim().isEmpty()
                ? savedBase : requestedBaseUrl);
        this.baseUrl = startingAddress(this.localUrl, this.remoteUrl,
                requestedAddress, this.routePreference);
        this.certificateFingerprint = normalizeFingerprint(active == null
                ? preferences.getString("certificateSha256", "")
                : active.certificateFingerprint);
        this.computerId = active == null
                ? preferences.getString("computerId", "") : active.computerId;
        rebuildHttpClient();
        if (monitorRoutes) {
            updateRouteStatusProbe();
        }
    }

    /** Compatibility constructor used only by old callers; it has no identity until configured. */
    public AicliClient(String baseUrl) {
        preferences = null;
        identityPreferences = null;
        connectionStore = null;
        phoneId = "";
        monitorRoutes = false;
        this.baseUrl = normalize(baseUrl);
        rebuildHttpClient();
    }

    public String phoneId() { return phoneId; }
    public String baseUrl() { return baseUrl; }
    public String certificateFingerprint() { return certificateFingerprint; }
    public String computerId() { return computerId; }
    public String localUrl() { return localUrl; }
    public String remoteUrl() { return remoteUrl; }
    public String routePreference() { return routePreference; }

    public void setRouteStatusListener(RouteStatusListener listener) {
        routeStatusListener = listener;
        notifyRouteStatus();
        if (monitorRoutes) updateRouteStatusProbe();
    }

    public void probeAvailability(AvailabilityCallback callback) {
        if (callback == null) return;
        executor.execute(() -> {
            boolean online = false;
            String current = baseUrl;
            String local = localUrl;
            String remote = remoteUrl;
            if (!current.isEmpty()) online = probeAddress(current);
            if (!online && !local.isEmpty() && !local.equals(current)) {
                online = probeAddress(local);
            }
            if (!online && !remote.isEmpty() && !remote.equals(current) &&
                    !remote.equals(local)) {
                online = probeAddress(remote);
            }
            callback.onResult(online);
        });
    }

    public String deviceName() {
        String saved = identityPreferences == null
                ? "" : identityPreferences.getString(PHONE_NAME_KEY, "");
        return saved == null || saved.trim().isEmpty()
                ? keyManager.deviceName() : saved.trim();
    }
    public String deviceModel() { return keyManager.model(); }

    public boolean setDeviceName(String value) {
        String name = value == null ? "" : value.trim();
        if (identityPreferences == null || name.isEmpty() || name.length() > 64) return false;
        identityPreferences.edit().putString(PHONE_NAME_KEY, name).commit();
        return true;
    }

    public void updateDeviceName(String value, ActionCallback callback) {
        String name = value == null ? "" : value.trim();
        if (!setDeviceName(name)) {
            callback.onError("手机名称不能为空且不能超过 64 个字符");
            return;
        }
        JsonObject body = new JsonObject();
        body.addProperty("name", name);
        signedRequest("POST", "/api/device/profile", body.toString(), new ResponseCallback() {
            @Override public void onResponse(JsonObject root, int status) { callback.onSuccess(); }
            @Override public void onError(String message) { callback.onError(message); }
        });
    }

    public void setBaseUrl(String value) {
        baseUrl = normalize(value);
        synchronized (authLock) { token = ""; tokenExpiresAt = 0L; }
        if (connectionStore != null && !computerId.isEmpty()) {
            connectionStore.updateActiveBaseUrl(baseUrl);
        } else if (preferences != null) {
            preferences.edit().putString("baseUrl", baseUrl).apply();
        }
        if (monitorRoutes) {
            updateRouteStatusProbe();
        }
    }

    /** Stop network callbacks when the owning activity is discarded. */
    public void close() {
        clientClosed = true;
        stopRouteStatusProbe();
        closeEvents();
        executor.shutdownNow();
        routeProbeExecutor.shutdownNow();
    }

    public void configurePinnedConnection(String value, String fingerprint, String computer) {
        configurePinnedConnections(value, "", value, fingerprint, computer);
    }

    public void configurePinnedConnections(String local, String remote, String preferred,
                                           String fingerprint, String computer) {
        configurePinnedConnections(local, remote, preferred, fingerprint, computer, "", true);
    }

    private void configurePinnedConnections(String local, String remote, String preferred,
                                            String fingerprint, String computer,
                                            String computerName, boolean persist) {
        localUrl = normalizeAddress(local);
        remoteUrl = normalizeAddress(remote);
        routePreference = ComputerConnectionStore.ROUTE_LOCAL;
        baseUrl = normalizeAddress(preferred);
        if (baseUrl.isEmpty()) baseUrl = !localUrl.isEmpty() ? localUrl : remoteUrl;
        certificateFingerprint = normalizeFingerprint(fingerprint);
        computerId = computer == null ? "" : computer;
        persistConnectionState = persist;
        if ((!baseUrl.isEmpty() && !baseUrl.startsWith("https://")) ||
                (!localUrl.isEmpty() && !localUrl.startsWith("https://")) ||
                (!remoteUrl.isEmpty() && !remoteUrl.startsWith("https://"))) {
            throw new IllegalArgumentException("手机连接必须使用 HTTPS");
        }
        token = "";
        tokenExpiresAt = 0L;
        if (persist && connectionStore != null && !computerId.isEmpty()) {
            connectionStore.upsert(new ComputerConnectionStore.Computer(
                    computerId,
                    ComputerConnectionStore.defaultName(computerName, computerId, baseUrl),
                    baseUrl, localUrl, remoteUrl, certificateFingerprint,
                    System.currentTimeMillis()), true);
        } else if (persist && preferences != null) {
            preferences.edit().putString("baseUrl", baseUrl)
                    .putString("localAddress", localUrl)
                    .putString("remoteAddress", remoteUrl)
                    .putString("routePreference", routePreference)
                    .putString("certificateSha256", certificateFingerprint)
                    .putString("computerId", computerId).apply();
        }
        rebuildHttpClient();
        if (monitorRoutes) {
            updateRouteStatusProbe();
        }
    }

    public void listConversations(String provider, ConversationCallback callback) {
        signedRequest("GET", "/api/conversations", "", new ResponseCallback() {
            @Override public void onResponse(JsonObject root, int status) {
                refreshComputerIdentity(root);
                JsonArray items = root.has("items") && root.get("items").isJsonArray()
                        ? root.getAsJsonArray("items") : new JsonArray();
                List<Conversation> result = new ArrayList<>();
                for (JsonElement item : items) {
                    JsonObject value = item.getAsJsonObject();
                    String itemProvider = stringValue(value, "provider");
                    if (provider == null || provider.equalsIgnoreCase(itemProvider)) {
                        result.add(new Conversation(stringValue(value, "id"), stringValue(value, "name"),
                                itemProvider, stringValue(value, "type"),
                                stringValue(value, "group"), stringValue(value, "folder"),
                                stringValue(value, "status")));
                    }
                }
                callback.onSuccess(result);
            }
            @Override public void onError(String message) { callback.onError(message); }
        });
    }

    public void listConversations(ConversationCallback callback) {
        listConversations(null, callback);
    }

    public void control(String conversationId, String action, ActionCallback callback) {
        postJson("/api/conversations/" + safePath(conversationId) + "/control",
                gson.toJson(new ActionPayload(action)), callback);
    }

    public void sendInput(String conversationId, String input, ActionCallback callback) {
        postJson("/api/conversations/" + safePath(conversationId) + "/input",
                gson.toJson(new InputPayload(input)), callback);
    }

    public void sendMessage(String conversationId, String message, ActionCallback callback) {
        postJson("/api/conversations/" + safePath(conversationId) + "/chat",
                gson.toJson(new MessagePayload(message)), callback);
    }

    public void sendKey(String conversationId, String key, ActionCallback callback) {
        if (!"left".equals(key) && !"right".equals(key) &&
                !"up".equals(key) && !"down".equals(key) &&
                !"enter".equals(key)) {
            callback.onError("不支持的终端按键");
            return;
        }
        postJson("/api/conversations/" + safePath(conversationId) + "/key",
                gson.toJson(new KeyPayload(key)), callback);
    }

    public void uploadImage(String conversationId, byte[] image, String contentType,
                            AttachmentCallback callback) {
        if (image == null || image.length == 0) {
            callback.onError("图片内容为空");
            return;
        }
        if (image.length > 6 * 1024 * 1024) {
            callback.onError("图片超过 6MB 上传上限");
            return;
        }
        String normalizedType = contentType == null ? "" : contentType.toLowerCase(Locale.US);
        if (!"image/jpeg".equals(normalizedType) && !"image/png".equals(normalizedType) &&
                !"image/webp".equals(normalizedType)) {
            callback.onError("仅支持 JPEG、PNG 或 WebP 图片");
            return;
        }
        String path = "/api/conversations/" + safePath(conversationId)
                + "/attachment?uploadId=" + UUID.randomUUID().toString();
        signedBinaryRequest("POST", path, image, normalizedType, callback);
    }

    public void listEvents(String conversationId, long afterEventId, HistoryCallback callback) {
        String path = "/api/conversations/" + safePath(conversationId)
                + "/events?after=" + Math.max(0, afterEventId) + "&limit=1000";
        signedRequest("GET", path, "", new ResponseCallback() {
            @Override public void onResponse(JsonObject root, int status) {
                JsonArray items = root.has("items") && root.get("items").isJsonArray()
                        ? root.getAsJsonArray("items") : new JsonArray();
                List<String> events = new ArrayList<>();
                for (JsonElement item : items) events.add(item.toString());
                callback.onSuccess(events);
            }
            @Override public void onError(String message) { callback.onError(message); }
        });
    }

    public void loadTerminalHistory(String conversationId, String historyToken,
                                    int beforeLine, int limit,
                                    TerminalHistoryCallback callback) {
        String path = "/api/conversations/" + safePath(conversationId)
                + "/terminal/history?token=" + safePath(historyToken)
                + "&beforeLine=" + Math.max(0, beforeLine)
                + "&limit=" + Math.max(1, Math.min(200, limit));
        signedRequest("GET", path, "", new ResponseCallback() {
            @Override public void onResponse(JsonObject root, int status) {
                JsonArray values = root.has("lines") && root.get("lines").isJsonArray()
                        ? root.getAsJsonArray("lines") : new JsonArray();
                List<String> lines = new ArrayList<>();
                for (JsonElement value : values) {
                    lines.add(value == null || value.isJsonNull() ? "" : value.getAsString());
                }
                callback.onSuccess(new TerminalHistoryPage(
                        stringValue(root, "historyToken"), lines,
                        root.has("startLine") ? root.get("startLine").getAsInt() : 0,
                        root.has("totalLines") ? root.get("totalLines").getAsInt() : 0,
                        root.has("hasMore") && root.get("hasMore").getAsBoolean(),
                        root.has("expired") && root.get("expired").getAsBoolean()));
            }
            @Override public void onError(String message) { callback.onError(message); }
        });
    }

    public void connectEvents(EventCallback callback) {
        connectEvents("", callback);
    }

    public void connectEvents(String conversationId, EventCallback callback) {
        closeEvents();
        final long generation = eventsGeneration.incrementAndGet();
        eventsCallback = callback;
        eventsConversationId = conversationId == null ? "" : conversationId;
        closed = false;
        executor.execute(() -> {
            if (generation != eventsGeneration.get() || closed) return;
            String attemptedAddress = baseUrl;
            try {
                ensureAuthenticated();
                final String connectionAddress = baseUrl;
                String path = "/api/events";
                String safeConversation = safePath(conversationId);
                String safeTerminalSession = safePath(callback == null
                        ? "" : callback.terminalSessionId());
                long afterSequence = callback == null
                        ? 0L : Math.max(0L, callback.terminalSequence());
                StringBuilder query = new StringBuilder();
                if (!safeConversation.isEmpty()) {
                    query.append("conversationId=").append(safeConversation);
                }
                if (!safeTerminalSession.isEmpty()) {
                    if (query.length() > 0) query.append('&');
                    query.append("terminalSessionId=").append(safeTerminalSession);
                }
                if (afterSequence > 0L) {
                    if (query.length() > 0) query.append('&');
                    query.append("afterSequence=").append(afterSequence);
                }
                if (query.length() > 0) path += "?" + query;
                Request request = signedBuilder("GET", path, "", true)
                        .url(webSocketUrl(connectionAddress, path)).build();
                eventsSocket = http.newWebSocket(request, new WebSocketListener() {
                    @Override public void onOpen(@NonNull WebSocket socket,
                                                 @NonNull Response response) {
                        if (generation != eventsGeneration.get()) return;
                        recordSuccessfulAddress(connectionAddress);
                        EventCallback current = eventsCallback;
                        if (current != null) current.onOpen();
                    }
                    @Override public void onMessage(@NonNull WebSocket socket, @NonNull String text) {
                        if (generation != eventsGeneration.get()) return;
                        EventCallback current = eventsCallback;
                        if (current != null) current.onEvent(text);
                    }
                    @Override public void onClosed(@NonNull WebSocket socket, int code, @NonNull String reason) {
                        scheduleReconnect(generation, conversationId, callback);
                    }
                    @Override public void onFailure(@NonNull WebSocket socket, @NonNull Throwable error, Response response) {
                        if (generation != eventsGeneration.get()) return;
                        recoverRoutesAfterFailure(connectionAddress, false);
                        scheduleReconnect(generation, conversationId, callback);
                    }
                });
            } catch (Exception exception) {
                recoverRoutesAfterFailure(attemptedAddress, false);
                scheduleReconnect(generation, conversationId, callback);
            }
        });
    }

    public void closeEvents() {
        eventsGeneration.incrementAndGet();
        closed = true;
        eventsCallback = null;
        if (reconnectTask != null) reconnectTask.cancel(false);
        reconnectTask = null;
        WebSocket socket = eventsSocket;
        eventsSocket = null;
        if (socket != null) socket.close(1000, "screen closed");
    }

    public void clearConnection() {
        closeEvents();
        stopRouteStatusProbe();
        localUrl = "";
        remoteUrl = "";
        baseUrl = "";
        certificateFingerprint = "";
        computerId = "";
        synchronized (authLock) {
            token = "";
            tokenExpiresAt = 0L;
        }
        if (connectionStore != null) connectionStore.removeActive();
        else if (preferences != null) {
            preferences.edit().remove("baseUrl").remove("localAddress")
                    .remove("remoteAddress").remove("routePreference")
                    .remove("certificateSha256")
                    .remove("computerId").apply();
        }
        rebuildHttpClient();
        localRouteHealth = RouteHealth.UNAVAILABLE;
        remoteRouteHealth = RouteHealth.UNAVAILABLE;
        notifyRouteStatus();
    }

    /** Starts the QR pairing flow. The payload is the JSON embedded by desktop. */
    public void pairFromQr(String qrPayload, PairingCallback callback) {
        executor.execute(() -> {
            try {
                JsonObject qr = JsonParser.parseString(qrPayload).getAsJsonObject();
                String local = normalizeAddress(stringValue(qr, "localAddress"));
                String remote = normalizeAddress(stringValue(qr, "remoteAddress"));
                if (local.isEmpty() && remote.isEmpty()) throw new IOException("二维码没有可用电脑地址");
                String fingerprint = stringValue(qr, "certificateSha256");
                String computer = stringValue(qr, "computerId");
                String computerName = stringValue(qr, "computerName");
                String pairingId = stringValue(qr, "pairingId");
                String challenge = stringValue(qr, "challenge");
                if (pairingId.isEmpty() || challenge.isEmpty() || computer.isEmpty())
                    throw new IOException("二维码内容不完整");
                long expiresAt = qr.has("expiresAt") ? qr.get("expiresAt").getAsLong() : 0L;
                if (expiresAt > 0L && expiresAt < System.currentTimeMillis() / 1000L)
                    throw new IOException("二维码已过期，请在电脑端刷新");
                if (fingerprint.isEmpty()) throw new IOException("二维码没有证书指纹");
                callback.onStatus("正在提交", "正在向电脑提交手机公钥…");
                String publicKey = keyManager.publicKeyBase64();
                String phoneName = deviceName();
                String canonical = "AICLI-PAIR|" + computer + "|" + pairingId + "|" + challenge + "|"
                        + phoneId + "|" + phoneName + "|" + keyManager.model() + "|" + publicKey;
                JsonObject request = new JsonObject();
                request.addProperty("pairingId", pairingId);
                request.addProperty("phoneId", phoneId);
                request.addProperty("deviceName", phoneName);
                request.addProperty("model", keyManager.model());
                request.addProperty("publicKey", publicKey);
                request.addProperty("signature", keyManager.sign(canonical));
                Exception connectionError = null;
                boolean submitted = false;
                String[] addresses = local.equals(remote) ? new String[]{local} : new String[]{local, remote};
                for (String address : addresses) {
                    if (address == null || address.isEmpty()) continue;
                    try {
                        configurePinnedConnections(local, remote, address, fingerprint,
                                computer, computerName, false);
                        executeUntrustedPost("/api/pair/request", request.toString());
                        submitted = true;
                        break;
                    } catch (Exception error) {
                        connectionError = error;
                    }
                }
                if (!submitted) throw connectionError == null ? new IOException("无法连接电脑") : connectionError;
                callback.onStatus("等待确认", "电脑端显示手机信息后，请点击确认绑定。");
                long deadline = System.currentTimeMillis() + 5 * 60 * 1000L;
                while (System.currentTimeMillis() < deadline) {
                    JsonObject status = executeUntrustedGet("/api/pair/status?pairingId=" + pairingId);
                    String value = stringValue(status, "status");
                    String statusComputerName = stringValue(status, "computerName");
                    if (!statusComputerName.isEmpty()) computerName = statusComputerName;
                    if ("authorized".equals(value)) {
                        callback.onStatus("已确认", "正在建立安全会话…");
                        ensureAuthenticated();
                        if (connectionStore != null) {
                            connectionStore.upsert(new ComputerConnectionStore.Computer(
                                    computer,
                                    ComputerConnectionStore.defaultName(
                                            computerName, computer, baseUrl),
                                    baseUrl, localUrl, remoteUrl,
                                    certificateFingerprint, System.currentTimeMillis()), true);
                        }
                        persistConnectionState = true;
                        callback.onComplete();
                        return;
                    }
                    if ("rejected".equals(value)) throw new IOException("电脑拒绝了本次绑定");
                    Thread.sleep(1200L);
                }
                throw new IOException("等待电脑确认超时");
            } catch (Exception exception) {
                callback.onError(messageOf(exception));
            }
        });
    }

    private void postJson(String path, String body, ActionCallback callback) {
        signedRequest("POST", path, body, new ResponseCallback() {
            @Override public void onResponse(JsonObject root, int status) { callback.onSuccess(); }
            @Override public void onError(String message) { callback.onError(message); }
        });
    }

    private interface ResponseCallback { void onResponse(JsonObject root, int status); void onError(String message); }

    private void signedRequest(String method, String path, String body, ResponseCallback callback) {
        executor.execute(() -> {
            Exception lastFailure = null;
            for (int connectionAttempt = 0; connectionAttempt < 2; connectionAttempt++) {
                String attemptedAddress = baseUrl;
                try {
                    Response response = executeSigned(method, path, body, attemptedAddress);
                    String text = response.body() == null ? "{}" : response.body().string();
                    int status = response.code();
                    response.close();
                    if (status == 401) {
                        synchronized (authLock) { token = ""; tokenExpiresAt = 0L; }
                        ensureAuthenticated();
                        response = executeSigned(method, path, body, attemptedAddress);
                        text = response.body() == null ? "{}" : response.body().string();
                        status = response.code();
                        response.close();
                    }
                    if (status < 200 || status >= 300) {
                        callback.onError("电脑返回 HTTP " + status + errorText(text));
                        return;
                    }
                    recordSuccessfulAddress(attemptedAddress);
                    callback.onResponse(parseObject(text), status);
                    return;
                } catch (Exception exception) {
                    lastFailure = exception;
                    if (connectionAttempt == 0 &&
                            recoverRoutesAfterFailure(attemptedAddress, true)) continue;
                    break;
                }
            }
            callback.onError(messageOf(lastFailure == null ? new IOException("连接失败") : lastFailure));
        });
    }

    private void signedBinaryRequest(String method, String path, byte[] body,
                                     String contentType, AttachmentCallback callback) {
        executor.execute(() -> {
            Exception lastFailure = null;
            for (int connectionAttempt = 0; connectionAttempt < 2; connectionAttempt++) {
                String attemptedAddress = baseUrl;
                try {
                    Response response = executeSignedBinary(
                            method, path, body, contentType, attemptedAddress);
                    String text = response.body() == null ? "{}" : response.body().string();
                    int status = response.code();
                    response.close();
                    if (status == 401) {
                        synchronized (authLock) { token = ""; tokenExpiresAt = 0L; }
                        ensureAuthenticated();
                        response = executeSignedBinary(
                                method, path, body, contentType, attemptedAddress);
                        text = response.body() == null ? "{}" : response.body().string();
                        status = response.code();
                        response.close();
                    }
                    if (status < 200 || status >= 300) {
                        callback.onError("电脑返回 HTTP " + status + errorText(text));
                        return;
                    }
                    recordSuccessfulAddress(attemptedAddress);
                    callback.onSuccess(stringValue(parseObject(text), "path"));
                    return;
                } catch (Exception exception) {
                    lastFailure = exception;
                    if (connectionAttempt == 0 &&
                            recoverRoutesAfterFailure(attemptedAddress, true)) continue;
                    break;
                }
            }
            callback.onError(messageOf(lastFailure == null ? new IOException("连接失败") : lastFailure));
        });
    }

    private Response executeSigned(String method, String path, String body,
                                   String address) throws Exception {
        if (!transportError.isEmpty()) throw new IOException(transportError);
        ensureAuthenticated();
        return http.newCall(signedBuilder(method, path, body, true)
                .url(normalize(address) + path).build()).execute();
    }

    private Response executeSignedBinary(String method, String path, byte[] body,
                                         String contentType, String address) throws Exception {
        if (!transportError.isEmpty()) throw new IOException(transportError);
        ensureAuthenticated();
        return http.newCall(signedBinaryBuilder(method, path, body, contentType, true)
                .url(normalize(address) + path).build()).execute();
    }

    private Request.Builder signedBuilder(String method, String path, String body, boolean includeToken)
            throws Exception {
        if (phoneId.isEmpty()) throw new IOException("设备身份未初始化");
        String canonicalPath = path;
        int query = canonicalPath.indexOf('?');
        if (query >= 0) canonicalPath = canonicalPath.substring(0, query);
        String requestId = UUID.randomUUID().toString();
        long timestamp = System.currentTimeMillis() / 1000L;
        long currentSequence = nextSequence();
        String canonical = method + "\n" + canonicalPath + "\n" + requestId + "\n"
                + timestamp + "\n" + currentSequence + "\n" + sha256Hex(body);
        RequestBody requestBody = body.isEmpty() ? null : RequestBody.create(body, JSON);
        Request.Builder builder = new Request.Builder().url(baseUrl + path)
                .header("X-Aicli-Phone", phoneId)
                .header("X-Aicli-Request-Id", requestId)
                .header("X-Aicli-Timestamp", Long.toString(timestamp))
                .header("X-Aicli-Sequence", Long.toString(currentSequence))
                .header("X-Aicli-Signature", keyManager.sign(canonical));
        if (includeToken && !token.isEmpty()) builder.header("Authorization", "Bearer " + token);
        if ("GET".equals(method)) return builder.get();
        return builder.method(method, requestBody);
    }

    private Request.Builder signedBinaryBuilder(String method, String path, byte[] body,
                                                String contentType, boolean includeToken)
            throws Exception {
        if (phoneId.isEmpty()) throw new IOException("设备身份未初始化");
        String canonicalPath = path;
        int query = canonicalPath.indexOf('?');
        if (query >= 0) canonicalPath = canonicalPath.substring(0, query);
        String requestId = UUID.randomUUID().toString();
        long timestamp = System.currentTimeMillis() / 1000L;
        long currentSequence = nextSequence();
        String canonical = method + "\n" + canonicalPath + "\n" + requestId + "\n"
                + timestamp + "\n" + currentSequence + "\n" + sha256Hex(body);
        RequestBody requestBody = RequestBody.create(body, MediaType.get(contentType));
        Request.Builder builder = new Request.Builder().url(baseUrl + path)
                .header("X-Aicli-Phone", phoneId)
                .header("X-Aicli-Request-Id", requestId)
                .header("X-Aicli-Timestamp", Long.toString(timestamp))
                .header("X-Aicli-Sequence", Long.toString(currentSequence))
                .header("X-Aicli-Signature", keyManager.sign(canonical));
        if (includeToken && !token.isEmpty()) builder.header("Authorization", "Bearer " + token);
        return builder.method(method, requestBody);
    }

    private void ensureAuthenticated() throws Exception {
        if (phoneId.isEmpty()) throw new IOException("设备身份未初始化");
        synchronized (authLock) {
            if (!token.isEmpty() && tokenExpiresAt > System.currentTimeMillis() / 1000L + 30L) return;
            JsonObject challenge = executeUntrustedGet("/api/auth/challenge?phoneId=" + phoneId);
            String challengeId = stringValue(challenge, "challengeId");
            String nonce = stringValue(challenge, "nonce");
            String serverComputer = stringValue(challenge, "computerId");
            if (!computerId.isEmpty() && !computerId.equals(serverComputer)) throw new IOException("电脑身份不匹配");
            computerId = serverComputer;
            refreshComputerIdentity(challenge);
            String canonical = "AICLI-AUTH|" + computerId + "|" + phoneId + "|" + challengeId + "|" + nonce;
            JsonObject body = new JsonObject();
            body.addProperty("phoneId", phoneId);
            body.addProperty("challengeId", challengeId);
            body.addProperty("signature", keyManager.sign(canonical));
            JsonObject result = executeUntrustedPost("/api/auth/token", body.toString());
            token = stringValue(result, "token");
            tokenExpiresAt = result.has("expiresAt") ? result.get("expiresAt").getAsLong() : 0L;
            if (token.isEmpty()) throw new IOException("电脑没有签发会话令牌");
            refreshComputerIdentity(result);
            if (persistConnectionState && preferences != null) {
                preferences.edit().putString("computerId", computerId)
                        .putString("certificateSha256", certificateFingerprint).apply();
            }
            syncDeviceNameBestEffort();
        }
    }

    private void refreshComputerIdentity(JsonObject value) {
        if (value == null || connectionStore == null) return;
        String id = stringValue(value, "computerId");
        if (id.isEmpty()) id = computerId;
        String name = stringValue(value, "computerName");
        if (!id.isEmpty() && !name.isEmpty()) connectionStore.updateName(id, name);
    }

    private void syncDeviceNameBestEffort() {
        if (http == null || token.isEmpty() || phoneId.isEmpty()) return;
        try {
            JsonObject body = new JsonObject();
            body.addProperty("name", deviceName());
            String text = body.toString();
            try (Response response = http.newCall(signedBuilder(
                    "POST", "/api/device/profile", text, true)
                    .url(baseUrl + "/api/device/profile").build()).execute()) {
                // Older desktops may not expose this endpoint yet. Name sync
                // is best-effort and must never block normal terminal access.
            }
        } catch (Exception ignored) { }
    }

    private JsonObject executeUntrustedGet(String path) throws Exception {
        if (!transportError.isEmpty()) throw new IOException(transportError);
        Response response = http.newCall(new Request.Builder().url(baseUrl + path).get().build()).execute();
        return readJsonResponse(response);
    }

    private JsonObject executeUntrustedPost(String path, String body) throws Exception {
        if (!transportError.isEmpty()) throw new IOException(transportError);
        Response response = http.newCall(new Request.Builder().url(baseUrl + path)
                .post(RequestBody.create(body, JSON)).build()).execute();
        return readJsonResponse(response);
    }

    private JsonObject readJsonResponse(Response response) throws Exception {
        String body = response.body() == null ? "{}" : response.body().string();
        int status = response.code();
        response.close();
        if (status < 200 || status >= 300) throw new IOException("电脑返回 HTTP " + status + errorText(body));
        return parseObject(body);
    }

    private void rebuildHttpClient() {
        transportError = "";
        try {
            X509TrustManager trustManager = pinnedTrustManager(certificateFingerprint);
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, new TrustManager[]{trustManager}, null);
            SSLSocketFactory socketFactory = context.getSocketFactory();
            OkHttpClient.Builder builder = new OkHttpClient.Builder()
                    .connectTimeout(8, TimeUnit.SECONDS).readTimeout(20, TimeUnit.SECONDS)
                    .writeTimeout(20, TimeUnit.SECONDS).pingInterval(20, TimeUnit.SECONDS);
            builder.sslSocketFactory(socketFactory, trustManager);
            if (!certificateFingerprint.isEmpty()) builder.hostnameVerifier((host, session) -> true);
            http = builder.build();
        } catch (Exception exception) {
            // Never silently fall back to the platform trust store after a
            // pinned TLS setup failed.  A rejecting client fails closed and
            // makes the problem visible to the user instead of downgrading
            // the phone-to-computer channel.
            transportError = "TLS 安全配置失败，请重新扫码绑定电脑";
            try {
                X509TrustManager rejecting = new X509TrustManager() {
                    @Override public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException { throw new CertificateException("不允许的客户端证书"); }
                    @Override public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
                    @Override public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException { throw new CertificateException("未配置证书指纹"); }
                };
                SSLContext context = SSLContext.getInstance("TLS");
                context.init(null, new TrustManager[]{rejecting}, null);
                http = new OkHttpClient.Builder().sslSocketFactory(context.getSocketFactory(), rejecting)
                        .connectTimeout(8, TimeUnit.SECONDS).readTimeout(20, TimeUnit.SECONDS)
                        .writeTimeout(20, TimeUnit.SECONDS).build();
            } catch (Exception ignored) {
                http = null;
            }
        }
    }

    private X509TrustManager pinnedTrustManager(String fingerprint) throws Exception {
        TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((java.security.KeyStore) null);
        X509TrustManager system = null;
        for (TrustManager manager : factory.getTrustManagers()) if (manager instanceof X509TrustManager) system = (X509TrustManager) manager;
        final X509TrustManager delegate = system;
        return new X509TrustManager() {
            @Override public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
                if (delegate != null) delegate.checkClientTrusted(chain, authType);
            }
            @Override public X509Certificate[] getAcceptedIssuers() { return delegate == null ? new X509Certificate[0] : delegate.getAcceptedIssuers(); }
            @Override public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
                if (chain == null || chain.length == 0) throw new CertificateException("服务器没有证书");
                if (fingerprint == null || fingerprint.isEmpty()) {
                    if (delegate != null) delegate.checkServerTrusted(chain, authType);
                    return;
                }
                try {
                    String actual = bytesToHex(MessageDigest.getInstance("SHA-256").digest(chain[0].getEncoded()));
                    if (!normalizeFingerprint(fingerprint).equals(actual)) throw new CertificateException("证书指纹不匹配");
                } catch (CertificateException exception) { throw exception; }
                catch (Exception exception) { throw new CertificateException(exception); }
            }
        };
    }

    private void scheduleReconnect(long generation, String conversationId,
                                   EventCallback callback) {
        if (generation != eventsGeneration.get() || closed || callback == null) return;
        callback.onClosed();
        if (reconnectTask == null || reconnectTask.isDone()) {
            reconnectTask = executor.schedule(
                    () -> {
                        if (generation == eventsGeneration.get() && !closed) {
                            connectEvents(conversationId, callback);
                        }
                    }, 2, TimeUnit.SECONDS);
        }
    }

    /**
     * Recovers an unavailable active route without ever selecting an untested
     * address. The failed route is retried while the other configured route is
     * probed at the same time. If both recover, the configured preferred route
     * wins; otherwise the first usable route becomes active.
     */
    private boolean recoverRoutesAfterFailure(String failedAddress,
                                              boolean reconnectEvents) {
        synchronized (routeRecoveryLock) {
            if (clientClosed) return false;
            String failed = normalize(failedAddress);
            String current = normalize(baseUrl);
            if (current.isEmpty()) return false;

            // A different recovery operation already selected a healthy route.
            if (!failed.isEmpty() && !failed.equals(current)) return true;

            String preferred = preferredRouteAddress();
            String other = alternateRouteAddress(current);
            markAddressHealth(current, RouteHealth.OFFLINE);
            markAddressConnecting(current);
            if (!other.isEmpty()) markAddressConnecting(other);
            notifyRouteStatus();

            RouteProbeResult result = probeRoutesConcurrently(current, other);
            markAddressHealth(current,
                    result.currentOnline ? RouteHealth.ONLINE : RouteHealth.OFFLINE);
            if (!other.isEmpty()) {
                markAddressHealth(other,
                        result.otherOnline ? RouteHealth.ONLINE : RouteHealth.OFFLINE);
            }

            String selected = "";
            if (!preferred.isEmpty() && preferred.equals(current) && result.currentOnline) {
                selected = current;
            } else if (!preferred.isEmpty() && preferred.equals(other) && result.otherOnline) {
                selected = other;
            } else if (result.currentOnline) {
                selected = current;
            } else if (result.otherOnline) {
                selected = other;
            }

            if (selected.isEmpty()) {
                notifyRouteStatus();
                return false;
            }
            if (!selected.equals(current)) {
                switchCurrentAddress(selected, reconnectEvents);
            }
            recordSuccessfulAddress(selected);
            return true;
        }
    }

    private boolean switchCurrentAddress(String value, boolean reconnectEvents) {
        String address = normalize(value);
        if (address.isEmpty()) return false;
        synchronized (routeLock) {
            if (address.equals(baseUrl)) return false;
            baseUrl = address;
        }
        synchronized (authLock) { token = ""; tokenExpiresAt = 0L; }
        markAddressConnecting(address);
        notifyRouteStatus();
        if (monitorRoutes) {
            updateRouteStatusProbe();
        }
        if (reconnectEvents) reconnectEventsForAddressChange();
        return true;
    }

    private void reconnectEventsForAddressChange() {
        EventCallback callback = eventsCallback;
        String conversationId = eventsConversationId;
        if (callback == null || closed || clientClosed) return;
        connectEvents(conversationId, callback);
    }

    private boolean probeAddress(String address) {
        OkHttpClient current = http;
        if (current == null || !transportError.isEmpty()) return false;
        try {
            OkHttpClient probeClient = current.newBuilder()
                    .connectTimeout(2, TimeUnit.SECONDS)
                    .readTimeout(3, TimeUnit.SECONDS)
                    .writeTimeout(3, TimeUnit.SECONDS)
                    .callTimeout(4, TimeUnit.SECONDS)
                    .build();
            try (Response response = probeClient.newCall(new Request.Builder()
                    .url(address + "/api/health").get().build()).execute()) {
                if (!response.isSuccessful()) return false;
                String body = response.body() == null ? "{}" : response.body().string();
                JsonObject health = parseObject(body);
                String serverComputer = stringValue(health, "computerId");
                return computerId.isEmpty() || computerId.equals(serverComputer);
            }
        } catch (Exception ignored) {
            return false;
        }
    }

    private RouteProbeResult probeRoutesConcurrently(String current, String other) {
        Future<Boolean> currentProbe = null;
        Future<Boolean> otherProbe = null;
        try {
            currentProbe = routeProbeExecutor.submit(() -> probeAddress(current));
            if (!other.isEmpty()) {
                otherProbe = routeProbeExecutor.submit(() -> probeAddress(other));
            }
            boolean currentOnline = awaitProbe(currentProbe);
            boolean otherOnline = otherProbe != null && awaitProbe(otherProbe);
            return new RouteProbeResult(currentOnline, otherOnline);
        } catch (RuntimeException ignored) {
            return new RouteProbeResult(false, false);
        } finally {
            if (currentProbe != null && !currentProbe.isDone()) currentProbe.cancel(true);
            if (otherProbe != null && !otherProbe.isDone()) otherProbe.cancel(true);
        }
    }

    private static boolean awaitProbe(Future<Boolean> probe) {
        if (probe == null) return false;
        try {
            return Boolean.TRUE.equals(probe.get(5, TimeUnit.SECONDS));
        } catch (Exception ignored) {
            probe.cancel(true);
            return false;
        }
    }

    private void updateRouteStatusProbe() {
        if (clientClosed) return;
        synchronized (routeLock) {
            if (routeStatusProbeTask != null && !routeStatusProbeTask.isDone()) {
                notifyRouteStatus();
                return;
            }
            localRouteHealth = localUrl.isEmpty()
                    ? RouteHealth.UNAVAILABLE : RouteHealth.CONNECTING;
            remoteRouteHealth = remoteUrl.isEmpty()
                    ? RouteHealth.UNAVAILABLE : RouteHealth.CONNECTING;
            notifyRouteStatus();
            try {
                routeStatusProbeTask = executor.scheduleWithFixedDelay(
                        this::probeConfiguredRoutes, 0L, 10L, TimeUnit.SECONDS);
            } catch (RuntimeException ignored) { }
        }
    }

    private void stopRouteStatusProbe() {
        synchronized (routeLock) {
            if (routeStatusProbeTask != null) routeStatusProbeTask.cancel(false);
            routeStatusProbeTask = null;
        }
    }

    private void probeConfiguredRoutes() {
        if (clientClosed) return;
        synchronized (routeRecoveryLock) {
            String local = localUrl;
            String remote = remoteUrl;
            String preferred = preferredRouteAddress();
            String current = normalize(baseUrl);
            String fallback = alternateRouteAddress(preferred);

            if (local.isEmpty()) localRouteHealth = RouteHealth.UNAVAILABLE;
            if (remote.isEmpty()) remoteRouteHealth = RouteHealth.UNAVAILABLE;

            if (preferred.isEmpty()) {
                probeOnlyCurrentAddress(current);
                return;
            }

            if (current.equals(preferred)) {
                // While the preferred route is healthy the backup remains
                // completely idle. Only after a failed preferred probe do we
                // retry it and test the backup concurrently.
                markAddressConnecting(preferred);
                markAddressUnavailable(fallback);
                notifyRouteStatus();
                if (probeAddress(preferred)) {
                    markAddressHealth(preferred, RouteHealth.ONLINE);
                    notifyRouteStatus();
                    return;
                }
                recoverRoutesAfterFailure(preferred, true);
                return;
            }

            // The fallback route is active. Keep it untouched and only probe
            // the configured preferred route. Switch after the preferred
            // health check succeeds, never merely because its address exists.
            markAddressConnecting(preferred);
            notifyRouteStatus();
            if (probeAddress(preferred)) {
                markAddressHealth(preferred, RouteHealth.ONLINE);
                if (switchCurrentAddress(preferred, true)) {
                    recordSuccessfulAddress(preferred);
                } else {
                    notifyRouteStatus();
                }
                return;
            }

            markAddressHealth(preferred, RouteHealth.OFFLINE);
            if (current.isEmpty()) {
                notifyRouteStatus();
                return;
            }
            if (addressHealth(current) == RouteHealth.ONLINE) {
                notifyRouteStatus();
                return;
            }

            // This is normally reached only during startup before a live
            // WebSocket has confirmed the saved fallback route.
            markAddressConnecting(current);
            notifyRouteStatus();
            if (probeAddress(current)) {
                markAddressHealth(current, RouteHealth.ONLINE);
                recordSuccessfulAddress(current);
            } else {
                recoverRoutesAfterFailure(current, true);
            }
        }
    }

    private void probeOnlyCurrentAddress(String current) {
        if (current.isEmpty()) {
            notifyRouteStatus();
            return;
        }
        markAddressConnecting(current);
        notifyRouteStatus();
        RouteHealth health = probeAddress(current)
                ? RouteHealth.ONLINE : RouteHealth.OFFLINE;
        markAddressHealth(current, health);
        if (health == RouteHealth.ONLINE) recordSuccessfulAddress(current);
        else notifyRouteStatus();
    }

    private void markAddressConnecting(String address) {
        String normalized = normalize(address);
        if (normalized.isEmpty()) return;
        if (!localUrl.isEmpty() && normalized.equals(localUrl) &&
                localRouteHealth != RouteHealth.ONLINE) {
            localRouteHealth = RouteHealth.CONNECTING;
        }
        if (!remoteUrl.isEmpty() && normalized.equals(remoteUrl) &&
                remoteRouteHealth != RouteHealth.ONLINE) {
            remoteRouteHealth = RouteHealth.CONNECTING;
        }
    }

    private void markAddressUnavailable(String address) {
        markAddressHealth(address, RouteHealth.UNAVAILABLE);
    }

    private void markAddressHealth(String address, RouteHealth health) {
        String normalized = normalize(address);
        if (normalized.isEmpty()) return;
        if (!localUrl.isEmpty() && normalized.equals(localUrl)) {
            localRouteHealth = health;
        }
        if (!remoteUrl.isEmpty() && normalized.equals(remoteUrl)) {
            remoteRouteHealth = health;
        }
    }

    private RouteHealth addressHealth(String address) {
        String normalized = normalize(address);
        if (normalized.isEmpty()) return RouteHealth.UNAVAILABLE;
        if (!localUrl.isEmpty() && normalized.equals(localUrl)) {
            return localRouteHealth;
        }
        if (!remoteUrl.isEmpty() && normalized.equals(remoteUrl)) {
            return remoteRouteHealth;
        }
        return RouteHealth.UNAVAILABLE;
    }

    private void notifyRouteStatus() {
        RouteStatusListener listener = routeStatusListener;
        if (listener == null) return;
        String current = normalize(baseUrl);
        listener.onRouteStatus(localRouteHealth, remoteRouteHealth,
                !localUrl.isEmpty() && current.equals(localUrl),
                !remoteUrl.isEmpty() && current.equals(remoteUrl));
    }

    private String preferredRouteAddress() {
        return ComputerConnectionStore.preferredAddress(
                localUrl, remoteUrl, routePreference);
    }

    private String alternateRouteAddress(String route) {
        String normalized = normalize(route);
        String local = normalize(localUrl);
        String remote = normalize(remoteUrl);
        if (!local.isEmpty() && !local.equals(normalized)) return local;
        if (!remote.isEmpty() && !remote.equals(normalized)) return remote;
        return "";
    }

    private void recordSuccessfulAddress(String value) {
        String address = normalize(value);
        if (address.isEmpty() || !address.equals(baseUrl)) return;
        if (!localUrl.isEmpty() && address.equals(localUrl)) {
            localRouteHealth = RouteHealth.ONLINE;
        }
        if (!remoteUrl.isEmpty() && address.equals(remoteUrl)) {
            remoteRouteHealth = RouteHealth.ONLINE;
        }
        notifyRouteStatus();
        if (persistConnectionState && connectionStore != null && !computerId.isEmpty()) {
            connectionStore.updateBaseUrl(computerId, address);
        } else if (persistConnectionState && preferences != null) {
            preferences.edit().putString("baseUrl", address).apply();
        }
        if (monitorRoutes) {
            updateRouteStatusProbe();
        }
    }

    private static String startingAddress(String local, String remote, String lastSuccessful,
                                          String preference) {
        String saved = normalize(lastSuccessful);
        String normalizedLocal = normalize(local);
        String normalizedRemote = normalize(remote);
        if (!saved.isEmpty() &&
                (saved.equals(normalizedLocal) || saved.equals(normalizedRemote))) {
            return saved;
        }
        String preferred = ComputerConnectionStore.preferredAddress(
                normalizedLocal, normalizedRemote, preference);
        return preferred.isEmpty() ? saved : preferred;
    }

    private static String distinctAddress(String candidate, String current) {
        String normalized = normalize(candidate);
        return normalized.isEmpty() || normalized.equals(normalize(current)) ? "" : normalized;
    }

    private static final class RouteProbeResult {
        final boolean currentOnline;
        final boolean otherOnline;

        RouteProbeResult(boolean currentOnline, boolean otherOnline) {
            this.currentOnline = currentOnline;
            this.otherOnline = otherOnline;
        }
    }

    private long nextSequence() {
        if (preferences == null) {
            synchronized (authLock) { return ++sequence; }
        }
        synchronized (SEQUENCE_LOCK) {
            String suffix = computerId == null || computerId.isEmpty()
                    ? "default" : computerId;
            String key = "sequence." + suffix;
            long legacy = preferences.getLong("sequence", 0L);
            long stored = preferences.getLong(key, 0L);
            long current = Math.max(Math.max(sequence, legacy), stored) + 1L;
            preferences.edit().putLong(key, current).putLong("sequence", current).commit();
            sequence = current;
            return current;
        }
    }

    private static String webSocketUrl(String address, String path) {
        if (address.startsWith("https://")) return "wss://" + address.substring(8) + path;
        if (address.startsWith("http://")) return "ws://" + address.substring(7) + path;
        return address + path;
    }

    private static String normalize(String value) { String result = value == null ? "" : value.trim(); while (result.endsWith("/")) result = result.substring(0, result.length() - 1); return result; }
    private static String normalizeAddress(String value) {
        String result = normalize(value);
        if (result.isEmpty()) return result;
        if (!result.startsWith("https://") && !result.startsWith("http://")) result = "https://" + result;
        return result;
    }
    private static String normalizeFingerprint(String value) { return value == null ? "" : value.replace(":", "").replace(" ", "").toUpperCase(Locale.US); }
    private static String sha256Hex(String value) { try { return bytesToHex(MessageDigest.getInstance("SHA-256").digest(value.getBytes(java.nio.charset.StandardCharsets.UTF_8))); } catch (Exception exception) { return ""; } }
    private static String sha256Hex(byte[] value) { try { return bytesToHex(MessageDigest.getInstance("SHA-256").digest(value)); } catch (Exception exception) { return ""; } }
    private static String bytesToHex(byte[] bytes) { StringBuilder builder = new StringBuilder(bytes.length * 2); for (byte value : bytes) builder.append(String.format(Locale.US, "%02X", value & 0xff)); return builder.toString(); }
    private static String safePath(String value) { return value == null ? "" : value.replaceAll("[^A-Za-z0-9_.-]", ""); }
    private static String stringValue(JsonObject object, String name) { return object.has(name) && !object.get(name).isJsonNull() ? object.get(name).getAsString() : ""; }
    private static JsonObject parseObject(String body) { JsonElement element = JsonParser.parseString(body == null || body.isEmpty() ? "{}" : body); return element.isJsonObject() ? element.getAsJsonObject() : new JsonObject(); }
    private static String errorText(String body) { try { return "：" + stringValue(parseObject(body), "error"); } catch (Exception ignored) { return ""; } }
    private static String messageOf(Exception exception) { return exception.getMessage() == null ? "连接失败" : exception.getMessage(); }

    private static final class ActionPayload { final String action; ActionPayload(String action) { this.action = action; } }
    private static final class InputPayload { final String input; InputPayload(String input) { this.input = input; } }
    private static final class KeyPayload { final String key; KeyPayload(String key) { this.key = key; } }
    private static final class MessagePayload { final String message; final String requestId = UUID.randomUUID().toString(); MessagePayload(String message) { this.message = message; } }
}
