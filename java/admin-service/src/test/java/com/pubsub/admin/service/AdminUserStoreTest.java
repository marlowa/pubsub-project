package com.pubsub.admin.service;

import com.pubsub.admin.model.AdminRole;
import com.pubsub.admin.model.AdminUser;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Path;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class AdminUserStoreTest {

    @TempDir
    Path tempDir;

    private AdminUserStore store() {
        return new AdminUserStore(tempDir.resolve("users.toml"));
    }

    @Test
    void emptyWhenFileAbsent() {
        assertTrue(store().isEmpty());
        assertTrue(store().loadAll().isEmpty());
    }

    @Test
    void createAndFindUser() {
        AdminUserStore s = store();
        s.createUser("alice", "Password1", AdminRole.ADMIN, false);

        assertFalse(s.isEmpty());
        AdminUser user = s.findByUsername("alice").orElseThrow();
        assertEquals("alice", user.username());
        assertEquals(AdminRole.ADMIN, user.role());
        assertFalse(user.forcePasswordChange());
    }

    @Test
    void checkPasswordCorrectAndWrong() {
        AdminUserStore s = store();
        s.createUser("bob", "Secret99!", AdminRole.VIEWER, true);

        assertTrue(s.checkPassword("bob", "Secret99!"));
        assertFalse(s.checkPassword("bob", "wrong"));
        assertFalse(s.checkPassword("nobody", "Secret99!"));
    }

    @Test
    void duplicateUsernameThrows() {
        AdminUserStore s = store();
        s.createUser("carol", "Pass1234", AdminRole.VIEWER, false);

        assertThrows(IllegalArgumentException.class,
                () -> s.createUser("carol", "Other567", AdminRole.ADMIN, false));
    }

    @Test
    void updateRole() {
        AdminUserStore s = store();
        s.createUser("dave", "Pass1234", AdminRole.VIEWER, false);

        s.updateRole("dave", AdminRole.ADMIN);

        assertEquals(AdminRole.ADMIN, s.findByUsername("dave").orElseThrow().role());
    }

    @Test
    void updatePasswordClearsForceFlag() {
        AdminUserStore s = store();
        s.createUser("eve", "OldPass1", AdminRole.VIEWER, true);

        s.updatePassword("eve", "NewPass2");

        assertTrue(s.checkPassword("eve", "NewPass2"));
        assertFalse(s.findByUsername("eve").orElseThrow().forcePasswordChange());
    }

    @Test
    void setForceFlagExplicitly() {
        AdminUserStore s = store();
        s.createUser("frank", "Pass1234", AdminRole.VIEWER, false);

        s.setForcePasswordChange("frank", true);

        assertTrue(s.findByUsername("frank").orElseThrow().forcePasswordChange());
    }

    @Test
    void deleteUser() {
        AdminUserStore s = store();
        s.createUser("grace", "Pass1234", AdminRole.VIEWER, false);
        s.deleteUser("grace");

        assertTrue(s.findByUsername("grace").isEmpty());
        assertTrue(s.isEmpty());
    }

    @Test
    void countAdmins() {
        AdminUserStore s = store();
        s.createUser("u1", "Pass1234", AdminRole.ADMIN, false);
        s.createUser("u2", "Pass1234", AdminRole.VIEWER, false);
        s.createUser("u3", "Pass1234", AdminRole.ADMIN, false);

        assertEquals(2, s.countAdmins());
    }

    @Test
    void findByUsername_unknownUser_returnsEmpty() {
        assertTrue(store().findByUsername("nobody").isEmpty());
    }

    @Test
    void countAdmins_noAdmins_returnsZero() {
        AdminUserStore s = store();
        s.createUser("viewer", "Pass1234", AdminRole.VIEWER, false);

        assertEquals(0, s.countAdmins());
    }

    @Test
    void loadAll_multipleUsers_returnsAll() {
        AdminUserStore s = store();
        s.createUser("u1", "Pass1234", AdminRole.ADMIN,  false);
        s.createUser("u2", "Pass5678", AdminRole.VIEWER, false);
        s.createUser("u3", "Pass9012", AdminRole.ADMIN,  false);

        assertEquals(3, s.loadAll().size());
    }

    @Test
    void updateRole_unknownUser_isNoOp() {
        AdminUserStore s = store();
        s.createUser("alice", "Pass1234", AdminRole.VIEWER, false);

        s.updateRole("nobody", AdminRole.ADMIN);

        assertEquals(AdminRole.VIEWER, s.findByUsername("alice").orElseThrow().role());
    }

    @Test
    void deleteUser_unknownUser_isNoOp() {
        AdminUserStore s = store();
        s.createUser("alice", "Pass1234", AdminRole.VIEWER, false);

        s.deleteUser("nobody");

        assertEquals(1, s.loadAll().size());
    }

    @Test
    void persistsAcrossInstances() {
        Path file = tempDir.resolve("shared.toml");
        AdminUserStore s1 = new AdminUserStore(file);
        s1.createUser("helen", "Pass1234", AdminRole.ADMIN, false);

        AdminUserStore s2 = new AdminUserStore(file);
        List<AdminUser> users = s2.loadAll();
        assertEquals(1, users.size());
        assertEquals("helen", users.get(0).username());
    }
}
