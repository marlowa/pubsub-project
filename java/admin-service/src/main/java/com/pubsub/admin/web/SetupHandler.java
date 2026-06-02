package com.pubsub.admin.web;

import com.pubsub.admin.model.AdminRole;
import com.pubsub.admin.service.AdminUserStore;
import io.javalin.http.Context;

import java.util.Map;

/** Handles the first-run setup wizard shown when no admin users exist. */
public class SetupHandler {
    private final AdminUserStore store;

    public SetupHandler(AdminUserStore store) {
        this.store = store;
    }

    public void show(Context ctx) {
        if (!store.isEmpty()) {
            ctx.redirect("/firms");
            return;
        }
        ctx.render("/templates/setup.ftl", Map.of());
    }

    public void create(Context ctx) {
        if (!store.isEmpty()) {
            ctx.redirect("/firms");
            return;
        }
        String username = requireParam(ctx, "username");
        String password = requireParam(ctx, "password");
        String confirm  = ctx.formParam("confirm");

        if (!password.equals(confirm)) {
            ctx.render("/templates/setup.ftl",
                    Map.of("error", "Passwords do not match.", "username", username));
            return;
        }
        if (password.length() < 8) {
            ctx.render("/templates/setup.ftl",
                    Map.of("error", "Password must be at least 8 characters.", "username", username));
            return;
        }

        store.createUser(username, password, AdminRole.ADMIN, false);
        ctx.redirect("/login");
    }

    private static String requireParam(Context ctx, String name) {
        String value = ctx.formParam(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("Missing required field: " + name);
        }
        return value.trim();
    }
}
