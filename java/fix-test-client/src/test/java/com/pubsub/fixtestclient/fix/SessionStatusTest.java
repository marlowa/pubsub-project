package com.pubsub.fixtestclient.fix;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;

/**
 * The status the web page polls. A disconnected status is the one the page starts from and
 * the one it falls back to, so it has to be empty in every field rather than stale.
 */
class SessionStatusTest {

    @Test
    void disconnected_isEmptyInEveryField() {
        SessionStatus status = SessionStatus.disconnected();

        assertFalse(status.connected());
        assertFalse(status.loggingOn());
        assertFalse(status.loggedOn());
        assertEquals("", status.senderCompId());
        assertEquals("", status.targetCompId());
        assertEquals("", status.host());
        assertEquals(0, status.port());
        assertNull(status.logonTime());
        assertEquals(0, status.startingSeqNum());
        assertEquals(0, status.nextOutgoingSeqNum());
        assertEquals(0, status.nextIncomingSeqNum());
        assertEquals("", status.lastError());
        assertEquals(0, status.suggestedSeqNum());
    }

    @Test
    void disconnected_keepsTheReasonItWasGiven() {
        assertEquals("connection refused", SessionStatus.disconnected("connection refused").lastError());
    }
}
