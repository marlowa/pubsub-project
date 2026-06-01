package com.pubsub.admin.service;

import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;

/** Derives SCRAM-SHA-256 credential material from a plaintext password. */
public class ScramDerivation {
    private static final int DEFAULT_ITERATIONS = 4096;
    private static final int SALT_BYTES = 16;
    private static final SecureRandom SECURE_RANDOM = new SecureRandom();

    private ScramDerivation() {}

    public static ScramCredential derive(String password) {
        return derive(password, DEFAULT_ITERATIONS);
    }

    public static ScramCredential derive(String password, int iterations) {
        byte[] salt = new byte[SALT_BYTES];
        SECURE_RANDOM.nextBytes(salt);
        return derive(password, iterations, salt);
    }

    /** Package-private to allow tests to supply a fixed salt for cross-validation. */
    static ScramCredential derive(String password, int iterations, byte[] salt) {
        try {
            byte[] saltedPassword = pbkdf2(password, salt, iterations);
            byte[] clientKey = hmacSha256(saltedPassword, "Client Key");
            byte[] storedKey = sha256(clientKey);
            byte[] serverKey = hmacSha256(saltedPassword, "Server Key");
            return new ScramCredential(
                    bytesToHex(storedKey),
                    bytesToHex(serverKey),
                    bytesToHex(salt),
                    iterations);
        } catch (Exception e) {
            throw new IllegalStateException("SCRAM-SHA-256 derivation failed", e);
        }
    }

    private static byte[] pbkdf2(String password, byte[] salt, int iterations) throws Exception {
        PBEKeySpec spec = new PBEKeySpec(password.toCharArray(), salt, iterations, 256);
        SecretKeyFactory factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256");
        byte[] key = factory.generateSecret(spec).getEncoded();
        spec.clearPassword();
        return key;
    }

    private static byte[] hmacSha256(byte[] key, String data) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(data.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] sha256(byte[] data) throws Exception {
        return MessageDigest.getInstance("SHA-256").digest(data);
    }

    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(String.format("%02x", b));
        }
        return sb.toString();
    }
}
