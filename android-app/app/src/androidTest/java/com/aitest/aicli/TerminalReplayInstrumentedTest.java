package com.aitest.aicli;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertEquals;

import android.content.Context;
import android.os.Bundle;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;
import java.util.ArrayList;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;

/** Read-only end-to-end checks for the retained native terminal stream. */
@RunWith(AndroidJUnit4.class)
public final class TerminalReplayInstrumentedTest {
    @Test
    public void claudeReplayContainsExpectedText() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        String expected = arguments.getString("expectedText", "nihao");
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        AicliClient client = new AicliClient(context, "");
        AtomicReference<String> failure = new AtomicReference<>();

        CountDownLatch listed = new CountDownLatch(1);
        AtomicReference<List<AicliClient.Conversation>> conversations = new AtomicReference<>();
        client.listConversations(new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> result) {
                conversations.set(result);
                listed.countDown();
            }
            @Override public void onError(String message) {
                failure.set(message);
                listed.countDown();
            }
        });
        assertTrue("conversation request timed out", listed.await(30, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());

        AicliClient.Conversation claude = null;
        for (AicliClient.Conversation conversation : conversations.get()) {
            String identity = (conversation.provider + " " + conversation.type + " "
                    + conversation.name).toLowerCase(Locale.US);
            if (identity.contains("claude")) {
                claude = conversation;
                break;
            }
        }
        assertNotNull("no Claude project was returned", claude);

        CountDownLatch found = new CountDownLatch(1);
        AtomicInteger dataFrames = new AtomicInteger();
        AtomicLong maximumSequence = new AtomicLong();
        AtomicReference<String> terminalSession = new AtomicReference<>("");
        AtomicBoolean historyStarted = new AtomicBoolean();
        final AicliClient.Conversation target = claude;
        client.connectEvents(claude.id, new AicliClient.EventCallback() {
            @Override public void onEvent(String eventJson) {
                try {
                    JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
                    if (event.has("terminalSessionId")) {
                        terminalSession.set(event.get("terminalSessionId").getAsString());
                    }
                    if (!event.has("type")) return;
                    String type = event.get("type").getAsString();
                    if ("terminal.bootstrap".equals(type)) {
                        JsonObject payload = event.getAsJsonObject("payload");
                        JsonArray lines = payload == null ? null : payload.getAsJsonArray("lines");
                        if (lines != null) {
                            for (JsonElement line : lines) {
                                if (line != null && !line.isJsonNull() &&
                                        line.getAsString().contains(expected)) {
                                    found.countDown();
                                    break;
                                }
                            }
                        }
                        if (found.getCount() > 0 && payload != null &&
                                payload.has("hasMore") &&
                                payload.get("hasMore").getAsBoolean() &&
                                historyStarted.compareAndSet(false, true)) {
                            loadOlderHistory(client, target.id,
                                    payload.get("historyToken").getAsString(),
                                    payload.get("startLine").getAsInt(), expected,
                                    found, failure);
                        }
                        return;
                    }
                    if (!"terminal.data".equals(type)) return;
                    dataFrames.incrementAndGet();
                    if (event.has("sequence")) {
                        maximumSequence.accumulateAndGet(
                                event.get("sequence").getAsLong(), Math::max);
                    }
                    JsonObject payload = event.getAsJsonObject("payload");
                    if (payload != null && payload.has("data") &&
                            payload.get("data").getAsString().contains(expected)) {
                        found.countDown();
                    }
                } catch (Exception ignored) { }
            }
            @Override public void onClosed() { }
        });
        boolean received = found.await(20, TimeUnit.SECONDS);
        client.close();
        assertNull(failure.get(), failure.get());
        assertTrue("retained Claude stream did not contain '" + expected
                        + "'; frames=" + dataFrames.get()
                        + ", maxSequence=" + maximumSequence.get()
                        + ", session=" + terminalSession.get(),
                received);
    }

    private static void loadOlderHistory(AicliClient client, String conversationId,
                                         String token, int beforeLine, String expected,
                                         CountDownLatch found,
                                         AtomicReference<String> failure) {
        client.loadTerminalHistory(conversationId, token, beforeLine, 200,
                new AicliClient.TerminalHistoryCallback() {
            @Override public void onSuccess(AicliClient.TerminalHistoryPage page) {
                for (String line : page.lines) {
                    if (line.contains(expected)) {
                        found.countDown();
                        return;
                    }
                }
                if (page.hasMore && page.startLine > 0) {
                    loadOlderHistory(client, conversationId, token, page.startLine,
                            expected, found, failure);
                }
            }

            @Override public void onError(String message) {
                failure.set(message);
                found.countDown();
            }
        });
    }

    @Test
    public void pagedPlainHistoryStaysWithinOneThousandRows() {
        VtTerminalEmulator terminal = new VtTerminalEmulator(32, 120);
        ArrayList<String> bootstrap = new ArrayList<>();
        for (int index = 800; index < 1000; index++) {
            bootstrap.add("line-" + index);
        }
        terminal.loadPlainSnapshot(bootstrap, 32, 120);
        assertEquals(200, terminal.totalLines());

        for (int page = 3; page >= 0; page--) {
            ArrayList<String> lines = new ArrayList<>();
            for (int index = page * 200; index < (page + 1) * 200; index++) {
                lines.add("line-" + index);
            }
            assertEquals(200, terminal.prependPlainHistory(lines));
        }
        assertEquals(1000, terminal.totalLines());
        assertEquals(0, terminal.prependPlainHistory(
                java.util.Collections.singletonList("too-old")));
    }
}
