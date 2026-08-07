package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.Config;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.verify;

class FixSessionBindingTest {

    private FixEngine fixEngine;
    private FixSessionBinding binding;
    private Config config;

    private GatewayEndpoint endpoint(String key) {
        return config.gatewayByKey(key).orElseThrow();
    }

    @BeforeEach
    void setUp() {
        fixEngine = mock(FixEngine.class);
        config = Config.load(getClass().getClassLoader().getResource("test-app.toml").getPath());
        binding = new FixSessionBinding(fixEngine, config);
    }

    @Test
    void logon_usesTheFirstStandardEndpointAndDefaultTarget() throws Exception {
        binding.logon("APM001", "secret", true);

        verify(fixEngine).logon(endpoint("fix-a"), "APM001", null, "secret", true);
    }

    @Test
    void logon_withTargetOverride_usesTheFirstStandardEndpoint() throws Exception {
        binding.logon("APM001", "ALTGW", "secret", false);

        verify(fixEngine).logon(endpoint("fix-a"), "APM001", "ALTGW", "secret", false);
    }

    @Test
    void logonTo_reachesASecondInstanceOfTheSameProtocol() throws Exception {
        // The reason this overload exists: a script that could only ever reach the first
        // FIX gateway could not drive a failover, nor provoke the refusal a gateway gives a
        // session provisioned for another instance.
        binding.logonTo("fix-b", "APM001", null, "secret", false);

        verify(fixEngine).logon(endpoint("fix-b"), "APM001", null, "secret", false);
    }

    @Test
    void logonTo_rejectsAGatewayThatIsNotConfigured() {
        // Named endpoints are checked against configuration rather than passed through: a
        // typo should fail here, not as an unexplained connection refusal later.
        assertThrows(IllegalArgumentException.class,
                     () -> binding.logonTo("fix-z", "APM001", null, "secret", false));
    }

    @Test
    void logonProprietary_usesTheProprietaryEndpointWithoutTls() throws Exception {
        // Picks the endpoint by its dialect, so the nanosecond-timestamp mode and the
        // address it belongs to cannot come apart.
        binding.logonProprietary("APM001", "secret");

        verify(fixEngine).logon(endpoint("fix-proprietary"), "APM001", null, "secret", false);
    }

    @Test
    void logonProprietary_withTargetOverride_usesTheProprietaryEndpointWithoutTls() throws Exception {
        binding.logonProprietary("APM001", "ALTGW", "secret");

        verify(fixEngine).logon(endpoint("fix-proprietary"), "APM001", "ALTGW", "secret", false);
    }
}
