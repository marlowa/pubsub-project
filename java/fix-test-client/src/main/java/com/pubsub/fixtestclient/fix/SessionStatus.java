package com.pubsub.fixtestclient.fix;

import java.time.Instant;

public record SessionStatus(
        boolean connected,
        boolean loggedOn,
        String senderCompId,
        String targetCompId,
        String host,
        int port,
        Instant logonTime,
        int startingSeqNum,
        int nextOutgoingSeqNum,
        int nextIncomingSeqNum,
        String lastError
) {
    public static SessionStatus disconnected() {
        return new SessionStatus(false, false, "", "", "", 0, null, 0, 0, 0, "");
    }
}
