package com.pubsub.admin.web;

import com.pubsub.admin.exception.ForbiddenException;
import com.pubsub.admin.model.AdminRole;
import com.pubsub.admin.model.AdminUser;
import com.pubsub.admin.service.AdminUserStore;
import io.javalin.http.Context;

import java.util.List;

/**
 * Before-handler that enforces authentication and role-based access for every request.
 *
 * Flow:
 *   1. Public paths (/login, /setup) always pass through.
 *   2. If no users exist, redirect everything to /setup.
 *   3. If no session, redirect to /login.
 *   4. If force_password_change, redirect to /change-password.
 *   5. VIEWER role may not POST to data-mutating routes.
 */
public class AuthFilter {
    public static final String SESSION_USERNAME = "username";
    public static final String SESSION_ROLE     = "role";

    private static final List<String> PUBLIC_PREFIXES      = List.of("/login", "/setup", "/static");
    private static final List<String> SAFE_AUTH_PREFIXES   = List.of("/change-password", "/logout");

    private final AdminUserStore store;

    public AuthFilter(AdminUserStore store) {
        this.store = store;
    }

    public void apply(Context ctx) {
        String path = ctx.path();

        if (PUBLIC_PREFIXES.stream().anyMatch(path::startsWith)) {
            return;
        }

        if (store.isEmpty()) {
            ctx.redirect("/setup");
            return;
        }

        String username = ctx.sessionAttribute(SESSION_USERNAME);
        if (username == null) {
            ctx.redirect("/login");
            return;
        }

        AdminUser user = store.findByUsername(username).orElse(null);
        if (user == null) {
            invalidateSession(ctx);
            ctx.redirect("/login");
            return;
        }

        if (user.forcePasswordChange()
                && SAFE_AUTH_PREFIXES.stream().noneMatch(path::startsWith)) {
            ctx.redirect("/change-password");
            return;
        }

        if (user.role() == AdminRole.VIEWER
                && "POST".equals(ctx.req().getMethod())
                && SAFE_AUTH_PREFIXES.stream().noneMatch(path::startsWith)) {
            throw new ForbiddenException("Insufficient permissions — ADMIN role required");
        }
    }

    public static void invalidateSession(Context ctx) {
        jakarta.servlet.http.HttpSession session = ctx.req().getSession(false);
        if (session != null) {
            session.invalidate();
        }
    }
}
