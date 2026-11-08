package com.pubsub.admin.service;

import com.pubsub.admin.model.AdminRole;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class FirstAdministratorSetupTest {

    @TempDir
    Path tempDir;

    private Path secretFile() {
        return tempDir.resolve("admin_setup_secret.txt");
    }

    private AdminUserStore store() {
        return new AdminUserStore(tempDir.resolve("users.toml"));
    }

    private FirstAdministratorSetup setup() {
        return new FirstAdministratorSetup(store(), new SetupSecret(secretFile()));
    }

    private String secret() throws Exception {
        return Files.readString(secretFile(), StandardCharsets.US_ASCII).strip();
    }

    @Test
    void withNoAdministratorStartingWritesTheSecretAndSetupIsOpen() {
        FirstAdministratorSetup setup = setup();
        assertTrue(setup.prepare());
        assertTrue(setup.isOpen());
        assertTrue(Files.exists(secretFile()));
    }

    @Test
    void withoutTheSecretNoAdministratorIsCreated() {
        FirstAdministratorSetup setup = setup();
        setup.prepare();
        assertEquals(FirstAdministratorSetup.Outcome.WRONG_SECRET, setup.create("mallory", "Password1", "Password1", "guess"));
        assertEquals(FirstAdministratorSetup.Outcome.WRONG_SECRET, setup.create("mallory", "Password1", "Password1", null));
        assertTrue(store().isEmpty(), "a request without the secret created an administrator");
        assertTrue(setup.isOpen());
    }

    @Test
    void theSecretIsCheckedBeforeThePassword() {
        FirstAdministratorSetup setup = setup();
        setup.prepare();
        assertEquals(FirstAdministratorSetup.Outcome.WRONG_SECRET, setup.create("mallory", "short", "other", "guess"),
                "a request without the secret learns nothing about the password rules");
    }

    @Test
    void withTheSecretTheFirstAdministratorIsCreatedAndTheSecretCannotBeUsedAgain() throws Exception {
        FirstAdministratorSetup setup = setup();
        setup.prepare();
        String secret = secret();
        assertEquals(FirstAdministratorSetup.Outcome.CREATED, setup.create("alice", "Password1", "Password1", secret));
        assertEquals(AdminRole.ADMIN, store().findByUsername("alice").orElseThrow().role());
        assertFalse(Files.exists(secretFile()), "the secret was not deleted once used");
        assertFalse(setup.isOpen());
        assertEquals(FirstAdministratorSetup.Outcome.ALREADY_SET_UP, setup.create("bob", "Password1", "Password1", secret));
        assertTrue(store().findByUsername("bob").isEmpty());
    }

    @Test
    void thePasswordRulesStillApplyWithTheSecret() throws Exception {
        FirstAdministratorSetup setup = setup();
        setup.prepare();
        String secret = secret();
        assertEquals(FirstAdministratorSetup.Outcome.PASSWORDS_DIFFER, setup.create("alice", "Password1", "Password2", secret));
        assertEquals(FirstAdministratorSetup.Outcome.PASSWORD_TOO_SHORT, setup.create("alice", "short", "short", secret));
        assertTrue(store().isEmpty());
        assertTrue(Files.exists(secretFile()), "a refused request does not use up the secret");
    }

    @Test
    void onceAnAdministratorExistsStartingDeletesALeftoverSecret() throws Exception {
        Files.writeString(secretFile(), "leftover\n", StandardCharsets.US_ASCII);
        store().createUser("alice", "Password1", AdminRole.ADMIN, false);
        FirstAdministratorSetup setup = setup();
        assertFalse(setup.prepare());
        assertFalse(Files.exists(secretFile()));
        assertFalse(setup.isOpen());
    }
}
