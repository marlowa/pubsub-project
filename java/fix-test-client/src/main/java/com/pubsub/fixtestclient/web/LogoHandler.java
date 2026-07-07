package com.pubsub.fixtestclient.web;

import io.javalin.http.Context;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * Serves the company logo shown in the bottom-right corner of every page.
 *
 * The logo is configurable via [web] logo_path in app.toml. When that path points
 * at a readable file it is served; otherwise the client falls back to the bundled
 * default packaged at /web/logo.png on the classpath. A PNG is expected so a logo
 * with transparency alpha-blends over the page.
 */
public class LogoHandler {

    private static final String defaultResource = "/web/logo.png";

    private final String logoPath;

    public LogoHandler(String logoPath) {
        this.logoPath = logoPath;
    }

    public void getLogo(Context ctx) throws IOException {
        if (logoPath != null && !logoPath.isBlank()) {
            Path path = Paths.get(logoPath);
            if (Files.isRegularFile(path) && Files.isReadable(path)) {
                ctx.contentType(contentTypeFor(path.toString()));
                ctx.result(Files.readAllBytes(path));
                return;
            }
        }
        serveDefault(ctx);
    }

    private void serveDefault(Context ctx) throws IOException {
        try (InputStream in = LogoHandler.class.getResourceAsStream(defaultResource)) {
            if (in == null) {
                ctx.status(404);
                return;
            }
            ctx.contentType("image/png");
            ctx.result(in.readAllBytes());
        }
    }

    private static String contentTypeFor(String name) {
        String lower = name.toLowerCase();
        if (lower.endsWith(".png")) {
            return "image/png";
        }
        if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
            return "image/jpeg";
        }
        if (lower.endsWith(".gif")) {
            return "image/gif";
        }
        if (lower.endsWith(".svg")) {
            return "image/svg+xml";
        }
        if (lower.endsWith(".webp")) {
            return "image/webp";
        }
        return "image/png";
    }
}
