package com.pubsub.admin.web;

import com.pubsub.admin.service.FirstAdministratorSetup;
import io.javalin.http.Context;

import java.util.Map;

/**
 * Handles the first-run setup page, shown while no administrator exists. Creating the first administrator needs the
 * one-time setup secret, which is in a file on the host the editor runs on (FirstAdministratorSetup, R-0153).
 */
public class SetupHandler {
    private final FirstAdministratorSetup setup;

    public SetupHandler(FirstAdministratorSetup setup) {
        this.setup = setup;
    }

    public void show(Context ctx) {
        if (!setup.isOpen()) {
            ctx.redirect("/firms");
            return;
        }
        ctx.render("/templates/setup.ftl", Map.of());
    }

    public void create(Context ctx) {
        String username = requireParam(ctx, "username");
        String password = requireParam(ctx, "password");
        switch (setup.create(username, password, ctx.formParam("confirm"), ctx.formParam("secret"))) {
            case CREATED -> ctx.redirect("/login");
            case ALREADY_SET_UP -> ctx.redirect("/firms");
            case WRONG_SECRET -> ctx.status(403).render("/templates/setup.ftl",
                    Map.of("error", "That is not the one-time setup secret.", "username", username));
            case PASSWORDS_DIFFER -> ctx.render("/templates/setup.ftl",
                    Map.of("error", "Passwords do not match.", "username", username));
            case PASSWORD_TOO_SHORT -> ctx.render("/templates/setup.ftl",
                    Map.of("error", "Password must be at least " + FirstAdministratorSetup.MINIMUM_PASSWORD_LENGTH + " characters.",
                            "username", username));
            default -> throw new IllegalStateException("an outcome of first-run setup that is not handled");
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
