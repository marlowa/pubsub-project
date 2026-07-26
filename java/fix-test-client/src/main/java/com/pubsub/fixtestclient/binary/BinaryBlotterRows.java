package com.pubsub.fixtestclient.binary;

import com.pubsub.fixtestclient.blotter.BlotterRow;
import com.pubsub.fixtestclient.protocol.FixOrders;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;

/**
 * Turns binary-gateway messages into blotter rows.
 *
 * The blotter is protocol-agnostic by design -- a row is a set of order fields, not a FIX
 * message -- so the same table shows traffic from either gateway. What differs is only where
 * the fields are read from: QuickFIX tag lookups for the FIX session, generated struct fields
 * here. The MsgType column keeps the FIX letters ("D", "F", "8") because they name the
 * message types the venue deals in regardless of how they were carried.
 */
public final class BinaryBlotterRows {

    private BinaryBlotterRows() {
    }

    public static BlotterRow fromOrder(long id, FixOrders.NewOrderSingle order) {
        return new BlotterRow(
                id, Instant.now(), "OUT", "D", 0,
                order.cl_ord_id, "", "", "", "", "", "", "",
                order.has_security_id ? order.security_id : "",
                order.symbol,
                asFixChar(order.side.value),
                order.order_qty,
                order.has_price ? order.price : "",
                asFixChar(order.ord_type.value),
                "", "",
                summariseParties(order.no_party_i_ds),
                summariseUnderlyings(order.no_underlyings));
    }

    public static BlotterRow fromCancel(long id, FixOrders.OrderCancelRequest cancel) {
        return new BlotterRow(
                id, Instant.now(), "OUT", "F", 0,
                cancel.cl_ord_id, cancel.orig_cl_ord_id, "", "", "", "", "", "",
                "", cancel.symbol,
                asFixChar(cancel.side.value),
                cancel.order_qty, "", "", "", "", "", "");
    }

    public static BlotterRow fromReport(long id, FixOrders.ExecutionReport report) {
        return new BlotterRow(
                id, Instant.now(), "IN", "8", 0,
                report.has_cl_ord_id ? report.cl_ord_id : "",
                "",
                report.order_id,
                report.exec_id,
                asFixChar(report.exec_type.value),
                asFixChar(report.ord_status.value),
                report.has_ord_rej_reason ? String.valueOf(report.ord_rej_reason.value) : "",
                "",
                report.has_security_id ? report.security_id : "",
                report.symbol,
                asFixChar(report.side.value),
                report.has_order_qty ? report.order_qty : "",
                report.has_price ? report.price : "",
                report.has_ord_type ? asFixChar(report.ord_type.value) : "",
                report.cum_qty,
                report.leaves_qty,
                summariseParties(report.no_party_i_ds),
                summariseUnderlyings(report.no_underlyings));
    }

    // Repeating groups render as a compact one-line summary, matching what the FIX blotter
    // shows: the full detail belongs in the topic probe, not a table cell.

    /**
     * Renders a char-enum's wire value as the FIX character it stands for.
     *
     * The DSL gives char enums the ASCII code as their value, so Side.Buy is 49 rather than
     * '1'. The blotter should show what a FIX reader would recognise.
     */
    private static String asFixChar(int wireValue) {
        return wireValue >= 32 && wireValue < 127 ? String.valueOf((char) wireValue) : String.valueOf(wireValue);
    }

    private static String summariseParties(FixOrders.PartyIDs[] parties) {
        if (parties == null || parties.length == 0) {
            return "";
        }
        List<String> parts = new ArrayList<>();
        for (FixOrders.PartyIDs party : parties) {
            StringBuilder text = new StringBuilder(party.has_party_id ? party.party_id : "");
            if (party.has_party_id_source) {
                text.append('/').append(asFixChar(party.party_id_source.value));
            }
            if (party.has_party_role) {
                text.append('/').append(String.valueOf(party.party_role.value));
            }
            parts.add(text.toString());
        }
        return String.join(", ", parts);
    }

    private static String summariseUnderlyings(FixOrders.Underlyings[] underlyings) {
        if (underlyings == null || underlyings.length == 0) {
            return "";
        }
        List<String> parts = new ArrayList<>();
        for (FixOrders.Underlyings underlying : underlyings) {
            StringBuilder text = new StringBuilder(underlying.has_underlying_symbol ? underlying.underlying_symbol : "");
            if (underlying.has_underlying_security_id) {
                text.append('/').append(underlying.underlying_security_id);
            }
            if (underlying.has_underlying_qty) {
                text.append('/').append(underlying.underlying_qty);
            }
            parts.add(text.toString());
        }
        return String.join(", ", parts);
    }
}
