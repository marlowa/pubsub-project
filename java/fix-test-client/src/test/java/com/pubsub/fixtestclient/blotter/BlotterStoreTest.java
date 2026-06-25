package com.pubsub.fixtestclient.blotter;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import quickfix.Message;
import quickfix.field.ClOrdID;
import quickfix.field.MsgSeqNum;
import quickfix.field.MsgType;
import quickfix.field.OrdStatus;
import quickfix.field.OrdType;
import quickfix.field.OrderQty;
import quickfix.field.Price;
import quickfix.field.Side;
import quickfix.field.Symbol;

import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

class BlotterStoreTest {

    private BlotterStore store;

    @BeforeEach
    void setUp() {
        store = new BlotterStore();
    }

    @Test
    void addInbound_storesRowWithInDirection() {
        store.addInbound(new Message());

        assertEquals("IN", store.rows().get(0).direction());
    }

    @Test
    void addOutbound_storesRowWithOutDirection() {
        store.addOutbound(new Message());

        assertEquals("OUT", store.rows().get(0).direction());
    }

    @Test
    void addInbound_extractsFieldsFromMessage() {
        Message message = new Message();
        message.getHeader().setInt(MsgSeqNum.FIELD, 7);
        message.setString(ClOrdID.FIELD, "ORD-001");
        message.setString(Symbol.FIELD, "BHP");
        message.setChar(Side.FIELD, '1');
        message.setDouble(OrderQty.FIELD, 100.0);
        message.setDouble(Price.FIELD, 10.50);
        message.setChar(OrdType.FIELD, '2');
        message.setChar(OrdStatus.FIELD, '0');

        store.addInbound(message);

        BlotterRow row = store.rows().get(0);
        assertEquals(7, row.seqNum());
        assertEquals("ORD-001", row.clOrdId());
        assertEquals("BHP", row.symbol());
        assertEquals("1", row.side());
        assertEquals("100", row.ordQty());
        assertEquals("10.5", row.price());
        assertEquals("2", row.ordType());
        assertEquals("0", row.ordStatus());
    }

    @Test
    void addInbound_missingFields_usesDefaults() {
        store.addInbound(new Message());

        BlotterRow row = store.rows().get(0);
        assertEquals(0, row.seqNum());
        assertEquals("", row.clOrdId());
        assertEquals("", row.symbol());
        assertEquals("", row.side());
    }

    @Test
    void addInbound_seqNumFallsBackToHeader() {
        Message message = new Message();
        message.getHeader().setInt(MsgSeqNum.FIELD, 42);

        store.addInbound(message);

        assertEquals(42, store.rows().get(0).seqNum());
    }

    @Test
    void multipleMessages_assignIncreasingIds() {
        store.addInbound(new Message());
        store.addInbound(new Message());
        store.addInbound(new Message());

        List<BlotterRow> rows = store.rows();
        assertTrue(rows.get(0).id() < rows.get(1).id());
        assertTrue(rows.get(1).id() < rows.get(2).id());
    }

    @Test
    void rows_returnsCopyNotLiveView() {
        store.addInbound(new Message());
        List<BlotterRow> snapshot = store.rows();

        store.addInbound(new Message());

        assertEquals(1, snapshot.size());
    }

    @Test
    void clear_removesAllRows() {
        store.addInbound(new Message());
        store.addInbound(new Message());

        store.clear();

        assertTrue(store.rows().isEmpty());
    }

    @Test
    void addInbound_rowsReturnedInInsertionOrder() {
        Message first = new Message();
        first.setString(ClOrdID.FIELD, "FIRST");
        Message second = new Message();
        second.setString(ClOrdID.FIELD, "SECOND");

        store.addInbound(first);
        store.addInbound(second);

        List<BlotterRow> rows = store.rows();
        assertEquals("FIRST",  rows.get(0).clOrdId());
        assertEquals("SECOND", rows.get(1).clOrdId());
    }

    @Test
    void rows_returnedListIsIndependentCopy() {
        store.addInbound(new Message());
        List<BlotterRow> rows1 = store.rows();
        List<BlotterRow> rows2 = store.rows();

        assertNotSame(rows1, rows2);
    }

    @Test
    void addInbound_messageTypeNotRequiredForBlotter() {
        Message message = new Message();
        message.getHeader().setString(MsgType.FIELD, MsgType.EXECUTION_REPORT);
        message.setString(ClOrdID.FIELD, "ORD-ER");

        store.addInbound(message);

        assertEquals("ORD-ER", store.rows().get(0).clOrdId());
    }
}
