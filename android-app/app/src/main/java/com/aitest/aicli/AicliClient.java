package com.aitest.aicli;

import androidx.annotation.NonNull;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;

/** Small HTTP/WebSocket client shared by the Android screens. */
public final class AicliClient {
    private static final MediaType JSON = MediaType.get("application/json; charset=utf-8");

    public static final class Conversation {
        public final String id;
        public final String name;
        public final String provider;
        public final String status;

        Conversation(String id, String name, String provider, String status) {
            this.id = id;
            this.name = name;
            this.provider = provider;
            this.status = status;
        }
    }

    public interface ConversationCallback {
        void onSuccess(List<Conversation> conversations);
        void onError(String message);
    }

    public interface ActionCallback {
        void onSuccess();
        void onError(String message);
    }

    public interface EventCallback {
        void onEvent(String eventJson);
        void onClosed();
    }

    public interface HistoryCallback {
        void onSuccess(List<String> events);
        void onError(String message);
    }

    private final OkHttpClient http = new OkHttpClient.Builder()
            .connectTimeout(5, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .writeTimeout(15, TimeUnit.SECONDS)
            .build();
    private final Gson gson = new Gson();
    private volatile String baseUrl;
    private WebSocket eventsSocket;

    public AicliClient(String baseUrl) {
        setBaseUrl(baseUrl);
    }

    public void setBaseUrl(String baseUrl) {
        String normalized = baseUrl == null ? "" : baseUrl.trim();
        while (normalized.endsWith("/")) {
            normalized = normalized.substring(0, normalized.length() - 1);
        }
        this.baseUrl = normalized;
    }

    public String baseUrl() {
        return baseUrl;
    }

    public void listConversations(String provider, ConversationCallback callback) {
        if (baseUrl.isEmpty()) {
            callback.onError("尚未设置电脑地址");
            return;
        }
        Request request = new Request.Builder().url(baseUrl + "/api/conversations").get().build();
        http.newCall(request).enqueue(new Callback() {
            @Override
            public void onFailure(@NonNull Call call, @NonNull IOException error) {
                callback.onError(error.getMessage() == null ? "连接失败" : error.getMessage());
            }

            @Override
            public void onResponse(@NonNull Call call, @NonNull Response response) {
                try (Response responseToClose = response) {
                    if (!response.isSuccessful() || response.body() == null) {
                        callback.onError("电脑返回 HTTP " + response.code());
                        return;
                    }
                    JsonObject root = JsonParser.parseString(response.body().string()).getAsJsonObject();
                    JsonArray items = root.has("items") && root.get("items").isJsonArray()
                            ? root.getAsJsonArray("items") : new JsonArray();
                    List<Conversation> result = new ArrayList<>();
                    for (JsonElement item : items) {
                        JsonObject value = item.getAsJsonObject();
                        String itemProvider = stringValue(value, "provider");
                        if (provider == null || provider.equals(itemProvider)) {
                            result.add(new Conversation(
                                    stringValue(value, "id"),
                                    stringValue(value, "name"),
                                    itemProvider,
                                    stringValue(value, "status")));
                        }
                    }
                    callback.onSuccess(result);
                } catch (Exception parseError) {
                    callback.onError("响应格式错误");
                }
            }
        });
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

    public void listEvents(String conversationId, long afterEventId, HistoryCallback callback) {
        if (baseUrl.isEmpty()) {
            callback.onError("尚未设置电脑地址");
            return;
        }
        String url = baseUrl + "/api/conversations/" + safePath(conversationId)
                + "/events?after=" + Math.max(0, afterEventId) + "&limit=1000";
        Request request = new Request.Builder().url(url).get().build();
        http.newCall(request).enqueue(new Callback() {
            @Override
            public void onFailure(@NonNull Call call, @NonNull IOException error) {
                callback.onError(error.getMessage() == null ? "历史记录读取失败" : error.getMessage());
            }

            @Override
            public void onResponse(@NonNull Call call, @NonNull Response response) {
                try (Response responseToClose = response) {
                    if (!response.isSuccessful() || response.body() == null) {
                        callback.onError("电脑返回 HTTP " + response.code());
                        return;
                    }
                    JsonObject root = JsonParser.parseString(response.body().string()).getAsJsonObject();
                    JsonArray items = root.has("items") && root.get("items").isJsonArray()
                            ? root.getAsJsonArray("items") : new JsonArray();
                    List<String> events = new ArrayList<>();
                    for (JsonElement item : items) {
                        events.add(item.toString());
                    }
                    callback.onSuccess(events);
                } catch (Exception parseError) {
                    callback.onError("历史记录格式错误");
                }
            }
        });
    }

    public void connectEvents(EventCallback callback) {
        closeEvents();
        if (baseUrl.isEmpty()) {
            callback.onClosed();
            return;
        }
        String wsUrl = baseUrl.replaceFirst("^http://", "ws://")
                .replaceFirst("^https://", "wss://") + "/api/events";
        Request request = new Request.Builder().url(wsUrl).get().build();
        eventsSocket = http.newWebSocket(request, new WebSocketListener() {
            @Override
            public void onMessage(@NonNull WebSocket webSocket, @NonNull String text) {
                callback.onEvent(text);
            }

            @Override
            public void onClosed(@NonNull WebSocket webSocket, int code, @NonNull String reason) {
                callback.onClosed();
            }

            @Override
            public void onFailure(@NonNull WebSocket webSocket, @NonNull Throwable throwable, Response response) {
                callback.onClosed();
            }
        });
    }

    public void closeEvents() {
        if (eventsSocket != null) {
            eventsSocket.close(1000, "screen closed");
            eventsSocket = null;
        }
    }

    private void postJson(String path, String json, ActionCallback callback) {
        if (baseUrl.isEmpty()) {
            callback.onError("尚未设置电脑地址");
            return;
        }
        RequestBody body = RequestBody.create(json, JSON);
        Request request = new Request.Builder().url(baseUrl + path).post(body).build();
        http.newCall(request).enqueue(new Callback() {
            @Override
            public void onFailure(@NonNull Call call, @NonNull IOException error) {
                callback.onError(error.getMessage() == null ? "连接失败" : error.getMessage());
            }

            @Override
            public void onResponse(@NonNull Call call, @NonNull Response response) {
                try (Response ignored = response) {
                    if (response.isSuccessful()) {
                        callback.onSuccess();
                    } else {
                        callback.onError("电脑返回 HTTP " + response.code());
                    }
                }
            }
        });
    }

    private static String safePath(String value) {
        return value == null ? "" : value.replaceAll("[^A-Za-z0-9_.-]", "");
    }

    private static String stringValue(JsonObject object, String name) {
        return object.has(name) && !object.get(name).isJsonNull()
                ? object.get(name).getAsString() : "";
    }

    private static final class ActionPayload {
        final String action;
        ActionPayload(String action) { this.action = action; }
    }

    private static final class InputPayload {
        final String input;
        InputPayload(String input) { this.input = input; }
    }

    private static final class MessagePayload {
        final String message;
        final String requestId;

        MessagePayload(String message) {
            this.message = message;
            this.requestId = java.util.UUID.randomUUID().toString();
        }
    }
}
