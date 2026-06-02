package com.pubsub.admin.web;

import com.pubsub.admin.model.AdminUser;
import com.pubsub.admin.service.AdminUserStore;
import io.javalin.http.Context;

import java.util.Map;

public class LoginHandler {
    private final AdminUserStore store;

    public LoginHandler(AdminUserStore store) {
        this.store = store;
    }

    public void showLogin(Context ctx) {
        String error = ctx.queryParam("error");
        Map<String, Object> model = error != null
                ? Map.of("error", "Invalid username or password.")
                : Map.of();
        ctx.render("/templates/login.ftl", model);
    }

    public void login(Context ctx) {
        String username = ctx.formParam("username");
        String password = ctx.formParam("password");

        if (username == null || username.isBlank() || password == null || password.isBlank()) {
            ctx.redirect("/login?error=1");
            return;
        }

        if (!store.checkPassword(username, password)) {
            ctx.redirect("/login?error=1");
            return;
        }

        AdminUser user = store.findByUsername(username).orElseThrow();
        ctx.req().getSession(true).setAttribute(AuthFilter.SESSION_USERNAME, username);
        ctx.req().getSession(false).setAttribute(AuthFilter.SESSION_ROLE, user.role().name());

        if (user.forcePasswordChange()) {
            ctx.redirect("/change-password");
        } else {
            ctx.redirect("/firms");
        }
    }

    public void logout(Context ctx) {
        AuthFilter.invalidateSession(ctx);
        ctx.redirect("/login");
    }
}
