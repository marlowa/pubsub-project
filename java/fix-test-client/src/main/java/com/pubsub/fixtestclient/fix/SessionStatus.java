package com.pubsub.fixtestclient.fix;

import java.time.Instant;

public record SessionStatus(
        boolean connected,
        boolean loggingOn,
        boolean loggedOn,
        String senderCompId,
        String targetCompId,
        String host,
        int port,
        Instant logonTime,
        int startingSeqNum,
        int nextOutgoingSeqNum,
        int nextIncomingSeqNum,
        String lastError,
        int suggestedSeqNum
) {
    public static SessionStatus disconnected(String lastError) {
        return new SessionStatus(false, false, false, "", "", "", 0, null, 0, 0, 0, lastError, 0);
    }

    public static SessionStatus disconnected() {
        return disconnected("");
    }
}
