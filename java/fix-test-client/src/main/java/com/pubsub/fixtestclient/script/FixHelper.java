package com.pubsub.fixtestclient.script;

import quickfix.fix50sp2.NewOrderSingle;
import quickfix.field.HandlInst;
import quickfix.field.TransactTime;

import java.time.LocalDateTime;

public class FixHelper {

    public NewOrderSingle newOrderSingle() {
        NewOrderSingle nos = new NewOrderSingle();
        nos.set(new HandlInst(HandlInst.AUTOMATED_EXECUTION_ORDER_PRIVATE_NO_BROKER_INTERVENTION));
        nos.set(new TransactTime(LocalDateTime.now()));
        return nos;
    }
}
