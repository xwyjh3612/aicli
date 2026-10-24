package com.aitest.aicli;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.os.SystemClock;
import android.view.View;
import android.widget.EditText;

import androidx.core.content.FileProvider;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import com.google.gson.JsonObject;
import com.google.android.material.button.MaterialButton;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;

import okhttp3.mockwebserver.Dispatcher;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.mockwebserver.RecordedRequest;
import okhttp3.tls.HandshakeCertificates;
import okhttp3.tls.HeldCertificate;

/** Exercises the actual picker result, image processing, upload and composer UI. */
@RunWith(AndroidJUnit4.class)
public final class ComposerInstrumentedTest {
    private static final String COMPUTER_ID = "composer-test-computer";
    private final AtomicInteger uploadCount = new AtomicInteger();
    private final AtomicInteger pickerCount = new AtomicInteger();
    private final AtomicReference<List<Uri>> selection = new AtomicReference<>();
    private final List<Integer> uploadedColors = Collections.synchronizedList(new ArrayList<>());
    private final List<File> imageFiles = new ArrayList<>();
    private final CountDownLatch firstUpload = new CountDownLatch(1);
    private final CountDownLatch releaseFirstUpload = new CountDownLatch(1);
    private volatile boolean delayFirstUpload;
    private volatile int failingUpload;
    private Context context;
    private Map<String, ?> savedConnectionPreferences;
    private MockWebServer server;
    private ActivityScenario<MainActivity> scenario;
    private Instrumentation.ActivityMonitor pickerMonitor;

    @Before
    public void setUp() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        context = instrumentation.getTargetContext();
        HeldCertificate certificate = new HeldCertificate.Builder()
                .commonName("localhost").addSubjectAlternativeName("localhost").build();
        HandshakeCertificates certificates = new HandshakeCertificates.Builder()
                .heldCertificate(certificate).build();
        server = new MockWebServer();
        server.useHttps(certificates.sslSocketFactory(), false);
        server.setDispatcher(new Dispatcher() {
            @Override public MockResponse dispatch(RecordedRequest request) throws InterruptedException {
                String path = request.getPath();
                if (path.startsWith("/api/auth/challenge")) {
                    return json("{\"computerId\":\"" + COMPUTER_ID
                            + "\",\"challengeId\":\"test\",\"nonce\":\"test-nonce\"}");
                }
                if (path.equals("/api/auth/token")) {
                    return json("{\"token\":\"test-token\",\"expiresAt\":"
                            + (System.currentTimeMillis() / 1000L + 3600L) + "}");
                }
                if (path.equals("/api/conversations")) {
                    return json("{\"items\":[{\"id\":\"composer-project\",\"name\":\"图片测试\","
                            + "\"provider\":\"codex\",\"type\":\"Codex\",\"group\":\"默认分组\","
                            + "\"folder\":\"\",\"status\":\"running\"}]}");
                }
                if (path.contains("/attachment?")) {
                    int index = uploadCount.incrementAndGet();
                    byte[] bytes = request.getBody().readByteArray();
                    Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                    uploadedColors.add(bitmap.getPixel(0, 0));
                    bitmap.recycle();
                    if (index == 1) {
                        firstUpload.countDown();
                        if (delayFirstUpload) releaseFirstUpload.await(15, TimeUnit.SECONDS);
                    }
                    if (index == failingUpload) return json("{\"error\":\"test failure\"}").setResponseCode(500);
                    JsonObject response = new JsonObject();
                    response.addProperty("path", imagePath(index));
                    return json(response.toString());
                }
                return json("{}");
            }
        });
        server.start();
        String fingerprint = hex(MessageDigest.getInstance("SHA-256")
                .digest(certificate.certificate().getEncoded()));
        String address = server.url("/").toString();
        ComputerConnectionStore.Computer computer = new ComputerConnectionStore.Computer(
                COMPUTER_ID, "Composer test", address, address, "", fingerprint, 0L);
        savedConnectionPreferences = context.getSharedPreferences("connection", Context.MODE_PRIVATE).getAll();
        new ComputerConnectionStore(context).upsert(computer, true);
        scenario = ActivityScenario.launch(MainActivity.class);
        pickerMonitor = new Instrumentation.ActivityMonitor() {
            @Override public Instrumentation.ActivityResult onStartActivity(Intent intent) {
                if (!Intent.ACTION_GET_CONTENT.equals(intent.getAction())) return null;
                assertEquals("image/*", intent.getType());
                assertTrue("The gallery must allow selecting multiple images",
                        intent.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false));
                List<Uri> uris = selection.get();
                ClipData clips = ClipData.newUri(context.getContentResolver(), "images", uris.get(0));
                for (int index = 1; index < uris.size(); index++) clips.addItem(new ClipData.Item(uris.get(index)));
                pickerCount.incrementAndGet();
                Intent result = new Intent();
                result.setClipData(clips);
                return new Instrumentation.ActivityResult(Activity.RESULT_OK, result);
            }
        };
        instrumentation.addMonitor(pickerMonitor);
        awaitComposer(activity -> activity.findViewById(R.id.inputBar).isShown()
                && activity.findViewById(R.id.messageInput).getWidth() > 0);
    }

    @After
    public void tearDown() throws Exception {
        releaseFirstUpload.countDown();
        if (pickerMonitor != null) InstrumentationRegistry.getInstrumentation().removeMonitor(pickerMonitor);
        if (scenario != null) scenario.close();
        if (server != null) server.shutdown();
        restoreConnectionPreferences();
        if (context != null) {
            context.getSharedPreferences("conversation_cache", Context.MODE_PRIVATE)
                    .edit().remove("items." + COMPUTER_ID).apply();
        }
        for (File file : imageFiles) file.delete();
    }

    @Test
    public void menuStaysOnLeftWhenDraftContainsTextOrImagePaths() {
        scenario.onActivity(activity -> {
            EditText input = activity.findViewById(R.id.messageInput);
            MaterialButton menu = activity.findViewById(R.id.composerMenuButton);
            MaterialButton send = activity.findViewById(R.id.sendButton);
            assertTrue(menu.getRight() <= input.getLeft());
            assertTrue(input.getRight() <= send.getLeft());
            assertFalse(send.isEnabled());
            input.setText("分析这张图片 ");
            menu.performClick();
            assertEquals(View.VISIBLE, activity.findViewById(R.id.terminalKeyBar).getVisibility());
            invoke(activity, "insertUploadedImagePath", String.class, imagePath(1));
            assertEquals(View.VISIBLE, activity.findViewById(R.id.terminalKeyBar).getVisibility());
            assertEquals("返回输入键盘", menu.getContentDescription().toString());
            assertEquals("发送", send.getContentDescription().toString());
            assertTrue(send.isEnabled());
            assertTrue(activity.findViewById(R.id.galleryButton).isShown());
            assertTrue(input.getText().toString().contains(imagePath(1)));
        });
    }

    @Test
    public void multiSelectionAndAnotherSelectionUploadInOrderWithoutLosingTheDraft() throws Exception {
        delayFirstUpload = true;
        Uri red = image(Color.RED);
        Uri blue = image(Color.BLUE);
        Uri green = image(Color.GREEN);
        scenario.onActivity(activity -> ((EditText) activity.findViewById(R.id.messageInput)).setText("一起分析 "));
        choose(red, blue);
        assertTrue("First upload did not reach the desktop", firstUpload.await(15, TimeUnit.SECONDS));
        scenario.onActivity(activity -> {
            assertFalse(activity.findViewById(R.id.sendButton).isEnabled());
            assertEquals(View.VISIBLE, activity.findViewById(R.id.imageUploadStatus).getVisibility());
            assertTrue(activity.findViewById(R.id.composerMenuButton).isEnabled());
        });
        choose(green);
        releaseFirstUpload.countDown();
        awaitComposer(activity -> activity.findViewById(R.id.sendButton).isEnabled()
                && ((EditText) activity.findViewById(R.id.messageInput)).getText().toString().contains(imagePath(3)));
        scenario.onActivity(activity -> {
            String expected = "一起分析 \"" + imagePath(1) + "\" \"" + imagePath(2) + "\" \"" + imagePath(3) + "\" ";
            assertEquals(expected, ((EditText) activity.findViewById(R.id.messageInput)).getText().toString());
            assertEquals(View.GONE, activity.findViewById(R.id.imageUploadStatus).getVisibility());
            assertTrue(activity.findViewById(R.id.galleryButton).isShown());
        });
        assertEquals(2, pickerCount.get());
        assertEquals(3, uploadCount.get());
        assertTrue(Color.red(uploadedColors.get(0)) > 240);
        assertTrue(Color.blue(uploadedColors.get(1)) > 240);
        assertTrue(Color.green(uploadedColors.get(2)) > 240);
    }

    @Test
    public void oneFailedUploadDoesNotBlockTheRemainingImages() throws Exception {
        failingUpload = 2;
        choose(image(Color.RED), image(Color.BLUE), image(Color.GREEN));
        awaitComposer(activity -> activity.findViewById(R.id.sendButton).isEnabled()
                && ((EditText) activity.findViewById(R.id.messageInput)).getText().toString().contains(imagePath(3)));
        scenario.onActivity(activity -> {
            String text = ((EditText) activity.findViewById(R.id.messageInput)).getText().toString();
            assertTrue(text.contains(imagePath(1)));
            assertFalse(text.contains(imagePath(2)));
            assertEquals(View.GONE, activity.findViewById(R.id.imageUploadStatus).getVisibility());
        });
        assertEquals(3, uploadCount.get());
    }

    private void choose(Uri... uris) {
        selection.set(Arrays.asList(uris));
        scenario.onActivity(activity -> {
            if (activity.findViewById(R.id.terminalKeyBar).getVisibility() != View.VISIBLE) {
                activity.findViewById(R.id.composerMenuButton).performClick();
            }
            activity.findViewById(R.id.galleryButton).performClick();
        });
    }

    private Uri image(int color) throws Exception {
        File directory = new File(context.getCacheDir(), "camera");
        directory.mkdirs();
        File file = File.createTempFile("composer-test-", ".png", directory);
        imageFiles.add(file);
        Bitmap bitmap = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(color);
        try (FileOutputStream output = new FileOutputStream(file)) {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, output);
        } finally {
            bitmap.recycle();
        }
        return FileProvider.getUriForFile(context, context.getPackageName() + ".fileprovider", file);
    }

    private void awaitComposer(Predicate<MainActivity> condition) {
        for (int attempt = 0; attempt < 300; attempt++) {
            AtomicBoolean ready = new AtomicBoolean();
            scenario.onActivity(activity -> ready.set(condition.test(activity)));
            if (ready.get()) return;
            SystemClock.sleep(50L);
        }
        fail("Image uploads did not finish within 15 seconds");
    }

    private static MockResponse json(String body) {
        return new MockResponse().setHeader("Content-Type", "application/json").setBody(body);
    }

    private static String imagePath(int index) { return "C:\\images\\image-" + index + ".jpg"; }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte value : bytes) result.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return result.toString();
    }

    @SuppressWarnings("unchecked")
    private void restoreConnectionPreferences() {
        if (savedConnectionPreferences == null) return;
        SharedPreferences.Editor editor = context.getSharedPreferences("connection", Context.MODE_PRIVATE).edit().clear();
        for (Map.Entry<String, ?> entry : savedConnectionPreferences.entrySet()) {
            String key = entry.getKey();
            Object value = entry.getValue();
            if (value instanceof String) editor.putString(key, (String) value);
            else if (value instanceof Boolean) editor.putBoolean(key, (Boolean) value);
            else if (value instanceof Integer) editor.putInt(key, (Integer) value);
            else if (value instanceof Long) editor.putLong(key, (Long) value);
            else if (value instanceof Float) editor.putFloat(key, (Float) value);
            else if (value instanceof Set) editor.putStringSet(key, (Set<String>) value);
        }
        editor.commit();
    }

    private static void invoke(MainActivity activity, String name, Class<?> parameter, Object value) {
        try {
            Method method = parameter == null ? MainActivity.class.getDeclaredMethod(name)
                    : MainActivity.class.getDeclaredMethod(name, parameter);
            method.setAccessible(true);
            if (parameter == null) method.invoke(activity); else method.invoke(activity, value);
        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }
}
