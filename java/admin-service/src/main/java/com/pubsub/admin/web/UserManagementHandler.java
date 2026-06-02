package com.pubsub.admin.web;

import com.pubsub.admin.exception.ForbiddenException;
import com.pubsub.admin.exception.NotFoundException;
import com.pubsub.admin.model.AdminRole;
import com.pubsub.admin.model.AdminUser;
import com.pubsub.admin.service.AdminUserStore;
import io.javalin.http.Context;

import java.util.Map;

/** ADMIN-only handler for managing admin UI users. */
public class UserManagementHandler {
    private final AdminUserStore store;

    public UserManagementHandler(AdminUserStore store) {
        this.store = store;
    }

    public void list(Context ctx) {
        requireAdmin(ctx);
        ctx.render("/templates/users/list.ftl", Map.of("users", store.loadAll()));
    }

    public void showNew(Context ctx) {
        requireAdmin(ctx);
        ctx.render("/templates/users/form.ftl", Map.of());
    }

    public void create(Context ctx) {
        requireAdmin(ctx);
        String username  = requireParam(ctx, "username");
        String password  = requireParam(ctx, "password");
        String roleParam = ctx.formParam("role");
        AdminRole role   = "ADMIN".equals(roleParam) ? AdminRole.ADMIN : AdminRole.VIEWER;

        store.createUser(username, password, role, true);
        ctx.redirect("/admin/users");
    }

    public void showEdit(Context ctx) {
        requireAdmin(ctx);
        String username = ctx.pathParam("username");
        AdminUser user = store.findByUsername(username)
                .orElseThrow(() -> new NotFoundException("User not found: " + username));
        ctx.render("/templates/users/form.ftl", Map.of("user", user));
    }

    public void update(Context ctx) {
        requireAdmin(ctx);
        String username  = ctx.pathParam("username");
        store.findByUsername(username)
                .orElseThrow(() -> new NotFoundException("User not found: " + username));

        String roleParam = ctx.formParam("role");
        AdminRole role   = "ADMIN".equals(roleParam) ? AdminRole.ADMIN : AdminRole.VIEWER;

        if (role == AdminRole.VIEWER && isLastAdmin(username)) {
            ctx.render("/templates/users/form.ftl",
                    Map.of("user", store.findByUsername(username).orElseThrow(),
                           "error", "Cannot demote the last ADMIN user."));
            return;
        }

        store.updateRole(username, role);
        ctx.redirect("/admin/users");
    }

    public void showResetPassword(Context ctx) {
        requireAdmin(ctx);
        String username = ctx.pathParam("username");
        store.findByUsername(username)
                .orElseThrow(() -> new NotFoundException("User not found: " + username));
        ctx.render("/templates/users/reset-password.ftl", Map.of("username", username));
    }

    public void resetPassword(Context ctx) {
        requireAdmin(ctx);
        String username    = ctx.pathParam("username");
        store.findByUsername(username)
                .orElseThrow(() -> new NotFoundException("User not found: " + username));
        String newPassword = requireParam(ctx, "newPassword");
        store.updatePassword(username, newPassword);
        store.setForcePasswordChange(username, true);
        ctx.redirect("/admin/users");
    }

    public void delete(Context ctx) {
        requireAdmin(ctx);
        String username    = ctx.pathParam("username");
        String currentUser = ctx.sessionAttribute(AuthFilter.SESSION_USERNAME);

        if (username.equals(currentUser)) {
            ctx.render("/templates/users/list.ftl",
                    Map.of("users", store.loadAll(),
                           "error", "You cannot delete your own account."));
            return;
        }
        if (isLastAdmin(username)) {
            ctx.render("/templates/users/list.ftl",
                    Map.of("users", store.loadAll(),
                           "error", "Cannot delete the last ADMIN user."));
            return;
        }
        store.deleteUser(username);
        ctx.redirect("/admin/users");
    }

    private boolean isLastAdmin(String username) {
        return store.countAdmins() <= 1
                && store.findByUsername(username)
                        .map(u -> u.role() == AdminRole.ADMIN)
                        .orElse(false);
    }

    private static void requireAdmin(Context ctx) {
        String role = ctx.sessionAttribute(AuthFilter.SESSION_ROLE);
        if (!"ADMIN".equals(role)) {
            throw new ForbiddenException("Admin role required.");
        }
    }

    private static String requireParam(Context ctx, String name) {
        String value = ctx.formParam(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("Missing required field: " + name);
        }
        return value.trim();
    }
}
