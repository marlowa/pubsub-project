package com.pubsub.admin;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class ConfigTest {
    @Test
    void loadReadsDefaultsFromPropertiesFile() {
        Config config = Config.load();

        assertEquals("jdbc:postgresql://localhost:5432/pubsub", config.dbUrl());
        assertEquals("pubsub_app", config.dbUsername());
        assertEquals("pubsub_", config.tablePrefix());
        assertEquals(8080, config.serverPort());
        assertEquals("127.0.0.1", config.authServiceHost());
        assertEquals(7072, config.authServiceAdminPort());
        assertTrue(config.authServiceEnabled());
        assertEquals("PubSub Admin", config.brandName());
        assertEquals("", config.brandLogoUrl());
        assertEquals("", config.brandCssFile());
        assertEquals("admin_users.toml", config.adminUsersFile());
    }
}
