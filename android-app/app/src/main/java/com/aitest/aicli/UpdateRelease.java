package com.aitest.aicli;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

/** Strict public release metadata, independent of Android for unit testing. */
final class UpdateRelease {
    static final String MANIFEST_URL =
            "https://github.com/xwyjh3612/aicli/releases/latest/download/update-manifest.json";
    final String version;
    final long versionCode;
    final String url;
    final String sha256;
    final long size;

    private UpdateRelease(String version, long versionCode, String url, String sha256, long size) {
        this.version = version;
        this.versionCode = versionCode;
        this.url = url;
        this.sha256 = sha256;
        this.size = size;
    }

    boolean isNewerThan(long installedCode) { return versionCode > installedCode; }

    static UpdateRelease parse(String text) {
        JsonObject manifest = JsonParser.parseString(text).getAsJsonObject();
        if (integer(manifest, "schemaVersion") != 1) throw new IllegalArgumentException("Unsupported manifest");
        JsonObject value = manifest.getAsJsonObject("android");
        String version = string(value, "version");
        long code = integer(value, "versionCode");
        String url = string(value, "url");
        String sha256 = string(value, "sha256");
        long size = integer(value, "size");
        if (!version.matches("(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})") ||
                code <= 0 || code > 2100000000 || size <= 0 || size > 512L * 1024 * 1024 ||
                !sha256.matches("[a-f0-9]{64}") ||
                !url.matches("https://github\\.com/xwyjh3612/aicli/releases/download/[A-Za-z0-9_-][A-Za-z0-9._-]*/AI-CLI-Android-[0-9.]+-release\\.apk") ||
                !url.endsWith("/AI-CLI-Android-" + version + "-release.apk"))
            throw new IllegalArgumentException("Invalid release asset");
        return new UpdateRelease(version, code, url, sha256, size);
    }

    private static String string(JsonObject object, String name) {
        if (!object.get(name).isJsonPrimitive() || !object.getAsJsonPrimitive(name).isString())
            throw new IllegalArgumentException("Expected string");
        return object.get(name).getAsString();
    }

    private static long integer(JsonObject object, String name) {
        if (!object.get(name).isJsonPrimitive() || !object.getAsJsonPrimitive(name).isNumber() ||
                !object.get(name).getAsString().matches("0|[1-9][0-9]*"))
            throw new IllegalArgumentException("Expected unsigned integer");
        return Long.parseLong(object.get(name).getAsString());
    }
}
