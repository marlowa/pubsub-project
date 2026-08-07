package com.pubsub.fixtestclient.blotter;

import java.time.Instant;

public record BlotterRow(
        long id,
        Instant time,
        String direction,
        String msgType,
        int seqNum,
        String clOrdId,
        String origClOrdId,
        String orderId,
        String execId,
        String execType,
        String ordStatus,
        String ordRejReason,
        String cxlRejReason,
        String securityId,
        String symbol,
        String side,
        String ordQty,
        String price,
        String ordType,
        String cumQty,
        String leavesQty,
        String parties,
        String underlyings,
        /**
         * The order's time-in-force terms, as the venue reported them back.
         *
         * Appended rather than slotted in beside ordType where they belong logically: every
         * component of this record is a String, so a positional mistake at one of the three
         * construction sites would compile cleanly and show the wrong column. Display order
         * is decided in messages.html, not here.
         *
         * expireTime is the venue's own statement of when a GoodTillDate order dies. It is
         * shown because a member cannot otherwise tell which expiry convention the venue
         * follows -- several venues adjust a GTD expiry, and this one does not.
         */
        String timeInForce,
        String expireTime
) {
}
