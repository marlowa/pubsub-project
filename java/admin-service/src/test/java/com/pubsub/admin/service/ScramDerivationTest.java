package com.pubsub.admin.service;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;

/**
 * Cross-validates ScramDerivation against the known test vectors from auth_service_test.py.
 *
 * Reference values (auth_service_test.py lines 84–331):
 *   STUB_PASSWORD    = "stubpassword"
 *   _STUB_SALT       = "0102030405060708090a0b0c0d0e0f10"
 *   _STUB_STORED_KEY = "e0eaf13bf630627621a7f47e378fb8c62c5b4bb709d42767d0193dc537f34be2"
 *   _STUB_SERVER_KEY = "c016b7864891fe5bad757b60de234df09dde5a4be4deb015e158ca1aae9bec7d"
 *   _STUB_ITERATIONS = 4096
 */
class ScramDerivationTest {
    private static final String STUB_PASSWORD   = "stubpassword";
    private static final String STUB_SALT_HEX   = "0102030405060708090a0b0c0d0e0f10";
    private static final String STUB_STORED_KEY =
            "e0eaf13bf630627621a7f47e378fb8c62c5b4bb709d42767d0193dc537f34be2";
    private static final String STUB_SERVER_KEY =
            "c016b7864891fe5bad757b60de234df09dde5a4be4deb015e158ca1aae9bec7d";
    private static final int STUB_ITERATIONS = 4096;

    @Test
    void deriveMatchesPythonReferenceVectors() {
        ScramCredential cred = ScramDerivation.derive(
                STUB_PASSWORD, STUB_ITERATIONS, hexToBytes(STUB_SALT_HEX));

        assertEquals(STUB_STORED_KEY, cred.storedKey(), "storedKey must match Python reference");
        assertEquals(STUB_SERVER_KEY, cred.serverKey(), "serverKey must match Python reference");
        assertEquals(STUB_SALT_HEX,   cred.salt(),      "salt must round-trip correctly");
        assertEquals(STUB_ITERATIONS, cred.iterations());
    }

    @Test
    void derivedKeysAreCorrectHexLength() {
        ScramCredential cred = ScramDerivation.derive("anypassword", 4096);
        assertEquals(64, cred.storedKey().length(), "storedKey is SHA-256 = 32 bytes = 64 hex chars");
        assertEquals(64, cred.serverKey().length(), "serverKey is HMAC-SHA-256 = 32 bytes = 64 hex chars");
        assertEquals(32, cred.salt().length(),      "salt is 16 random bytes = 32 hex chars");
        assertEquals(4096, cred.iterations());
    }

    @Test
    void samePasswordAndSaltProducesSameCredential() {
        byte[] salt = hexToBytes(STUB_SALT_HEX);
        ScramCredential first  = ScramDerivation.derive("mypassword", 4096, salt);
        ScramCredential second = ScramDerivation.derive("mypassword", 4096, salt);
        assertEquals(first.storedKey(), second.storedKey());
        assertEquals(first.serverKey(), second.serverKey());
    }

    @Test
    void differentPasswordsProduceDifferentKeys() {
        byte[] salt = hexToBytes(STUB_SALT_HEX);
        ScramCredential cred1 = ScramDerivation.derive("password1", 4096, salt);
        ScramCredential cred2 = ScramDerivation.derive("password2", 4096, salt);
        assertNotEquals(cred1.storedKey(), cred2.storedKey());
        assertNotEquals(cred1.serverKey(), cred2.serverKey());
    }

    @Test
    void randomDerivationsUseDifferentSalts() {
        ScramCredential cred1 = ScramDerivation.derive("mypassword", 4096);
        ScramCredential cred2 = ScramDerivation.derive("mypassword", 4096);
        // Probability of collision is 2^-128 — effectively impossible.
        assertNotEquals(cred1.salt(), cred2.salt());
    }

    private static byte[] hexToBytes(String hex) {
        byte[] bytes = new byte[hex.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        }
        return bytes;
    }
}
