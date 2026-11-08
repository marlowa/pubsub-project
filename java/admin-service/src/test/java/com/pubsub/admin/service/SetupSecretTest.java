package com.pubsub.admin.service;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class SetupSecretTest {

    @TempDir
    Path tempDir;

    private Path file() {
        return tempDir.resolve("admin_setup_secret.txt");
    }

    private String written() throws Exception {
        return Files.readString(file(), StandardCharsets.US_ASCII).strip();
    }

    @Test
    void aNewSecretIsReadableOnlyByItsOwnerAndIsLongAndRandom() throws Exception {
        SetupSecret secret = new SetupSecret(file());
        assertTrue(secret.createIfAbsent());
        assertEquals("rw-------", PosixFilePermissions.toString(Files.getPosixFilePermissions(file())));
        String first = written();
        assertEquals(SetupSecret.SECRET_BYTES * 2, first.length());
        assertTrue(first.matches("[0-9a-f]+"));

        secret.remove();
        assertTrue(secret.createIfAbsent());
        assertNotEquals(first, written(), "each secret is drawn afresh");
    }

    @Test
    void aSecretAlreadyWrittenIsKept() throws Exception {
        SetupSecret secret = new SetupSecret(file());
        assertTrue(secret.createIfAbsent());
        String first = written();
        assertFalse(secret.createIfAbsent());
        assertEquals(first, written(), "an operator who read it before the editor restarted can still use it");
    }

    @Test
    void onlyTheSecretInTheFileMatches() throws Exception {
        SetupSecret secret = new SetupSecret(file());
        secret.createIfAbsent();
        String right = written();
        assertTrue(secret.matches(right));
        assertTrue(secret.matches("  " + right + "\n"), "spaces and line ends copied with it are ignored");
        assertFalse(secret.matches(right.substring(1)));
        assertFalse(secret.matches(right + "0"));
        assertFalse(secret.matches(""));
        assertFalse(secret.matches(null));
    }

    @Test
    void nothingMatchesOnceTheFileIsRemovedOrIfItIsEmpty() throws Exception {
        SetupSecret secret = new SetupSecret(file());
        secret.createIfAbsent();
        String right = written();
        secret.remove();
        assertFalse(Files.exists(file()));
        assertFalse(secret.matches(right));
        secret.remove();

        Files.writeString(file(), "\n", StandardCharsets.US_ASCII);
        assertFalse(secret.matches(" "));
        assertFalse(secret.matches("x"));
    }
}
