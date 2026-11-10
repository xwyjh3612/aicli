package com.aitest.aicli;

import android.content.Context;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.google.gson.JsonObject;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import okhttp3.OkHttpClient;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.tls.HandshakeCertificates;
import okhttp3.tls.HeldCertificate;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class AppUpdaterInstrumentedTest {
    private MockWebServer server;
    private Context context;
    private AppUpdater updater;

    @Before public void setup() throws Exception {
        context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        HeldCertificate certificate = new HeldCertificate.Builder().commonName("localhost")
                .addSubjectAlternativeName("localhost").build();
        HandshakeCertificates host = new HandshakeCertificates.Builder().heldCertificate(certificate).build();
        HandshakeCertificates trust = new HandshakeCertificates.Builder().addTrustedCertificate(certificate.certificate()).build();
        server = new MockWebServer();
        server.useHttps(host.sslSocketFactory(), false);
        server.start();
        OkHttpClient client = new OkHttpClient.Builder().sslSocketFactory(trust.sslSocketFactory(), trust.trustManager())
                .followSslRedirects(false).callTimeout(5, TimeUnit.SECONDS).build();
        updater = new AppUpdater(context, client, server.url("/manifest").toString());
    }

    @After public void teardown() throws Exception { server.shutdown(); }

    @Test public void httpsResponseIsReadAndNetworkFailuresAreRejected() throws Exception {
        server.enqueue(new MockResponse().setBody("update data"));
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        updater.fetch(server.url("/asset").toString(), 100, output::write);
        assertEquals("update data", output.toString("UTF-8"));
        server.enqueue(new MockResponse().setResponseCode(503));
        assertThrows(IOException.class, () -> updater.fetch(server.url("/asset").toString(), 100, output::write));
        server.enqueue(new MockResponse().setChunkedBody(new String(new char[200]).replace('\0', 'x'), 10));
        assertThrows(IOException.class, () -> updater.fetch(server.url("/asset").toString(), 100, output::write));
    }

    @Test public void untrustedCertificatesAndHttpDowngradesAreRejected() {
        AppUpdater productionTrust = new AppUpdater(context);
        assertThrows(IOException.class, () -> productionTrust.fetch(server.url("/asset").toString(), 100, (b, o, n) -> { }));
        server.enqueue(new MockResponse().setResponseCode(302).setHeader("Location", "http://localhost:1/insecure"));
        assertThrows(IOException.class, () -> updater.fetch(server.url("/asset").toString(), 100, (b, o, n) -> { }));
    }

    @Test public void corruptedAndRelabelledApksCannotBeInstalled() throws Exception {
        File installedApk = new File(context.getApplicationInfo().sourceDir);
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(installedApk)) {
            byte[] buffer = new byte[65536]; int count;
            while ((count = input.read(buffer)) != -1) sha.update(buffer, 0, count);
        }
        StringBuilder digest = new StringBuilder();
        for (byte value : sha.digest()) digest.append(String.format(Locale.ROOT, "%02x", value & 255));
        JsonObject metadata = new JsonObject();
        metadata.addProperty("version", "9.9.9");
        metadata.addProperty("versionCode", 999);
        metadata.addProperty("size", installedApk.length());
        metadata.addProperty("sha256", new String(new char[64]).replace('\0', '0'));
        metadata.addProperty("url", "https://github.com/xwyjh3612/aicli/releases/download/test/AI-CLI-Android-9.9.9-release.apk");
        JsonObject manifest = new JsonObject(); manifest.addProperty("schemaVersion", 1); manifest.add("android", metadata);
        assertThrows(IOException.class, () -> updater.verify(installedApk, UpdateRelease.parse(manifest.toString())));
        metadata.addProperty("sha256", digest.toString());
        // Even with a correct digest, relabelling an old APK as a new one fails.
        assertThrows(IOException.class, () -> updater.verify(installedApk, UpdateRelease.parse(manifest.toString())));
    }
}
