package com.pubsub.fixtestclient.blotter;

import java.time.Instant;

public record BlotterRow(
        long id,
        Instant time,
        String direction,
        int seqNum,
        String clOrdId,
        String origClOrdId,
        String orderId,
        String execId,
        String execType,
        String ordStatus,
        String ordRejReason,
        String cxlRejReason,
        String symbol,
        String side,
        String ordQty,
        String price,
        String ordType,
        String cumQty,
        String leavesQty
) {
}
