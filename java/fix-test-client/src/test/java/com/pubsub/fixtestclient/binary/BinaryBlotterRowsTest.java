package com.pubsub.fixtestclient.binary;

import com.pubsub.fixtestclient.blotter.BlotterRow;
import com.pubsub.fixtestclient.protocol.FixOrders;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;

/**
 * The blotter shows the same columns whichever gateway the client is using, so what these
 * check is that a binary message lands in the columns a reader of the FIX blotter expects:
 * the FIX characters rather than the wire numbers behind them, and an empty cell wherever
 * the message did not carry the field.
 */
class BinaryBlotterRowsTest {

    private static FixOrders.NewOrderSingle minimalOrder() {
        FixOrders.NewOrderSingle order = new FixOrders.NewOrderSingle();
        order.cl_ord_id = "ORD-001";
        order.symbol = "BHP";
        order.side = FixOrders.Side.Buy;
        order.ord_type = FixOrders.OrdType.Limit;
        order.order_qty = "100";
        return order;
    }

    private static FixOrders.ExecutionReport minimalReport() {
        FixOrders.ExecutionReport report = new FixOrders.ExecutionReport();
        report.order_id = "OID-1";
        report.exec_id = "EID-1";
        report.exec_type = FixOrders.ExecType.New;
        report.ord_status = FixOrders.OrdStatus.New;
        report.symbol = "BHP";
        report.side = FixOrders.Side.Sell;
        report.cum_qty = "0";
        report.leaves_qty = "100";
        return report;
    }

    @Test
    void fromOrder_fillsTheColumnsAnOrderCarries() {
        BlotterRow row = BinaryBlotterRows.fromOrder(7L, minimalOrder());

        assertEquals(7L, row.id());
        assertEquals("OUT", row.direction());
        assertEquals("D", row.msgType());
        assertEquals("ORD-001", row.clOrdId());
        assertEquals("BHP", row.symbol());
        assertEquals("100", row.ordQty());
    }

    @Test
    void fromOrder_showsTheFixCharacterRatherThanTheWireValue() {
        // The schema gives a char enum the ASCII code as its value, so Side.Buy is 49.
        BlotterRow row = BinaryBlotterRows.fromOrder(1L, minimalOrder());

        assertEquals("1", row.side());
        assertEquals("2", row.ordType());
    }

    @Test
    void fromOrder_leavesAbsentFieldsEmpty() {
        BlotterRow row = BinaryBlotterRows.fromOrder(1L, minimalOrder());

        assertEquals("", row.price());
        assertEquals("", row.securityId());
        assertEquals("", row.timeInForce());
        assertEquals("", row.expireTime());
        assertEquals("", row.parties());
        assertEquals("", row.underlyings());
    }

    @Test
    void fromOrder_showsPresentOptionalFields() {
        FixOrders.NewOrderSingle order = minimalOrder();
        order.has_price = true;
        order.price = "42.50";
        order.has_security_id = true;
        order.security_id = "AU000000BHP4";
        order.has_time_in_force = true;
        order.time_in_force = FixOrders.TimeInForce.GoodTillDate;

        BlotterRow row = BinaryBlotterRows.fromOrder(1L, order);

        assertEquals("42.50", row.price());
        assertEquals("AU000000BHP4", row.securityId());
        assertEquals("6", row.timeInForce());
    }

    @Test
    void fromOrder_showsAnExpiryToTheSecond() {
        FixOrders.NewOrderSingle order = minimalOrder();
        order.has_expire_time = true;
        // 2026-08-30 12:34:56 UTC, with a fraction the display is meant to drop.
        order.expire_time = 1_788_093_296L * 1_000_000_000L + 123_456_789L;

        BlotterRow row = BinaryBlotterRows.fromOrder(1L, order);

        assertEquals("20260830-12:34:56", row.expireTime());
    }

    @Test
    void fromOrder_summarisesEachPartyAsIdSourceRole() {
        FixOrders.PartyIDs full = new FixOrders.PartyIDs();
        full.has_party_id = true;
        full.party_id = "FIRM-A";
        full.has_party_id_source = true;
        full.party_id_source = FixOrders.PartyIDSource.UkNationalInsuranceOrPensionNumber;
        full.has_party_role = true;
        full.party_role = FixOrders.PartyRole.ExecutingFirm;

        FixOrders.PartyIDs idOnly = new FixOrders.PartyIDs();
        idOnly.has_party_id = true;
        idOnly.party_id = "FIRM-B";

        FixOrders.NewOrderSingle order = minimalOrder();
        order.no_party_i_ds = new FixOrders.PartyIDs[] {full, idOnly};

        assertEquals("FIRM-A/6/1, FIRM-B", BinaryBlotterRows.fromOrder(1L, order).parties());
    }

    @Test
    void fromOrder_summarisesEachUnderlyingAsSymbolIdQuantity() {
        FixOrders.Underlyings full = new FixOrders.Underlyings();
        full.has_underlying_symbol = true;
        full.underlying_symbol = "BHP";
        full.has_underlying_security_id = true;
        full.underlying_security_id = "AU000000BHP4";
        full.has_underlying_qty = true;
        full.underlying_qty = "50";

        FixOrders.Underlyings symbolOnly = new FixOrders.Underlyings();
        symbolOnly.has_underlying_symbol = true;
        symbolOnly.underlying_symbol = "RIO";

        FixOrders.NewOrderSingle order = minimalOrder();
        order.no_underlyings = new FixOrders.Underlyings[] {full, symbolOnly};

        assertEquals("BHP/AU000000BHP4/50, RIO", BinaryBlotterRows.fromOrder(1L, order).underlyings());
    }

    @Test
    void fromCancel_fillsTheColumnsACancelCarries() {
        FixOrders.OrderCancelRequest cancel = new FixOrders.OrderCancelRequest();
        cancel.cl_ord_id = "CXL-001";
        cancel.orig_cl_ord_id = "ORD-001";
        cancel.symbol = "BHP";
        cancel.side = FixOrders.Side.Sell;
        cancel.order_qty = "100";

        BlotterRow row = BinaryBlotterRows.fromCancel(3L, cancel);

        assertEquals("OUT", row.direction());
        assertEquals("F", row.msgType());
        assertEquals("CXL-001", row.clOrdId());
        assertEquals("ORD-001", row.origClOrdId());
        assertEquals("2", row.side());
        assertEquals("100", row.ordQty());
    }

    @Test
    void fromReport_readsTheGatewaysAnswerIntoTheRow() {
        BlotterRow row = BinaryBlotterRows.fromReport(9L, minimalReport());

        assertEquals("IN", row.direction());
        assertEquals("8", row.msgType());
        assertEquals("OID-1", row.orderId());
        assertEquals("EID-1", row.execId());
        assertEquals("0", row.execType());
        assertEquals("0", row.ordStatus());
        assertEquals("0", row.cumQty());
        assertEquals("100", row.leavesQty());
    }

    @Test
    void fromReport_leavesAbsentFieldsEmpty() {
        BlotterRow row = BinaryBlotterRows.fromReport(1L, minimalReport());

        assertEquals("", row.clOrdId());
        assertEquals("", row.ordRejReason());
        assertEquals("", row.ordQty());
        assertEquals("", row.ordType());
        assertEquals("", row.expireTime());
    }

    @Test
    void fromReport_showsARejectionReasonAsItsNumber() {
        FixOrders.ExecutionReport report = minimalReport();
        report.has_cl_ord_id = true;
        report.cl_ord_id = "ORD-001";
        report.has_ord_rej_reason = true;
        report.ord_rej_reason = FixOrders.OrdRejReason.UnknownSymbol;
        report.has_ord_type = true;
        report.ord_type = FixOrders.OrdType.Market;
        report.has_order_qty = true;
        report.order_qty = "100";
        report.has_price = true;
        report.price = "42.50";

        BlotterRow row = BinaryBlotterRows.fromReport(1L, report);

        assertEquals("ORD-001", row.clOrdId());
        assertEquals("1", row.ordRejReason());
        assertEquals("1", row.ordType());
        assertEquals("100", row.ordQty());
        assertEquals("42.50", row.price());
    }

}
