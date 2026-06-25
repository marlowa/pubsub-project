package com.pubsub.fixtestclient;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;

class ConfigTest {

    @Test
    void loadReadsAllFieldsFromTomlFile() {
        String path = getClass().getClassLoader().getResource("test-app.toml").getPath();
        Config config = Config.load(path);

        assertEquals(9090,            config.serverPort());
        assertEquals("test-output",   config.outputDir());
        assertEquals("test-scripts",  config.scriptsDir());
        assertEquals("192.168.1.1",   config.gatewayHost());
        assertEquals(7777,            config.gatewayPort());
        assertEquals("TEST_GW",       config.targetCompId());
        assertEquals("certs/test.jks", config.trustStorePath());
        assertEquals("test_pass",     config.trustStorePassword());
    }

    @Test
    void loadUsesDefaultsWhenFieldsAbsent() {
        String path = getClass().getClassLoader().getResource("empty-app.toml").getPath();
        Config config = Config.load(path);

        assertEquals(8081,                           config.serverPort());
        assertEquals("output",                       config.outputDir());
        assertEquals("scripts",                      config.scriptsDir());
        assertEquals("127.0.0.1",                    config.gatewayHost());
        assertEquals(9879,                           config.gatewayPort());
        assertEquals("GATEWAY",                      config.targetCompId());
        assertEquals("config/fix_gateway_trust.jks", config.trustStorePath());
        assertEquals("pubsub_dev",                   config.trustStorePassword());
    }
}
