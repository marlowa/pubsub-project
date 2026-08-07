package com.pubsub.fixtestclient.blotter;

import quickfix.FieldMap;
import quickfix.FieldNotFound;
import quickfix.Group;
import quickfix.Message;
import quickfix.field.ClOrdID;
import quickfix.field.CumQty;
import quickfix.field.ExecID;
import quickfix.field.ExecType;
import quickfix.field.ExpireTime;
import quickfix.field.LeavesQty;
import quickfix.field.MsgSeqNum;
import quickfix.field.MsgType;

import quickfix.field.CxlRejReason;
import quickfix.field.NoPartyIDs;
import quickfix.field.NoUnderlyings;
import quickfix.field.OrderID;
import quickfix.field.OrderQty;
import quickfix.field.OrigClOrdID;
import quickfix.field.OrdRejReason;
import quickfix.field.OrdStatus;
import quickfix.field.OrdType;
import quickfix.field.PartyID;
import quickfix.field.PartyIDSource;
import quickfix.field.PartyRole;
import quickfix.field.Price;
import quickfix.field.SecurityID;
import quickfix.field.Side;
import quickfix.field.Symbol;
import quickfix.field.TimeInForce;
import quickfix.field.UnderlyingQty;
import quickfix.field.UnderlyingSecurityID;
import quickfix.field.UnderlyingSymbol;

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

    /**
     * Appends a row built elsewhere.
     *
     * The FIX path builds rows here from QuickFIX messages; the binary path builds them from
     * generated PDU structs, which this class has no business knowing about. A blotter row is
     * a set of order fields rather than a FIX message, so both belong in the same table.
     */
    public void add(BlotterRow row) {
        rows.add(row);
    }

    /** Claims the next row id, for callers that build their own rows. */
    public long nextRowId() {
        return nextId.getAndIncrement();
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
                getMsgType(message),
                getInt(message, MsgSeqNum.FIELD),
                getString(message, ClOrdID.FIELD),
                getString(message, OrigClOrdID.FIELD),
                getString(message, OrderID.FIELD),
                getString(message, ExecID.FIELD),
                getChar(message, ExecType.FIELD),
                getChar(message, OrdStatus.FIELD),
                getString(message, OrdRejReason.FIELD),
                getString(message, CxlRejReason.FIELD),
                getString(message, SecurityID.FIELD),
                getString(message, Symbol.FIELD),
                getChar(message, Side.FIELD),
                getDecimal(message, OrderQty.FIELD),
                getDecimal(message, Price.FIELD),
                getChar(message, OrdType.FIELD),
                getDecimal(message, CumQty.FIELD),
                getDecimal(message, LeavesQty.FIELD),
                summarizeParties(message),
                summarizeUnderlyings(message),
                getChar(message, TimeInForce.FIELD),
                getString(message, ExpireTime.FIELD)
        );
    }

    // Repeating groups are rendered as a compact one-line summary per row (the topic
    // probe carries the full detail). A NewOrderSingle carries both groups; an echoed
    // ExecutionReport carries NoUnderlyings.

    private String summarizeParties(Message message) {
        List<String> parts = new ArrayList<>();
        for (Group group : message.getGroups(NoPartyIDs.FIELD)) {
            String id = getString(group, PartyID.FIELD);
            String source = getChar(group, PartyIDSource.FIELD);
            String role = getString(group, PartyRole.FIELD);
            parts.add(id + slash(source) + slash(role));
        }
        return String.join(", ", parts);
    }

    private String summarizeUnderlyings(Message message) {
        List<String> parts = new ArrayList<>();
        for (Group group : message.getGroups(NoUnderlyings.FIELD)) {
            String symbol = getString(group, UnderlyingSymbol.FIELD);
            String securityId = getString(group, UnderlyingSecurityID.FIELD);
            String qty = getDecimal(group, UnderlyingQty.FIELD);
            String base = symbol.isEmpty() ? securityId : symbol;
            parts.add(base + slash(qty));
        }
        return String.join(", ", parts);
    }

    private static String slash(String value) {
        return value.isEmpty() ? "" : "/" + value;
    }

    private String getString(FieldMap map, int tag) {
        try {
            return map.getString(tag);
        } catch (FieldNotFound e) {
            return "";
        }
    }

    private String getChar(FieldMap map, int tag) {
        try {
            return String.valueOf(map.getChar(tag));
        } catch (FieldNotFound e) {
            return "";
        }
    }

    private String getDecimal(FieldMap map, int tag) {
        try {
            return map.getDecimal(tag).toPlainString();
        } catch (FieldNotFound e) {
            return "";
        }
    }

    /** @return the raw MsgType (tag 35) from the header, e.g. "D"/"F"/"8"; labelled client-side. */
    private String getMsgType(Message message) {
        try {
            return message.getHeader().getString(MsgType.FIELD);
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
