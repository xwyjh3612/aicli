package com.aitest.aicli;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.os.Bundle;
import android.util.Base64;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** End-to-end pairing check that does not write anything to a terminal session. */
@RunWith(AndroidJUnit4.class)
public final class MultiComputerPairingInstrumentedTest {
    @Test
    public void pairStoreAndReconnect() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        String qrPayload = arguments.getString("qrPayload", "");
        if (qrPayload.isEmpty()) {
            String encoded = arguments.getString("qrPayloadBase64", "");
            if (!encoded.isEmpty()) {
                qrPayload = new String(Base64.decode(encoded, Base64.DEFAULT),
                        StandardCharsets.UTF_8);
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
            @Override public void onError(String message) {
                failure.set(message);
                pairing.countDown();
            }
        });
        assertTrue("pairing timed out", pairing.await(180, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());
        String pairedComputerId = pairingClient.computerId();
        assertFalse("desktop computer id is missing", pairedComputerId.isEmpty());
        pairingClient.close();

        ComputerConnectionStore store = new ComputerConnectionStore(context);
        ComputerConnectionStore.Computer active = store.active();
        assertTrue("paired desktop was not stored", active != null);
        assertEquals("paired desktop is not active", pairedComputerId, active.computerId);

        AicliClient client = new AicliClient(context, "");
        CountDownLatch conversations = new CountDownLatch(1);
        AtomicReference<List<AicliClient.Conversation>> values = new AtomicReference<>();
        client.listConversations(new AicliClient.ConversationCallback() {
            @Override public void onSuccess(List<AicliClient.Conversation> result) {
                values.set(result);
                conversations.countDown();
            }
            @Override public void onError(String message) {
                failure.set(message);
                conversations.countDown();
            }
        });
        assertTrue("conversation request timed out",
                conversations.await(30, TimeUnit.SECONDS));
        assertNull(failure.get(), failure.get());
        assertTrue("desktop did not return a conversation list", values.get() != null);
        client.close();
    }
}
