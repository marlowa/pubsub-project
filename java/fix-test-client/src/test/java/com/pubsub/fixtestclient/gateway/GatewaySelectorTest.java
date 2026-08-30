package com.pubsub.fixtestclient.gateway;

import com.pubsub.fixtestclient.binary.BinaryEngine;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.LogonMode;
import com.pubsub.fixtestclient.fix.SessionStatus;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

/**
 * The selector decides which of the two engines everything after the logon page talks to,
 * so what matters is that it answers for the engine that is actually live and that it will
 * not switch a session out from under itself.
 */
class GatewaySelectorTest {

    private static final GatewayEndpoint FIX_A =
            new GatewayEndpoint("fix-a", "FIX a", GatewayKind.FIX, "localhost", 9876, 9877, LogonMode.STANDARD);
    private static final GatewayEndpoint FIX_B =
            new GatewayEndpoint("fix-b", "FIX b", GatewayKind.FIX, "localhost", 9878, 0, LogonMode.STANDARD);
    private static final GatewayEndpoint BINARY_A =
            new GatewayEndpoint("bin-a", "Binary a", GatewayKind.BINARY, "localhost", 9879, 0, LogonMode.STANDARD);

    private FixEngine fixEngine;
    private BinaryEngine binaryEngine;

    @BeforeEach
    void setUp() {
        fixEngine = mock(FixEngine.class);
        binaryEngine = mock(BinaryEngine.class);
    }

    private GatewaySelector selectorOn(GatewayEndpoint initial) {
        return new GatewaySelector(fixEngine, binaryEngine, initial);
    }

    @Test
    void active_isTheEndpointItWasBuiltWith() {
        GatewaySelector selector = selectorOn(FIX_A);

        assertEquals(FIX_A, selector.active());
        assertEquals(GatewayKind.FIX, selector.activeKind());
        assertFalse(selector.isBinaryActive());
    }

    @Test
    void isBinaryActive_isTrueOnlyForABinaryEndpoint() {
        assertTrue(selectorOn(BINARY_A).isBinaryActive());
    }

    @Test
    void engines_areTheOnesItWasBuiltWith() {
        GatewaySelector selector = selectorOn(FIX_A);

        assertSame(fixEngine, selector.fix());
        assertSame(binaryEngine, selector.binary());
    }

    @Test
    void isLoggedOn_asksTheEngineTheActiveEndpointBelongsTo() {
        when(fixEngine.isLoggedOn()).thenReturn(true);
        when(binaryEngine.isLoggedOn()).thenReturn(false);

        assertTrue(selectorOn(FIX_A).isLoggedOn());
        assertFalse(selectorOn(BINARY_A).isLoggedOn());
    }

    @Test
    void status_comesFromTheEngineTheActiveEndpointBelongsTo() {
        SessionStatus fixStatus = SessionStatus.disconnected("fix");
        SessionStatus binaryStatus = SessionStatus.disconnected("binary");
        when(fixEngine.getStatus()).thenReturn(fixStatus);
        when(binaryEngine.getStatus()).thenReturn(binaryStatus);

        assertSame(fixStatus, selectorOn(FIX_A).status());
        assertSame(binaryStatus, selectorOn(BINARY_A).status());
    }

    @Test
    void setActive_switchesWhenNoSessionIsLive() {
        GatewaySelector selector = selectorOn(FIX_A);

        assertEquals("", selector.setActive(BINARY_A));
        assertEquals(BINARY_A, selector.active());
    }

    @Test
    void setActive_acceptsTheEndpointAlreadySelected() {
        when(fixEngine.isLoggedOn()).thenReturn(true);
        GatewaySelector selector = selectorOn(FIX_A);

        // Logged on, but this asks for no change, so there is nothing to refuse.
        assertEquals("", selector.setActive(FIX_A));
        assertEquals(FIX_A, selector.active());
    }

    @Test
    void setActive_refusesToSwitchWhileASessionIsLive() {
        when(fixEngine.isLoggedOn()).thenReturn(true);
        GatewaySelector selector = selectorOn(FIX_A);

        assertEquals("log out of FIX a before switching", selector.setActive(FIX_B));
        assertEquals(FIX_A, selector.active());
    }
}
