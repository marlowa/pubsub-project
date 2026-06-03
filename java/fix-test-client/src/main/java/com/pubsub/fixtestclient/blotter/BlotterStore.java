package com.pubsub.fixtestclient.blotter;

import quickfix.FieldNotFound;
import quickfix.Message;
import quickfix.field.ClOrdID;
import quickfix.field.CumQty;
import quickfix.field.ExecID;
import quickfix.field.ExecType;
import quickfix.field.LeavesQty;
import quickfix.field.MsgSeqNum;

import quickfix.field.CxlRejReason;
import quickfix.field.OrderID;
import quickfix.field.OrderQty;
import quickfix.field.OrigClOrdID;
import quickfix.field.OrdRejReason;
import quickfix.field.OrdStatus;
import quickfix.field.OrdType;
import quickfix.field.Price;
import quickfix.field.Side;
import quickfix.field.Symbol;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicLong;

public class BlotterStore {

    private final CopyOnWriteArrayList<BlotterRow> rows = new CopyOnWriteArrayList<>();
    private final AtomicLong nextId = new AtomicLong(1);

    public void addOutbound(Message message) {
        rows.add(buildRow("OUT", message));
    }

    public void addInbound(Message message) {
        rows.add(buildRow("IN", message));
    }

    public List<BlotterRow> rows() {
        return new ArrayList<>(rows);
    }

    public void clear() {
        rows.clear();
    }

    private BlotterRow buildRow(String direction, Message message) {
        return new BlotterRow(
                nextId.getAndIncrement(),
                Instant.now(),
                direction,
                getInt(message, MsgSeqNum.FIELD),
                getString(message, ClOrdID.FIELD),
                getString(message, OrigClOrdID.FIELD),
                getString(message, OrderID.FIELD),
                getString(message, ExecID.FIELD),
                getChar(message, ExecType.FIELD),
                getChar(message, OrdStatus.FIELD),
                getString(message, OrdRejReason.FIELD),
                getString(message, CxlRejReason.FIELD),
                getString(message, Symbol.FIELD),
                getChar(message, Side.FIELD),
                getDecimal(message, OrderQty.FIELD),
                getDecimal(message, Price.FIELD),
                getChar(message, OrdType.FIELD),
                getDecimal(message, CumQty.FIELD),
                getDecimal(message, LeavesQty.FIELD)
        );
    }

    private String getString(Message message, int tag) {
        try {
            return message.getString(tag);
        } catch (FieldNotFound e) {
            return "";
        }
    }

    private String getChar(Message message, int tag) {
        try {
            return String.valueOf(message.getChar(tag));
        } catch (FieldNotFound e) {
            return "";
        }
    }

    private String getDecimal(Message message, int tag) {
        try {
            return message.getDecimal(tag).toPlainString();
        } catch (FieldNotFound e) {
            return "";
        }
    }

    private int getInt(Message message, int tag) {
        try {
            return message.getInt(tag);
        } catch (FieldNotFound e) {
            try {
                return message.getHeader().getInt(tag);
            } catch (FieldNotFound e2) {
                return 0;
            }
        }
    }
}
