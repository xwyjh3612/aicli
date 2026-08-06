package com.aitest.aicli;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.os.Bundle;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;
import android.util.Base64;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** End-to-end test run against a desktop-generated pairing QR. */
@RunWith(AndroidJUnit4.class)
public final class PairingInstrumentedTest {
    @Test
    public void pairAuthenticateAndListConversations() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        String qrPayload = arguments.getString("qrPayload", "");
        if (qrPayload.isEmpty()) {
            String encoded = arguments.getString("qrPayloadBase64", "");
            if (!encoded.isEmpty()) {
                qrPayload = new String(Base64.decode(encoded, Base64.DEFAULT),
                        java.nio.charset.StandardCharsets.UTF_8);
            }
        }
        assertFalse("qrPayload instrumentation argument is required", qrPayload.isEmpty());

        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        AicliClient pairingClient = new AicliClient(context, "");
        CountDownLatch pairing = new CountDownLatch(1);
        AtomicReference<String> failure = new AtomicReference<>();
        pairingClient.pairFromQr(qrPayload, new AicliClient.PairingCallback() {
            @Override public void onStatus(String status, String message) { }
            @Override public void onComplete() { pairing.countDown(); }
            @Override public void onError(String message) { failure.set(message); pairing.countDown(); }
        });
        assertTrue("pairing timed out", pairing.await(180, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());

        // Simulate leaving the pairing screen, losing the short-lived token,
        // and reopening the app.  The replacement client must restore the
        // pinned address/certificate and authenticate with the same Keystore
        // private key without another desktop confirmation.
        pairingClient.close();
        AicliClient client = new AicliClient(context, "");

        CountDownLatch conversations = new CountDownLatch(1);
        AtomicReference<List<AicliClient.Conversation>> values = new AtomicReference<>();
        client.listConversations(null, new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> result) {
                values.set(result);
                conversations.countDown();
            }
            @Override public void onError(String message) {
                failure.set(message);
                conversations.countDown();
            }
        });
        assertTrue("conversation request timed out", conversations.await(30, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());
        assertTrue("desktop returned no conversations", values.get() != null && !values.get().isEmpty());

        AicliClient.Conversation selected = values.get().get(0);
        for (AicliClient.Conversation value : values.get()) {
            if ("running".equalsIgnoreCase(value.status)) {
                selected = value;
                break;
            }
        }
        final AicliClient.Conversation chosen = selected;

        CountDownLatch history = new CountDownLatch(1);
        client.listEvents(chosen.id, 0, new AicliClient.HistoryCallback() {
            @Override public void onSuccess(List<String> events) { history.countDown(); }
            @Override public void onError(String message) { failure.set(message); history.countDown(); }
        });
        assertTrue("event history request timed out", history.await(30, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());

        CountDownLatch websocketHello = new CountDownLatch(1);
        CountDownLatch echoedInput = new CountDownLatch(1);
        client.connectEvents(chosen.id, new AicliClient.EventCallback() {
            @Override public void onEvent(String eventJson) {
                if (eventJson.contains("\"type\":\"hello\"")) websocketHello.countDown();
                if (eventJson.contains("terminal.input")) echoedInput.countDown();
            }
            @Override public void onClosed() { }
        });
        assertTrue("authenticated WebSocket did not become ready",
                websocketHello.await(20, TimeUnit.SECONDS));

        CountDownLatch input = new CountDownLatch(1);
        client.sendInput(chosen.id, "\u000c", new AicliClient.ActionCallback() {
            @Override public void onSuccess() { input.countDown(); }
            @Override public void onError(String message) { failure.set(message); input.countDown(); }
        });
        assertTrue("remote input request timed out", input.await(30, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());
        assertTrue("desktop did not broadcast the accepted PTY input",
                echoedInput.await(15, TimeUnit.SECONDS));
        client.close();
    }

    @Test
    public void revokedDeviceCannotReconnect() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        AicliClient client = new AicliClient(context, "");
        CountDownLatch request = new CountDownLatch(1);
        AtomicReference<String> failure = new AtomicReference<>();
        AtomicReference<Boolean> accepted = new AtomicReference<>(false);
        client.listConversations(null, new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> result) {
                accepted.set(true);
                request.countDown();
            }

            @Override public void onError(String message) {
                failure.set(message);
                request.countDown();
            }
        });
        assertTrue("revocation check timed out", request.await(30, TimeUnit.SECONDS));
        assertFalse("revoked device still accessed the desktop", accepted.get());
        assertTrue("desktop did not explain that the device was revoked: " + failure.get(),
                failure.get() != null && (failure.get().contains("设备未绑定")
                        || failure.get().contains("设备已解绑")
                        || failure.get().contains("HTTP 401")));
        client.close();
    }
}
