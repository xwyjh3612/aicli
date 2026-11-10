package com.aitest.aicli;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.junit.Test;
import static org.junit.Assert.*;

public class UpdateReleaseTest {
    private JsonObject manifest() {
        return JsonParser.parseString("{\"schemaVersion\":1,\"android\":{\"version\":\"1.3.0\",\"versionCode\":10," +
                "\"size\":1024,\"sha256\":\"" + "a".repeat(64) + "\"," +
                "\"url\":\"https://github.com/xwyjh3612/aicli/releases/download/v1.3.0/AI-CLI-Android-1.3.0-release.apk\"}}")
                .getAsJsonObject();
    }
    @Test public void versionCodeControlsUpgradeAndRejectsDowngrade() {
        UpdateRelease release = UpdateRelease.parse(manifest().toString());
        assertTrue(release.isNewerThan(9));
        assertFalse(release.isNewerThan(10));
        assertFalse(release.isNewerThan(11));
    }
    @Test public void untrustedOrMismatchedUrlsAreRejected() {
        for (String url : new String[]{
                "http://github.com/xwyjh3612/aicli/releases/download/v1/AI-CLI-Android-1.3.0-release.apk",
                "https://evil.example/AI-CLI-Android-1.3.0-release.apk",
                "https://github.com/other/repo/releases/download/v1/AI-CLI-Android-1.3.0-release.apk",
                "https://github.com/xwyjh3612/aicli/releases/download/v1/AI-CLI-Android-1.2.0-release.apk"}) {
            JsonObject value = manifest(); value.getAsJsonObject("android").addProperty("url", url);
            assertThrows(RuntimeException.class, () -> UpdateRelease.parse(value.toString()));
        }
    }
    @Test public void malformedMetadataCannotTriggerDownload() {
        for (String text : new String[]{"{}", "not json", manifest().toString().replace("schemaVersion\":1", "schemaVersion\":2")})
            assertThrows(RuntimeException.class, () -> UpdateRelease.parse(text));
        for (long size : new long[]{-1, 0, 536870913L}) {
            JsonObject value = manifest(); value.getAsJsonObject("android").addProperty("size", size);
            assertThrows(RuntimeException.class, () -> UpdateRelease.parse(value.toString()));
        }
        for (String key : new String[]{"versionCode", "size"}) {
            JsonObject value = manifest(); value.getAsJsonObject("android").addProperty(key, 1.5);
            assertThrows(RuntimeException.class, () -> UpdateRelease.parse(value.toString()));
        }
        JsonObject value = manifest(); value.getAsJsonObject("android").addProperty("sha256", "invalid");
        assertThrows(RuntimeException.class, () -> UpdateRelease.parse(value.toString()));
    }
}
