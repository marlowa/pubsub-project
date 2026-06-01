package com.pubsub.admin;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

public record Config(
        String dbUrl,
        String dbUsername,
        String dbPassword,
        String tablePrefix,
        boolean authServiceEnabled,
        String authServiceHost,
        int authServiceAdminPort,
        int serverPort
) {
    public static Config load() {
        Properties props = new Properties();
        try (InputStream stream = Config.class.getResourceAsStream("/application.properties")) {
            if (stream == null) {
                throw new IllegalStateException("application.properties not found on classpath");
            }
            props.load(stream);
        } catch (IOException e) {
            throw new IllegalStateException("Failed to load application.properties", e);
        }

        return new Config(
                env("PUBSUB_DB_URL",      props.getProperty("db.url")),
                env("PUBSUB_DB_USERNAME", props.getProperty("db.username")),
                env("PUBSUB_APP_DB_PASSWORD", props.getProperty("db.password")),
                props.getProperty("db.table-prefix", "pubsub_"),
                Boolean.parseBoolean(props.getProperty("auth-service.enabled", "true")),
                props.getProperty("auth-service.host", "127.0.0.1"),
                Integer.parseInt(props.getProperty("auth-service.admin-port", "7072")),
                Integer.parseInt(props.getProperty("server.port", "8080"))
        );
    }

    /** Returns the environment variable value if set, otherwise the fallback. */
    private static String env(String name, String fallback) {
        String value = System.getenv(name);
        return (value != null && !value.isBlank()) ? value : fallback;
    }
}
