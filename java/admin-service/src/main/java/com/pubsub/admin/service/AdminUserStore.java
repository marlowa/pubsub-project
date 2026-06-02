package com.pubsub.admin.service;

import com.pubsub.admin.model.AdminRole;
import com.pubsub.admin.model.AdminUser;
import org.mindrot.jbcrypt.BCrypt;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/**
 * File-backed store for admin UI users.  All credentials are bcrypt-hashed.
 * Persistence format is a simple TOML file containing [[user]] array-of-tables entries.
 * All mutations are atomic (write to .tmp then rename).
 */
public class AdminUserStore {
    private final Path filePath;

    public AdminUserStore(Path filePath) {
        this.filePath = filePath;
    }

    public synchronized boolean isEmpty() {
        return loadAll().isEmpty();
    }

    public synchronized Optional<AdminUser> findByUsername(String username) {
        return loadAll().stream()
                .filter(u -> u.username().equals(username))
                .findFirst();
    }

    public boolean checkPassword(String username, String rawPassword) {
        return findByUsername(username)
                .map(u -> BCrypt.checkpw(rawPassword, u.passwordHash()))
                .orElse(false);
    }

    public synchronized void createUser(String username, String rawPassword,
                                        AdminRole role, boolean forcePasswordChange) {
        List<AdminUser> users = loadAll();
        if (users.stream().anyMatch(u -> u.username().equals(username))) {
            throw new IllegalArgumentException("User already exists: " + username);
        }
        users.add(new AdminUser(username, BCrypt.hashpw(rawPassword, BCrypt.gensalt(12)),
                role, forcePasswordChange));
        save(users);
    }

    public synchronized void updatePassword(String username, String rawPassword) {
        List<AdminUser> users = loadAll();
        save(users.stream()
                .map(u -> u.username().equals(username)
                        ? new AdminUser(u.username(),
                                BCrypt.hashpw(rawPassword, BCrypt.gensalt(12)),
                                u.role(), false)
                        : u)
                .toList());
    }

    public synchronized void updateRole(String username, AdminRole role) {
        List<AdminUser> users = loadAll();
        save(users.stream()
                .map(u -> u.username().equals(username)
                        ? new AdminUser(u.username(), u.passwordHash(), role, u.forcePasswordChange())
                        : u)
                .toList());
    }

    public synchronized void setForcePasswordChange(String username, boolean force) {
        List<AdminUser> users = loadAll();
        save(users.stream()
                .map(u -> u.username().equals(username)
                        ? new AdminUser(u.username(), u.passwordHash(), u.role(), force)
                        : u)
                .toList());
    }

    public synchronized void deleteUser(String username) {
        save(loadAll().stream()
                .filter(u -> !u.username().equals(username))
                .toList());
    }

    public synchronized long countAdmins() {
        return loadAll().stream()
                .filter(u -> u.role() == AdminRole.ADMIN)
                .count();
    }

    // ---- TOML serialization --------------------------------------------------------

    public List<AdminUser> loadAll() {
        if (!Files.exists(filePath)) {
            return new ArrayList<>();
        }
        try {
            return parse(Files.readString(filePath));
        } catch (IOException e) {
            throw new IllegalStateException("Failed to read admin users file: " + filePath, e);
        }
    }

    private void save(List<AdminUser> users) {
        Path tmp = filePath.resolveSibling(filePath.getFileName() + ".tmp");
        StringBuilder sb = new StringBuilder();
        sb.append("# PubSub admin service users — do not edit while the service is running.\n\n");
        for (AdminUser u : users) {
            sb.append("[[user]]\n");
            sb.append("username              = \"").append(u.username()).append("\"\n");
            sb.append("password_hash         = \"").append(u.passwordHash()).append("\"\n");
            sb.append("role                  = \"").append(u.role().name()).append("\"\n");
            sb.append("force_password_change = ").append(u.forcePasswordChange()).append("\n\n");
        }
        try {
            Files.writeString(tmp, sb.toString());
            Files.move(tmp, filePath,
                    StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException e) {
            throw new IllegalStateException("Failed to write admin users file: " + filePath, e);
        }
    }

    private static List<AdminUser> parse(String content) {
        List<AdminUser> users = new ArrayList<>();
        for (String block : content.split("\\[\\[user\\]\\]")) {
            String username = null;
            String passwordHash = null;
            String role = "VIEWER";
            boolean forcePasswordChange = false;
            for (String rawLine : block.split("\n")) {
                String line = rawLine.strip();
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                int eq = line.indexOf('=');
                if (eq < 0) {
                    continue;
                }
                String key = line.substring(0, eq).strip();
                String val = line.substring(eq + 1).strip();
                if (val.startsWith("\"") && val.endsWith("\"")) {
                    val = val.substring(1, val.length() - 1);
                }
                switch (key) {
                    case "username"              -> username = val;
                    case "password_hash"         -> passwordHash = val;
                    case "role"                  -> role = val;
                    case "force_password_change" -> forcePasswordChange = Boolean.parseBoolean(val);
                    default -> { /* ignore unknown keys */ }
                }
            }
            if (username != null && passwordHash != null) {
                users.add(new AdminUser(username, passwordHash,
                        AdminRole.valueOf(role), forcePasswordChange));
            }
        }
        return users;
    }
}
