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
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
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

    public interface ConversationCallback { void onSuccess(List<Conversation> conversations); void onError(String message); }
    public interface ActionCallback { void onSuccess(); void onError(String message); }
    public interface EventCallback {
        void onEvent(String eventJson);
        void onClosed();
        default String terminalSessionId() { return ""; }
        default long terminalSequence() { return 0L; }
    }
    public interface HistoryCallback { void onSuccess(List<String> events); void onError(String message); }

    public interface PairingCallback {
        void onStatus(String status, String message);
        void onComplete();
        void onError(String message);
    }

    private final Gson gson = new Gson();
    private final DeviceKeyManager keyManager = new DeviceKeyManager();
    private final ScheduledExecutorService executor = Executors.newScheduledThreadPool(2);
    private final Object authLock = new Object();
    private final SharedPreferences preferences;
    private final String phoneId;
    private volatile OkHttpClient http;
    private volatile String baseUrl = "";
    private volatile String localUrl = "";
    private volatile String remoteUrl = "";
    private volatile String certificateFingerprint = "";
    private volatile String computerId = "";
    private volatile String token = "";
    private volatile long tokenExpiresAt;
    private volatile String transportError = "";
    private long sequence;
    private volatile WebSocket eventsSocket;
    private volatile EventCallback eventsCallback;
    private volatile String eventsConversationId = "";
    private final AtomicLong eventsGeneration = new AtomicLong();
    private ScheduledFuture<?> reconnectTask;
    private volatile boolean closed;

    public AicliClient(Context context, String baseUrl) {
        preferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        SharedPreferences identity = context.getSharedPreferences(PHONE_PREFS, Context.MODE_PRIVATE);
        try {
            phoneId = keyManager.getOrCreatePhoneId(identity);
            keyManager.getOrCreateKeyPair();
        } catch (Exception exception) {
            throw new IllegalStateException("设备密钥初始化失败", exception);
        }
        sequence = preferences.getLong("sequence", 0L);
        this.localUrl = normalize(preferences.getString("localAddress", ""));
        this.remoteUrl = normalize(preferences.getString("remoteAddress", ""));
        String savedBase = preferences.getString("baseUrl", "");
        this.baseUrl = normalize(baseUrl == null || baseUrl.trim().isEmpty() ? savedBase : baseUrl);
        this.certificateFingerprint = normalizeFingerprint(preferences.getString("certificateSha256", ""));
        this.computerId = preferences.getString("computerId", "");
        rebuildHttpClient();
    }

    /** Compatibility constructor used only by old callers; it has no identity until configured. */
    public AicliClient(String baseUrl) {
        preferences = null;
        phoneId = "";
        this.baseUrl = normalize(baseUrl);
        rebuildHttpClient();
    }

    public String phoneId() { return phoneId; }
    public String baseUrl() { return baseUrl; }
    public String certificateFingerprint() { return certificateFingerprint; }
    public String computerId() { return computerId; }
    public String localUrl() { return localUrl; }
    public String remoteUrl() { return remoteUrl; }

    public String deviceName() { return keyManager.deviceName(); }
    public String deviceModel() { return keyManager.model(); }

    public void setBaseUrl(String value) {
        baseUrl = normalize(value);
        if (preferences != null) preferences.edit().putString("baseUrl", baseUrl).apply();
        rebuildHttpClient();
    }

    /** Stop network callbacks when the owning activity is discarded. */
    public void close() {
        closeEvents();
        executor.shutdownNow();
    }

    public void configurePinnedConnection(String value, String fingerprint, String computer) {
        configurePinnedConnections(value, "", value, fingerprint, computer);
    }

    public void configurePinnedConnections(String local, String remote, String preferred,
                                           String fingerprint, String computer) {
        localUrl = normalizeAddress(local);
        remoteUrl = normalizeAddress(remote);
        baseUrl = normalizeAddress(preferred);
        if (baseUrl.isEmpty()) baseUrl = !localUrl.isEmpty() ? localUrl : remoteUrl;
        certificateFingerprint = normalizeFingerprint(fingerprint);
        computerId = computer == null ? "" : computer;
        if ((!baseUrl.isEmpty() && !baseUrl.startsWith("https://")) ||
                (!localUrl.isEmpty() && !localUrl.startsWith("https://")) ||
                (!remoteUrl.isEmpty() && !remoteUrl.startsWith("https://"))) {
            throw new IllegalArgumentException("手机连接必须使用 HTTPS");
        }
        token = "";
        tokenExpiresAt = 0L;
        if (preferences != null) {
            preferences.edit().putString("baseUrl", baseUrl)
                    .putString("localAddress", localUrl)
                    .putString("remoteAddress", remoteUrl)
                    .putString("certificateSha256", certificateFingerprint)
                    .putString("computerId", computerId).apply();
        }
        rebuildHttpClient();
    }

    public void listConversations(String provider, ConversationCallback callback) {
        signedRequest("GET", "/api/conversations", "", new ResponseCallback() {
            @Override public void onResponse(JsonObject root, int status) {
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
            try {
                ensureAuthenticated();
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
                        .url(webSocketUrl(path)).build();
                eventsSocket = http.newWebSocket(request, new WebSocketListener() {
                    @Override public void onMessage(@NonNull WebSocket socket, @NonNull String text) {
                        if (generation != eventsGeneration.get()) return;
                        EventCallback current = eventsCallback;
                        if (current != null) current.onEvent(text);
                    }
                    @Override public void onClosed(@NonNull WebSocket socket, int code, @NonNull String reason) {
                        scheduleReconnect(generation, conversationId, callback);
                    }
                    @Override public void onFailure(@NonNull WebSocket socket, @NonNull Throwable error, Response response) {
                        scheduleReconnect(generation, conversationId, callback);
                    }
                });
            } catch (Exception exception) {
                switchToAlternateAddress();
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
        localUrl = "";
        remoteUrl = "";
        baseUrl = "";
        certificateFingerprint = "";
        computerId = "";
        synchronized (authLock) {
            token = "";
            tokenExpiresAt = 0L;
        }
        if (preferences != null) preferences.edit().clear().apply();
        rebuildHttpClient();
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
                String canonical = "AICLI-PAIR|" + computer + "|" + pairingId + "|" + challenge + "|"
                        + phoneId + "|" + keyManager.deviceName() + "|" + keyManager.model() + "|" + publicKey;
                JsonObject request = new JsonObject();
                request.addProperty("pairingId", pairingId);
                request.addProperty("phoneId", phoneId);
                request.addProperty("deviceName", keyManager.deviceName());
                request.addProperty("model", keyManager.model());
                request.addProperty("publicKey", publicKey);
                request.addProperty("signature", keyManager.sign(canonical));
                Exception connectionError = null;
                boolean submitted = false;
                String[] addresses = local.equals(remote) ? new String[]{local} : new String[]{local, remote};
                for (String address : addresses) {
                    if (address == null || address.isEmpty()) continue;
                    try {
                        configurePinnedConnections(local, remote, address, fingerprint, computer);
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
                    if ("authorized".equals(value)) {
                        callback.onStatus("已确认", "正在建立安全会话…");
                        ensureAuthenticated();
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
                try {
                    Response response = executeSigned(method, path, body);
                    String text = response.body() == null ? "{}" : response.body().string();
                    int status = response.code();
                    response.close();
                    if (status == 401) {
                        synchronized (authLock) { token = ""; tokenExpiresAt = 0L; }
                        ensureAuthenticated();
                        response = executeSigned(method, path, body);
                        text = response.body() == null ? "{}" : response.body().string();
                        status = response.code();
                        response.close();
                    }
                    if (status < 200 || status >= 300) {
                        callback.onError("电脑返回 HTTP " + status + errorText(text));
                        return;
                    }
                    callback.onResponse(parseObject(text), status);
                    return;
                } catch (Exception exception) {
                    lastFailure = exception;
                    if (connectionAttempt == 0 && switchToAlternateAddress()) continue;
                    break;
                }
            }
            callback.onError(messageOf(lastFailure == null ? new IOException("连接失败") : lastFailure));
        });
    }

    private Response executeSigned(String method, String path, String body) throws Exception {
        if (!transportError.isEmpty()) throw new IOException(transportError);
        ensureAuthenticated();
        return http.newCall(signedBuilder(method, path, body, true)
                .url(baseUrl + path).build()).execute();
    }

    private Request.Builder signedBuilder(String method, String path, String body, boolean includeToken)
            throws Exception {
        if (phoneId.isEmpty()) throw new IOException("设备身份未初始化");
        String canonicalPath = path;
        int query = canonicalPath.indexOf('?');
        if (query >= 0) canonicalPath = canonicalPath.substring(0, query);
        String requestId = UUID.randomUUID().toString();
        long timestamp = System.currentTimeMillis() / 1000L;
        long currentSequence;
        synchronized (authLock) {
            currentSequence = ++sequence;
            if (preferences != null) preferences.edit().putLong("sequence", currentSequence).apply();
        }
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
            String canonical = "AICLI-AUTH|" + computerId + "|" + phoneId + "|" + challengeId + "|" + nonce;
            JsonObject body = new JsonObject();
            body.addProperty("phoneId", phoneId);
            body.addProperty("challengeId", challengeId);
            body.addProperty("signature", keyManager.sign(canonical));
            JsonObject result = executeUntrustedPost("/api/auth/token", body.toString());
            token = stringValue(result, "token");
            tokenExpiresAt = result.has("expiresAt") ? result.get("expiresAt").getAsLong() : 0L;
            if (token.isEmpty()) throw new IOException("电脑没有签发会话令牌");
            if (preferences != null) preferences.edit().putString("computerId", computerId)
                    .putString("certificateSha256", certificateFingerprint).apply();
        }
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

    private boolean switchToAlternateAddress() {
        String alternate = "";
        if (!localUrl.isEmpty() && !localUrl.equals(baseUrl)) alternate = localUrl;
        else if (!remoteUrl.isEmpty() && !remoteUrl.equals(baseUrl)) alternate = remoteUrl;
        if (alternate.isEmpty()) return false;
        baseUrl = alternate;
        synchronized (authLock) { token = ""; tokenExpiresAt = 0L; }
        if (preferences != null) preferences.edit().putString("baseUrl", baseUrl).apply();
        rebuildHttpClient();
        return true;
    }

    private String webSocketUrl(String path) {
        if (baseUrl.startsWith("https://")) return "wss://" + baseUrl.substring(8) + path;
        if (baseUrl.startsWith("http://")) return "ws://" + baseUrl.substring(7) + path;
        return baseUrl + path;
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
