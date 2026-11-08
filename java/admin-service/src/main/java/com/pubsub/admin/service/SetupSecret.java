package com.pubsub.admin.service;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.HexFormat;

/**
 * The one-time secret that first-run setup asks for before it creates the first administrator (R-0153).
 *
 * While no administrator exists, anyone who can reach the editor could otherwise create one, and with it manage every
 * member's credentials and permissions. The secret proves that whoever runs setup has access to the host the editor
 * is installed on: it is written to a file there that only the account running the editor can read, and is shown
 * nowhere else. Creating the first administrator deletes the file, so the secret is used once.
 */
public class SetupSecret {
    /** The number of random bytes in a secret; it is written as twice as many hexadecimal digits. */
    static final int SECRET_BYTES = 32;

    /** One generator for every secret. A SecureRandom may be shared between threads. */
    private static final SecureRandom RANDOM = new SecureRandom();

    private final Path file;

    /** @param file Where the secret is kept. */
    public SetupSecret(Path file) {
        this.file = file;
    }

    /** Where the secret is kept. */
    public Path file() {
        return file;
    }

    /**
     * Writes a new secret to the file, readable and writable only by its owner, unless the file already holds one. A
     * secret written before is kept, so that an operator who has already read it is not sent back to the host because
     * the editor restarted.
     *
     * @return true if a new secret was written.
     */
    public boolean createIfAbsent() {
        try {
            Files.createFile(file, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        } catch (FileAlreadyExistsException alreadyThere) {
            return false;
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot create the first-run setup secret file " + file, e);
        }
        byte[] random = new byte[SECRET_BYTES];
        RANDOM.nextBytes(random);
        String secret = HexFormat.of().formatHex(random);
        try {
            Files.writeString(file, secret + "\n", StandardCharsets.US_ASCII);
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot write the first-run setup secret to " + file, e);
        }
        return true;
    }

    /**
     * Whether {@code offered} is the secret in the file. False when there is no file, or nothing was offered. Spaces and line
     * ends around either are ignored, since the secret is copied from a file. The comparison takes the same time
     * wherever the two first differ, so the time it takes says nothing about the secret.
     */
    public boolean matches(String offered) {
        if (offered == null || offered.isBlank()) {
            return false;
        }
        String secret;
        try {
            secret = Files.readString(file, StandardCharsets.US_ASCII).strip();
        } catch (NoSuchFileException absent) {
            return false;
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot read the first-run setup secret from " + file, e);
        }
        if (secret.isEmpty()) {
            return false;
        }
        return MessageDigest.isEqual(secret.getBytes(StandardCharsets.US_ASCII), offered.strip().getBytes(StandardCharsets.US_ASCII));
    }

    /** Deletes the file, so that the secret cannot be used again. Nothing happens if there is no file. */
    public void remove() {
        try {
            Files.deleteIfExists(file);
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot delete the first-run setup secret file " + file, e);
        }
    }
}
