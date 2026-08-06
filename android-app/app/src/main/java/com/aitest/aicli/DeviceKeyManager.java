package com.aitest.aicli;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.os.Build;

import java.nio.charset.StandardCharsets;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.Signature;
import android.util.Base64;
import java.util.UUID;

/** Owns the Android Keystore identity used to authenticate this phone. */
public final class DeviceKeyManager {
    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS = "aicli.device.identity.v1";

    public String getOrCreatePhoneId(android.content.SharedPreferences preferences) {
        String current = preferences.getString("phoneId", null);
        if (current != null && !current.isEmpty()) {
            return current;
        }
        String created = "phone_" + UUID.randomUUID();
        preferences.edit().putString("phoneId", created).apply();
        return created;
    }

    public KeyPair getOrCreateKeyPair() throws Exception {
        KeyStore keyStore = KeyStore.getInstance(KEYSTORE);
        keyStore.load(null);
        if (keyStore.containsAlias(KEY_ALIAS)) {
            return new KeyPair(
                    keyStore.getCertificate(KEY_ALIAS).getPublicKey(),
                    (PrivateKey) keyStore.getKey(KEY_ALIAS, null));
        }
        KeyPairGenerator generator = KeyPairGenerator.getInstance(
                KeyProperties.KEY_ALGORITHM_EC, KEYSTORE);
        KeyGenParameterSpec.Builder builder = new KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                .setAlgorithmParameterSpec(new java.security.spec.ECGenParameterSpec("secp256r1"))
                .setDigests(KeyProperties.DIGEST_SHA256);
        // The key is usable after the phone has been unlocked, without a
        // biometric prompt for every terminal keystroke. On Android 28+ this
        // is enforced by Keystore itself; older versions rely on file-based
        // encryption and the app process boundary.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            builder.setUnlockedDeviceRequired(true);
        } else {
            builder.setUserAuthenticationRequired(false);
        }
        generator.initialize(builder.build());
        return generator.generateKeyPair();
    }

    public String publicKeyBase64() throws Exception {
        return Base64.encodeToString(getOrCreateKeyPair().getPublic().getEncoded(), Base64.NO_WRAP);
    }

    public String signChallenge(String challenge) throws Exception {
        return sign(challenge);
    }

    public String sign(String message) throws Exception {
        Signature signature = Signature.getInstance("SHA256withECDSA");
        signature.initSign(getOrCreateKeyPair().getPrivate());
        signature.update(message.getBytes(StandardCharsets.UTF_8));
        return Base64.encodeToString(signature.sign(), Base64.NO_WRAP);
    }

    public String deviceName() {
        return android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL;
    }

    public String model() {
        return android.os.Build.MODEL;
    }

    public String keyAlias() {
        return KEY_ALIAS;
    }
}

