package com.aitest.aicli;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;

import androidx.appcompat.app.AlertDialog;
import androidx.core.content.FileProvider;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import okhttp3.Call;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;

/** Main-thread state machine; I/O uses a dedicated client with normal HTTPS trust. */
final class AppUpdater {
    private enum State { IDLE, CHECKING, AVAILABLE, DOWNLOADING, READY, PERMISSION, VERIFYING, INSTALL, ERROR, LATEST }
    private static final long INTERVAL_MS = TimeUnit.HOURS.toMillis(6);
    private final Context context;
    private final SharedPreferences preferences;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    // Never reuse the paired desktop client's certificate/pinning configuration.
    private final OkHttpClient http;
    private final String manifestUrl;
    private final AtomicBoolean cancelled = new AtomicBoolean();
    private volatile Call call;
    private volatile int progress;
    private Activity activity;
    private AlertDialog dialog;
    private State state = State.IDLE;
    private UpdateRelease release;
    private File apk;
    private boolean manual;
    private String error;
    private long nextAttempt;
    private final Runnable refreshProgress = new Runnable() {
        @Override public void run() {
            if (dialog != null && state == State.DOWNLOADING) {
                dialog.setMessage("正在下载 " + progress + "%\n下载完成后会校验安装包。");
                handler.postDelayed(this, 300);
            }
        }
    };

    AppUpdater(Context context) {
        this(context, new OkHttpClient.Builder()
                .connectTimeout(15, TimeUnit.SECONDS).readTimeout(30, TimeUnit.SECONDS)
                .callTimeout(20, TimeUnit.MINUTES).followSslRedirects(false).build(), UpdateRelease.MANIFEST_URL);
    }

    // Package-private dependency injection for transport tests; the application
    // always uses the constructor above and the fixed public release endpoint.
    AppUpdater(Context context, OkHttpClient http, String manifestUrl) {
        this.context = context.getApplicationContext();
        this.http = http;
        this.manifestUrl = manifestUrl;
        preferences = context.getSharedPreferences("app_updates", Context.MODE_PRIVATE);
    }

    void resume(Activity current) {
        activity = current;
        if (state == State.PERMISSION) {
            if (context.getPackageManager().canRequestPackageInstalls()) verifyForInstall();
            else state = State.READY;
        }
        present();
        handler.postDelayed(() -> {
            if (activity != null && state == State.IDLE && preferences.getBoolean("automatic", true) &&
                    System.currentTimeMillis() >= nextAttempt &&
                    System.currentTimeMillis() - preferences.getLong("last_success", 0) >= INTERVAL_MS)
                check(false);
        }, 2500);
    }

    void pause(Activity previous) {
        if (activity == previous) {
            dismiss();
            activity = null;
        }
    }

    boolean automatic() { return preferences.getBoolean("automatic", true); }
    void setAutomatic(boolean enabled) { preferences.edit().putBoolean("automatic", enabled).apply(); }

    void check(boolean requested) {
        if (state != State.IDLE && state != State.AVAILABLE && state != State.READY &&
                state != State.ERROR && state != State.LATEST) {
            if (requested) { manual = true; present(); }
            return;
        }
        dismiss();
        state = State.CHECKING;
        manual = requested;
        cancelled.set(false);
        nextAttempt = System.currentTimeMillis() + TimeUnit.MINUTES.toMillis(15);
        present();
        executor.execute(() -> {
            try {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                fetch(manifestUrl, 64 * 1024, bytes::write);
                UpdateRelease found = UpdateRelease.parse(bytes.toString(StandardCharsets.UTF_8.name()));
                handler.post(() -> {
                    dismiss();
                    if (cancelled.get()) { state = State.IDLE; return; }
                    preferences.edit().putLong("last_success", System.currentTimeMillis()).apply();
                    release = found;
                    state = found.isNewerThan(BuildConfig.VERSION_CODE) ? State.AVAILABLE : (manual ? State.LATEST : State.IDLE);
                    present();
                });
            } catch (Exception failure) { fail("无法检查更新，请检查网络后重试。", requested); }
        });
    }

    interface Sink { void write(byte[] buffer, int offset, int length) throws IOException; }

    void fetch(String url, long limit, Sink sink) throws IOException {
        if (cancelled.get()) throw new IOException("Cancelled");
        Call active = http.newCall(new Request.Builder().url(url).header("Cache-Control", "no-cache").build());
        if (limit == 64 * 1024) active.timeout().timeout(45, TimeUnit.SECONDS);
        call = active;
        if (cancelled.get()) active.cancel();
        try (Response response = active.execute()) {
            if (!response.isSuccessful() || response.body() == null || !response.request().url().isHttps())
                throw new IOException("Update server unavailable");
            if (response.body().contentLength() > limit) throw new IOException("Response too large");
            try (InputStream input = response.body().byteStream()) {
                byte[] buffer = new byte[64 * 1024];
                long total = 0;
                int count;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (cancelled.get() || total > limit) throw new IOException("Cancelled or oversized response");
                    sink.write(buffer, 0, count);
                    progress = (int) (total * 100 / limit);
                }
            }
        } finally { call = null; }
    }

    private void download() {
        dismiss();
        state = State.DOWNLOADING;
        cancelled.set(false);
        progress = 0;
        UpdateRelease selected = release;
        present();
        executor.execute(() -> {
            File directory = new File(context.getFilesDir(), "updates");
            File partial = new File(directory, "update.part");
            File destination = new File(directory, "update-" + selected.versionCode + ".apk");
            try {
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create update folder");
                File[] stale = directory.listFiles();
                if (stale != null) for (File file : stale)
                    if (file.isFile() && (file.getName().equals("update.part") || file.getName().matches("update-[0-9]+\\.apk"))) file.delete();
                try (FileOutputStream output = new FileOutputStream(partial)) {
                    fetch(selected.url, selected.size, output::write);
                }
                verify(partial, selected);
                if (cancelled.get() || !partial.renameTo(destination)) throw new IOException("Cannot finalize update");
                handler.post(() -> {
                    dismiss();
                    if (cancelled.get()) { destination.delete(); state = State.IDLE; return; }
                    apk = destination;
                    state = State.READY;
                    present();
                });
            } catch (Exception failure) {
                partial.delete();
                fail("更新下载或校验失败，未启动安装。请稍后重试。", true);
            }
        });
    }

    @SuppressWarnings("deprecation")
    void verify(File file, UpdateRelease selected) throws Exception {
        if (file.length() != selected.size) throw new IOException("APK size mismatch");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (cancelled.get()) throw new IOException("Cancelled");
                digest.update(buffer, 0, count);
            }
        }
        StringBuilder hex = new StringBuilder();
        for (byte value : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        if (!hex.toString().equals(selected.sha256)) throw new IOException("APK checksum mismatch");
        PackageManager packages = context.getPackageManager();
        int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
        PackageInfo archive = packages.getPackageArchiveInfo(file.getAbsolutePath(), flags);
        PackageInfo installed = packages.getPackageInfo(context.getPackageName(), flags);
        if (archive == null || !context.getPackageName().equals(archive.packageName) ||
                !selected.version.equals(archive.versionName) ||
                (Build.VERSION.SDK_INT >= 28 ? archive.getLongVersionCode() : archive.versionCode) != selected.versionCode ||
                !selected.isNewerThan(Build.VERSION.SDK_INT >= 28 ? installed.getLongVersionCode() : installed.versionCode))
            throw new IOException("APK identity or version mismatch");
        Signature[] expected = Build.VERSION.SDK_INT >= 28 ? installed.signingInfo.getApkContentsSigners() : installed.signatures;
        Signature[] actual = Build.VERSION.SDK_INT >= 28 ? archive.signingInfo.getApkContentsSigners() : archive.signatures;
        if (expected == null || actual == null || expected.length == 0 ||
                !new HashSet<>(Arrays.asList(expected)).equals(new HashSet<>(Arrays.asList(actual))))
            throw new IOException("APK signing key mismatch");
    }

    private void verifyForInstall() {
        dismiss();
        state = State.VERIFYING;
        cancelled.set(false);
        File selectedApk = apk;
        UpdateRelease selected = release;
        present();
        executor.execute(() -> {
            try {
                verify(selectedApk, selected);
                handler.post(() -> { dismiss(); state = cancelled.get() ? State.READY : State.INSTALL; present(); });
            } catch (Exception failure) { fail("安装包校验失败，请重新检查并下载更新。", true); }
        });
    }

    private void install() {
        if (activity == null) return;
        dismiss();
        try {
            if (!context.getPackageManager().canRequestPackageInstalls()) {
                state = State.PERMISSION;
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + context.getPackageName())));
                return;
            }
            Uri uri = FileProvider.getUriForFile(context, context.getPackageName() + ".fileprovider", apk);
            Intent intent = new Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive")
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            state = State.IDLE;
            activity.startActivity(intent);
        } catch (Exception failure) {
            error = "无法打开安装页面，请稍后重试。";
            state = State.ERROR;
            present();
        }
    }

    private void fail(String message, boolean visible) {
        handler.post(() -> {
            dismiss();
            if (cancelled.get()) { state = State.IDLE; return; }
            error = message;
            state = (visible || manual) ? State.ERROR : State.IDLE;
            present();
        });
    }

    private void cancel() {
        cancelled.set(true);
        Call active = call;
        if (active != null) active.cancel();
        dismiss();
        // The worker owns completion; do not admit a second job before it exits.
    }

    private void dismiss() {
        handler.removeCallbacks(refreshProgress);
        if (dialog != null) { dialog.dismiss(); dialog = null; }
    }

    private void present() {
        if (activity == null || activity.isFinishing() || activity.isDestroyed() || dialog != null) return;
        if (state == State.INSTALL) { install(); return; }
        MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(activity).setTitle("应用更新");
        switch (state) {
            case CHECKING:
                if (!manual) return;
                builder.setMessage("正在检查更新…").setNegativeButton("取消", (d, w) -> cancel());
                break;
            case AVAILABLE:
                builder.setMessage("发现新版本 " + release.version + "（当前 " + BuildConfig.VERSION_NAME + "）。\n\n下载大小约 " +
                        ((release.size + 1024 * 1024 - 1) / (1024 * 1024)) + " MB，是否下载更新？")
                        .setPositiveButton("下载更新", (d, w) -> download())
                        .setNegativeButton("稍后", (d, w) -> { state = State.IDLE; dismiss(); });
                break;
            case DOWNLOADING:
                builder.setMessage("正在下载 " + progress + "%").setNegativeButton("取消", (d, w) -> cancel());
                break;
            case READY:
                builder.setMessage("新版本已下载并通过完整性与签名校验。安装时可能需要允许此应用安装更新。")
                        .setPositiveButton("安装更新", (d, w) -> verifyForInstall())
                        .setNegativeButton("稍后", (d, w) -> { state = State.IDLE; dismiss(); });
                break;
            case VERIFYING:
                builder.setMessage("正在校验安装包…").setNegativeButton("取消", (d, w) -> cancel());
                break;
            case ERROR:
                builder.setMessage(error).setPositiveButton("知道了", (d, w) -> { state = State.IDLE; dismiss(); });
                break;
            case LATEST:
                builder.setMessage("当前已是最新版本。").setPositiveButton("知道了", (d, w) -> { state = State.IDLE; dismiss(); });
                break;
            default: return;
        }
        dialog = builder.setCancelable(false).create();
        dialog.show();
        if (state == State.DOWNLOADING) handler.post(refreshProgress);
    }
}
