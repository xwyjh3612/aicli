package com.aitest.aicli;

import android.content.Context;
import android.content.SharedPreferences;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.net.URI;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Stores every authorized desktop while exposing one active connection. */
public final class ComputerConnectionStore {
    public static final String ROUTE_LOCAL = "local";
    public static final String ROUTE_REMOTE = "remote";

    private static final Object LOCK = new Object();
    private static final String PREFS = "connection";
    private static final String COMPUTERS_KEY = "computers.v1";
    private static final String ACTIVE_KEY = "activeComputerId";
    private static final String MIGRATED_KEY = "multiComputerMigrated.v1";

    public static final class Computer {
        public final String computerId;
        public final String name;
        public final String remark;
        public final String baseUrl;
        public final String localUrl;
        public final String remoteUrl;
        public final String routePreference;
        public final String certificateFingerprint;
        public final long pairedAt;

        public Computer(String computerId, String name, String baseUrl,
                        String localUrl, String remoteUrl,
                        String certificateFingerprint, long pairedAt) {
            this(computerId, name, "", baseUrl, localUrl, remoteUrl,
                    ROUTE_LOCAL, certificateFingerprint, pairedAt);
        }

        public Computer(String computerId, String name, String remark, String baseUrl,
                        String localUrl, String remoteUrl,
                        String certificateFingerprint, long pairedAt) {
            this(computerId, name, remark, baseUrl, localUrl, remoteUrl,
                    ROUTE_LOCAL, certificateFingerprint, pairedAt);
        }

        public Computer(String computerId, String name, String remark, String baseUrl,
                        String localUrl, String remoteUrl, String routePreference,
                        String certificateFingerprint, long pairedAt) {
            this.computerId = clean(computerId);
            this.name = clean(name);
            this.remark = clean(remark);
            this.baseUrl = normalizeAddress(baseUrl);
            this.localUrl = normalizeAddress(localUrl);
            this.remoteUrl = normalizeAddress(remoteUrl);
            this.routePreference = normalizeRoutePreference(routePreference);
            this.certificateFingerprint = normalizeFingerprint(certificateFingerprint);
            this.pairedAt = pairedAt;
        }

        public String displayName() {
            if (!remark.isEmpty()) return remark;
            if (!name.isEmpty()) return name;
            String host = addressHost(!baseUrl.isEmpty() ? baseUrl
                    : !localUrl.isEmpty() ? localUrl : remoteUrl);
            if (!host.isEmpty()) return "电脑 " + host;
            if (computerId.length() > 12) {
                return "电脑 " + computerId.substring(computerId.length() - 8);
            }
            return computerId.isEmpty() ? "未命名电脑" : computerId;
        }

        public String connectionKey() {
            return computerId + "\n" + baseUrl + "\n" + localUrl + "\n"
                    + remoteUrl + "\n" + routePreference + "\n" + certificateFingerprint;
        }

        public Computer withBaseUrl(String value) {
            return new Computer(computerId, name, remark, value, localUrl, remoteUrl,
                    routePreference, certificateFingerprint, pairedAt);
        }

        public Computer withRemark(String value) {
            return new Computer(computerId, name, value, baseUrl, localUrl, remoteUrl,
                    routePreference, certificateFingerprint, pairedAt);
        }

        public Computer withName(String value) {
            return new Computer(computerId, value, remark, baseUrl, localUrl, remoteUrl,
                    routePreference, certificateFingerprint, pairedAt);
        }

        public Computer withAddresses(String local, String remote) {
            return withConnectionSettings(local, remote, routePreference);
        }

        public Computer withConnectionSettings(String local, String remote, String preference) {
            String normalizedLocal = normalizeAddress(local);
            String normalizedRemote = normalizeAddress(remote);
            String normalizedPreference = normalizeRoutePreference(preference);
            String lastSuccessful = normalizeAddress(baseUrl);
            if (!lastSuccessful.equals(normalizedLocal) &&
                    !lastSuccessful.equals(normalizedRemote)) {
                lastSuccessful = preferredAddress(
                        normalizedLocal, normalizedRemote, normalizedPreference);
            }
            return new Computer(computerId, name, remark, lastSuccessful,
                    normalizedLocal, normalizedRemote, normalizedPreference,
                    certificateFingerprint, pairedAt);
        }

        public boolean remotePreferred() { return ROUTE_REMOTE.equals(routePreference); }
    }

    private final SharedPreferences preferences;

    public ComputerConnectionStore(Context context) {
        preferences = context.getApplicationContext()
                .getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        migrateLegacyConnection();
    }

    public List<Computer> all() {
        synchronized (LOCK) {
            List<Computer> result = readComputers();
            result.sort((left, right) -> Long.compare(right.pairedAt, left.pairedAt));
            return Collections.unmodifiableList(result);
        }
    }

    public Computer active() {
        synchronized (LOCK) {
            List<Computer> computers = readComputers();
            if (computers.isEmpty()) return null;
            String activeId = preferences.getString(ACTIVE_KEY, "");
            for (Computer computer : computers) {
                if (computer.computerId.equals(activeId)) return computer;
            }
            Computer fallback = computers.get(0);
            preferences.edit().putString(ACTIVE_KEY, fallback.computerId).apply();
            mirrorActive(fallback);
            return fallback;
        }
    }

    public boolean activate(String computerId) {
        synchronized (LOCK) {
            for (Computer computer : readComputers()) {
                if (!computer.computerId.equals(computerId)) continue;
                preferences.edit().putString(ACTIVE_KEY, computer.computerId).apply();
                mirrorActive(computer);
                return true;
            }
            return false;
        }
    }

    public void upsert(Computer computer, boolean makeActive) {
        if (computer == null || computer.computerId.isEmpty()) return;
        synchronized (LOCK) {
            List<Computer> computers = readComputers();
            boolean replaced = false;
            for (int index = 0; index < computers.size(); index++) {
                Computer existing = computers.get(index);
                if (!existing.computerId.equals(computer.computerId)) continue;
                String name = computer.name.isEmpty() ? existing.name : computer.name;
                String remark = computer.remark.isEmpty() ? existing.remark : computer.remark;
                computers.set(index, new Computer(
                        computer.computerId, name, remark, computer.baseUrl,
                        computer.localUrl, computer.remoteUrl,
                        existing.routePreference,
                        computer.certificateFingerprint,
                        computer.pairedAt > 0 ? computer.pairedAt : existing.pairedAt));
                computer = computers.get(index);
                replaced = true;
                break;
            }
            if (!replaced) computers.add(computer);
            writeComputers(computers);
            if (makeActive || preferences.getString(ACTIVE_KEY, "").isEmpty()) {
                preferences.edit().putString(ACTIVE_KEY, computer.computerId).apply();
                mirrorActive(computer);
            }
        }
    }

    public boolean updateActiveBaseUrl(String value) {
        synchronized (LOCK) {
            Computer current = active();
            if (current == null) return false;
            return updateBaseUrl(current.computerId, value);
        }
    }

    public boolean updateBaseUrl(String computerId, String value) {
        String targetId = clean(computerId);
        String normalizedValue = normalizeAddress(value);
        if (targetId.isEmpty() || normalizedValue.isEmpty()) return false;
        synchronized (LOCK) {
            List<Computer> computers = readComputers();
            for (int index = 0; index < computers.size(); index++) {
                Computer current = computers.get(index);
                if (!current.computerId.equals(targetId)) continue;
                if (!normalizedValue.equals(current.localUrl) &&
                        !normalizedValue.equals(current.remoteUrl)) return false;
                if (normalizedValue.equals(current.baseUrl)) return true;
                Computer updated = current.withBaseUrl(normalizedValue);
                computers.set(index, updated);
                writeComputers(computers);
                if (targetId.equals(preferences.getString(ACTIVE_KEY, ""))) {
                    mirrorActive(updated);
                }
                return true;
            }
            return false;
        }
    }

    public Computer removeActive() {
        Computer current = active();
        return current == null ? null : remove(current.computerId);
    }

    public boolean updateRemark(String computerId, String value) {
        synchronized (LOCK) {
            List<Computer> computers = readComputers();
            for (int index = 0; index < computers.size(); index++) {
                Computer computer = computers.get(index);
                if (!computer.computerId.equals(clean(computerId))) continue;
                computers.set(index, computer.withRemark(value));
                writeComputers(computers);
                return true;
            }
            return false;
        }
    }

    public boolean updateName(String computerId, String value) {
        String updatedName = clean(value);
        if (updatedName.isEmpty()) return false;
        synchronized (LOCK) {
            List<Computer> computers = readComputers();
            for (int index = 0; index < computers.size(); index++) {
                Computer computer = computers.get(index);
                if (!computer.computerId.equals(clean(computerId))) continue;
                computers.set(index, computer.withName(updatedName));
                writeComputers(computers);
                return true;
            }
            return false;
        }
    }

    public boolean updateAddresses(String computerId, String localUrl, String remoteUrl) {
        Computer existing = find(computerId);
        String preference = existing == null ? ROUTE_LOCAL : existing.routePreference;
        return updateConnectionSettings(computerId, localUrl, remoteUrl, preference);
    }

    public boolean updateConnectionSettings(String computerId, String localUrl,
                                            String remoteUrl, String routePreference) {
        String targetId = clean(computerId);
        String normalizedLocal = normalizeAddress(localUrl);
        String normalizedRemote = normalizeAddress(remoteUrl);
        if (targetId.isEmpty() || (normalizedLocal.isEmpty() && normalizedRemote.isEmpty())) {
            return false;
        }
        synchronized (LOCK) {
            List<Computer> computers = readComputers();
            for (int index = 0; index < computers.size(); index++) {
                Computer computer = computers.get(index);
                if (!computer.computerId.equals(targetId)) continue;
                Computer updated = computer.withConnectionSettings(
                        normalizedLocal, normalizedRemote, routePreference);
                computers.set(index, updated);
                writeComputers(computers);
                if (targetId.equals(preferences.getString(ACTIVE_KEY, ""))) {
                    mirrorActive(updated);
                }
                return true;
            }
            return false;
        }
    }

    private Computer find(String computerId) {
        String targetId = clean(computerId);
        synchronized (LOCK) {
            for (Computer computer : readComputers()) {
                if (computer.computerId.equals(targetId)) return computer;
            }
            return null;
        }
    }

    public Computer remove(String computerId) {
        synchronized (LOCK) {
            String targetId = clean(computerId);
            if (targetId.isEmpty()) return null;
            List<Computer> computers = readComputers();
            Computer removed = null;
            for (Computer computer : computers) {
                if (computer.computerId.equals(targetId)) {
                    removed = computer;
                    break;
                }
            }
            if (removed == null) return null;
            computers.removeIf(value -> value.computerId.equals(targetId));
            writeComputers(computers);
            if (computers.isEmpty()) {
                preferences.edit().remove(ACTIVE_KEY).apply();
                mirrorActive(null);
            } else if (targetId.equals(preferences.getString(ACTIVE_KEY, "")) ||
                    !containsComputer(computers, preferences.getString(ACTIVE_KEY, ""))) {
                computers.sort((left, right) -> Long.compare(right.pairedAt, left.pairedAt));
                Computer next = computers.get(0);
                preferences.edit().putString(ACTIVE_KEY, next.computerId).apply();
                mirrorActive(next);
            }
            return removed;
        }
    }

    public int count() {
        synchronized (LOCK) {
            return readComputers().size();
        }
    }

    private void migrateLegacyConnection() {
        synchronized (LOCK) {
            if (preferences.getBoolean(MIGRATED_KEY, false)) return;
            List<Computer> computers = readComputers();
            if (computers.isEmpty()) {
                String baseUrl = preferences.getString("baseUrl", "");
                String localUrl = preferences.getString("localAddress", "");
                String remoteUrl = preferences.getString("remoteAddress", "");
                String fingerprint = preferences.getString("certificateSha256", "");
                String computerId = preferences.getString("computerId", "");
                if (!baseUrl.isEmpty() || !computerId.isEmpty()) {
                    if (computerId.isEmpty()) {
                        computerId = "manual-" + Integer.toHexString(baseUrl.hashCode());
                    }
                    Computer migrated = new Computer(
                            computerId, defaultName("", computerId, baseUrl), baseUrl,
                            localUrl, remoteUrl, fingerprint, System.currentTimeMillis());
                    computers.add(migrated);
                    writeComputers(computers);
                    preferences.edit().putString(ACTIVE_KEY, migrated.computerId).apply();
                }
            }
            preferences.edit().putBoolean(MIGRATED_KEY, true).apply();
        }
    }

    private List<Computer> readComputers() {
        List<Computer> result = new ArrayList<>();
        String json = preferences.getString(COMPUTERS_KEY, "[]");
        try {
            JsonElement root = JsonParser.parseString(json == null ? "[]" : json);
            if (!root.isJsonArray()) return result;
            for (JsonElement element : root.getAsJsonArray()) {
                if (!element.isJsonObject()) continue;
                JsonObject value = element.getAsJsonObject();
                Computer computer = new Computer(
                        stringValue(value, "computerId"), stringValue(value, "name"),
                        stringValue(value, "remark"),
                        stringValue(value, "baseUrl"), stringValue(value, "localUrl"),
                        stringValue(value, "remoteUrl"),
                        stringValue(value, "routePreference"),
                        stringValue(value, "certificateFingerprint"),
                        longValue(value, "pairedAt"));
                if (!computer.computerId.isEmpty()) result.add(computer);
            }
        } catch (Exception ignored) { }
        return result;
    }

    private void writeComputers(List<Computer> computers) {
        JsonArray array = new JsonArray();
        for (Computer computer : computers) {
            JsonObject value = new JsonObject();
            value.addProperty("computerId", computer.computerId);
            value.addProperty("name", computer.name);
            value.addProperty("remark", computer.remark);
            value.addProperty("baseUrl", computer.baseUrl);
            value.addProperty("localUrl", computer.localUrl);
            value.addProperty("remoteUrl", computer.remoteUrl);
            value.addProperty("routePreference", computer.routePreference);
            value.addProperty("certificateFingerprint", computer.certificateFingerprint);
            value.addProperty("pairedAt", computer.pairedAt);
            array.add(value);
        }
        preferences.edit().putString(COMPUTERS_KEY, array.toString()).commit();
    }

    private void mirrorActive(Computer computer) {
        SharedPreferences.Editor editor = preferences.edit();
        if (computer == null) {
            editor.remove("baseUrl").remove("localAddress").remove("remoteAddress")
                    .remove("routePreference").remove("certificateSha256")
                    .remove("computerId").apply();
            return;
        }
        editor.putString("baseUrl", computer.baseUrl)
                .putString("localAddress", computer.localUrl)
                .putString("remoteAddress", computer.remoteUrl)
                .putString("routePreference", computer.routePreference)
                .putString("certificateSha256", computer.certificateFingerprint)
                .putString("computerId", computer.computerId).apply();
    }

    public static String defaultName(String computerName, String computerId, String address) {
        String supplied = clean(computerName);
        if (!supplied.isEmpty()) return supplied;
        String host = addressHost(address);
        if (!host.isEmpty()) return "电脑 " + host;
        if (computerId != null && computerId.length() > 12) {
            return "电脑 " + computerId.substring(computerId.length() - 8);
        }
        return clean(computerId).isEmpty() ? "未命名电脑" : clean(computerId);
    }

    private static String addressHost(String address) {
        try {
            String normalized = normalizeAddress(address);
            return normalized.isEmpty() ? "" : clean(URI.create(normalized).getHost());
        } catch (Exception ignored) {
            return "";
        }
    }

    private static String normalizeAddress(String value) {
        String result = clean(value);
        while (result.endsWith("/")) result = result.substring(0, result.length() - 1);
        if (!result.isEmpty() && !result.startsWith("https://") &&
                !result.startsWith("http://")) result = "https://" + result;
        return result;
    }

    private static String normalizeFingerprint(String value) {
        return clean(value).replace(":", "").replace(" ", "").toUpperCase(java.util.Locale.US);
    }

    public static String normalizeRoutePreference(String value) {
        return ROUTE_REMOTE.equalsIgnoreCase(clean(value)) ? ROUTE_REMOTE : ROUTE_LOCAL;
    }

    public static String preferredAddress(String local, String remote, String preference) {
        String normalizedLocal = normalizeAddress(local);
        String normalizedRemote = normalizeAddress(remote);
        if (ROUTE_REMOTE.equals(normalizeRoutePreference(preference))) {
            return !normalizedRemote.isEmpty() ? normalizedRemote : normalizedLocal;
        }
        return !normalizedLocal.isEmpty() ? normalizedLocal : normalizedRemote;
    }

    private static boolean containsComputer(List<Computer> computers, String computerId) {
        for (Computer computer : computers) {
            if (computer.computerId.equals(computerId)) return true;
        }
        return false;
    }

    private static String clean(String value) { return value == null ? "" : value.trim(); }
    private static String stringValue(JsonObject object, String name) {
        return object.has(name) && !object.get(name).isJsonNull()
                ? object.get(name).getAsString() : "";
    }
    private static long longValue(JsonObject object, String name) {
        try { return object.has(name) ? object.get(name).getAsLong() : 0L; }
        catch (Exception ignored) { return 0L; }
    }
}
