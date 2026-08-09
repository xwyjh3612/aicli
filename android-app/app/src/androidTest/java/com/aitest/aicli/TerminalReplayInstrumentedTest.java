package com.aitest.aicli;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.os.Bundle;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

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
        client.connectEvents(claude.id, new AicliClient.EventCallback() {
            @Override public void onEvent(String eventJson) {
                try {
                    JsonObject event = JsonParser.parseString(eventJson).getAsJsonObject();
                    if (event.has("terminalSessionId")) {
                        terminalSession.set(event.get("terminalSessionId").getAsString());
                    }
                    if (!event.has("type") ||
                            !"terminal.data".equals(event.get("type").getAsString())) return;
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
        assertTrue("retained Claude stream did not contain '" + expected
                        + "'; frames=" + dataFrames.get()
                        + ", maxSequence=" + maximumSequence.get()
                        + ", session=" + terminalSession.get(),
                received);
    }
}
