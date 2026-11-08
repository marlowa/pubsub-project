package com.pubsub.admin.service;

import com.pubsub.admin.model.AdminRole;

import java.nio.file.Path;

/**
 * Creates the editor's first administrator, at first-run setup, once the one-time setup secret has been given
 * (R-0153).
 *
 * The checks are made in this order: that no administrator exists yet, that the secret is right, that the two
 * passwords match, and that the password is long enough. The secret is checked before anything about the password, so
 * that a request without it learns nothing else. Creating the administrator deletes the secret's file.
 */
public class FirstAdministratorSetup {
    /** The shortest password accepted for the first administrator. */
    public static final int MINIMUM_PASSWORD_LENGTH = 8;

    /** What came of a request to create the first administrator. */
    public enum Outcome {
        /** The administrator was created, and the secret deleted. */
        CREATED,
        /** An administrator exists already, so setup is closed and nothing was done. */
        ALREADY_SET_UP,
        /** The secret given was not the one in the file, or there is no file. Nothing was done. */
        WRONG_SECRET,
        /** The password and its confirmation differ. Nothing was done. */
        PASSWORDS_DIFFER,
        /** The password is shorter than MINIMUM_PASSWORD_LENGTH. Nothing was done. */
        PASSWORD_TOO_SHORT
    }

    private final AdminUserStore store;
    private final SetupSecret secret;

    public FirstAdministratorSetup(AdminUserStore store, SetupSecret secret) {
        this.store = store;
        this.secret = secret;
    }

    /** Whether setup is still open: no administrator exists yet. */
    public boolean isOpen() {
        return store.isEmpty();
    }

    /**
     * Prepares for setup when the editor starts: writes a secret if no administrator exists and there is none, and
     * deletes any secret left over once one does.
     *
     * @return true if setup is open, and the secret is in the file secretFile() names.
     */
    public boolean prepare() {
        if (store.isEmpty()) {
            secret.createIfAbsent();
            return true;
        }
        secret.remove();
        return false;
    }

    /** The file that holds the secret setup asks for. */
    public Path secretFile() {
        return secret.file();
    }

    /**
     * Creates the first administrator, {@code username} with {@code password}, if {@code offeredSecret} is the setup secret and the
     * password passes. Synchronized so that two requests at once cannot both create one.
     */
    public synchronized Outcome create(String username, String password, String confirm, String offeredSecret) {
        if (!store.isEmpty()) {
            return Outcome.ALREADY_SET_UP;
        }
        if (!secret.matches(offeredSecret)) {
            return Outcome.WRONG_SECRET;
        }
        if (!password.equals(confirm)) {
            return Outcome.PASSWORDS_DIFFER;
        }
        if (password.length() < MINIMUM_PASSWORD_LENGTH) {
            return Outcome.PASSWORD_TOO_SHORT;
        }
        store.createUser(username, password, AdminRole.ADMIN, false);
        secret.remove();
        return Outcome.CREATED;
    }
}
