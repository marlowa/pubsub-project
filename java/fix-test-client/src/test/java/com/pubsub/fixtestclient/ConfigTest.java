package com.pubsub.fixtestclient;

import com.pubsub.fixtestclient.fix.LogonMode;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import com.pubsub.fixtestclient.gateway.GatewayKind;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class ConfigTest {

    private Config load(String resource) {
        return Config.load(getClass().getClassLoader().getResource(resource).getPath());
    }

    @Test
    void loadReadsAllFieldsFromTomlFile() {
        Config config = load("test-app.toml");

        assertEquals(9090,            config.serverPort());
        assertEquals("test-output",   config.outputDir());
        assertEquals("test-scripts",  config.scriptsDir());
        assertEquals("TEST_GW",       config.targetCompId());
        assertEquals("certs/test.jks", config.trustStorePath());
        assertEquals("test_pass",     config.trustStorePassword());
        assertEquals("certs/test-logo.png", config.logoPath());
    }

    @Test
    void loadReadsEveryGatewayInOrder() {
        Config config = load("test-app.toml");

        assertEquals(4, config.gateways().size());
        // Order is the order the logon page offers them, so it is part of the contract:
        // the first entry is what the page selects before anyone touches it.
        assertEquals("fix-a", config.defaultGateway().key());
        assertEquals(java.util.List.of("fix-a", "fix-b", "fix-proprietary", "binary-a"),
                     config.gateways().stream().map(GatewayEndpoint::key).toList());
    }

    @Test
    void aSecondInstanceOfOneProtocolIsItsOwnEndpoint() {
        // The whole point of the list. The old shape held one port per protocol, so instance
        // b had nowhere to live and was unreachable however the venue was deployed.
        GatewayEndpoint a = load("test-app.toml").gatewayByKey("fix-a").orElseThrow();
        GatewayEndpoint b = load("test-app.toml").gatewayByKey("fix-b").orElseThrow();

        assertEquals(GatewayKind.FIX, a.kind());
        assertEquals(GatewayKind.FIX, b.kind());
        assertEquals(7777, a.port());
        assertEquals(7779, b.port());
    }

    @Test
    void anEndpointWithATlsPortOffersTls() {
        GatewayEndpoint endpoint = load("test-app.toml").gatewayByKey("fix-a").orElseThrow();

        assertTrue(endpoint.supportsTls());
        assertEquals(7778, endpoint.connectPort(true));
        assertEquals(7777, endpoint.connectPort(false));
    }

    @Test
    void anEndpointWithoutATlsPortNeverConnectsOverTls() {
        // Absent tls_port means no listener, not "share the plain port". Falling back to the
        // plain port would let a caller believe it had TLS when it had none.
        GatewayEndpoint binary = load("test-app.toml").gatewayByKey("binary-a").orElseThrow();

        assertFalse(binary.supportsTls());
        assertEquals(9999, binary.connectPort(true));
    }

    @Test
    void theLogonDialectIsAPropertyOfTheEndpoint() {
        // Proprietary is no longer a checkbox that redirects to another port: it IS another
        // endpoint, so the dialect and the address cannot disagree. Nanosecond timestamps
        // follow from this mode reaching FixApplication.
        Config config = load("test-app.toml");

        assertEquals(LogonMode.PROPRIETARY, config.gatewayByKey("fix-proprietary").orElseThrow().logonMode());
        assertEquals(LogonMode.STANDARD,    config.gatewayByKey("fix-a").orElseThrow().logonMode());
        // Plaintext by definition, so it must not offer TLS either.
        assertFalse(config.gatewayByKey("fix-proprietary").orElseThrow().supportsTls());
    }

    @Test
    void loadUsesDefaultsWhenFieldsAbsent() {
        Config config = load("minimal-app.toml");

        assertEquals(8081,                           config.serverPort());
        assertEquals("output",                       config.outputDir());
        assertEquals("scripts",                      config.scriptsDir());
        assertEquals("GATEWAY",                      config.targetCompId());
        assertEquals("config/fix_gateway_trust.jks", config.trustStorePath());
        assertEquals("pubsub_dev",                   config.trustStorePassword());
        assertEquals("",                             config.logoPath());
        // A gateway entry naming no label falls back to its key rather than showing blank.
        assertEquals("fix-a", config.defaultGateway().label());
    }

    @Test
    void aConfigWithNoGatewaysIsRejected() {
        // Deliberately fatal rather than defaulted. Inventing localhost:9879 would recreate
        // the exact bug the list exists to fix -- a single hardcoded endpoint that happens
        // to be instance a -- and would do it silently.
        IllegalStateException thrown = assertThrows(IllegalStateException.class, () -> load("empty-app.toml"));
        assertTrue(thrown.getMessage().contains("nothing to log on to"));
    }

    @Test
    void unknownGatewayKeyIsNotFound() {
        assertTrue(load("test-app.toml").gatewayByKey("fix-z").isEmpty());
    }
}
