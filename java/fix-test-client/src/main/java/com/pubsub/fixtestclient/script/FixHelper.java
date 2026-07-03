package com.pubsub.fixtestclient.script;

import quickfix.fix50sp2.NewOrderSingle;
import quickfix.fix50sp2.OrderCancelRequest;
import quickfix.field.HandlInst;
import quickfix.field.TransactTime;

import java.time.LocalDateTime;
import java.util.concurrent.atomic.AtomicLong;

public class FixHelper {

    private final AtomicLong counter = new AtomicLong(0);

    /**
     * Returns a unique string suitable for use as a ClOrdID.
     *
     * Format: {@code <epoch-millis>-<sequence>}, e.g. {@code 1751234567890-3}.
     * The millisecond timestamp changes between script runs; the sequence counter
     * increments within and across runs, so IDs are guaranteed unique for the
     * lifetime of the process.
     */
    public String uniqueId() {
        return System.currentTimeMillis() + "-" + counter.getAndIncrement();
    }

    public NewOrderSingle newOrderSingle() {
        NewOrderSingle nos = new NewOrderSingle();
        nos.set(new HandlInst(HandlInst.AUTOMATED_EXECUTION_ORDER_PRIVATE_NO_BROKER_INTERVENTION));
        nos.set(new TransactTime(LocalDateTime.now()));
        return nos;
    }

    public OrderCancelRequest orderCancelRequest() {
        OrderCancelRequest ocr = new OrderCancelRequest();
        ocr.set(new TransactTime(LocalDateTime.now()));
        return ocr;
    }
}
