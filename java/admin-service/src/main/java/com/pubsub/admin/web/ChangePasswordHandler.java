package com.pubsub.admin.web;

import com.pubsub.admin.service.AdminUserStore;
import io.javalin.http.Context;

import java.util.Map;

public class ChangePasswordHandler {
    private final AdminUserStore store;

    public ChangePasswordHandler(AdminUserStore store) {
        this.store = store;
    }

    public void show(Context ctx) {
        ctx.render("/templates/change-password.ftl", Map.of());
    }

    public void change(Context ctx) {
        String username    = ctx.sessionAttribute(AuthFilter.SESSION_USERNAME);
        String currentPwd  = ctx.formParam("currentPassword");
        String newPwd      = ctx.formParam("newPassword");
        String confirmPwd  = ctx.formParam("confirmPassword");

        if (!store.checkPassword(username, currentPwd)) {
            ctx.render("/templates/change-password.ftl",
                    Map.of("error", "Current password is incorrect."));
            return;
        }
        if (newPwd == null || newPwd.length() < 8) {
            ctx.render("/templates/change-password.ftl",
                    Map.of("error", "New password must be at least 8 characters."));
            return;
        }
        if (!newPwd.equals(confirmPwd)) {
            ctx.render("/templates/change-password.ftl",
                    Map.of("error", "Passwords do not match."));
            return;
        }

        store.updatePassword(username, newPwd);
        ctx.redirect("/firms");
    }
}
