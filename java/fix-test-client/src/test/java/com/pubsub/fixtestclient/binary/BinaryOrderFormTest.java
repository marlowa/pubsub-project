package com.pubsub.fixtestclient.binary;

import com.pubsub.fixtestclient.protocol.FixOrders;
import io.javalin.http.Context;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

/**
 * The web order form posts the same fields whichever gateway is live, so these check the
 * translation from those fields to the binary order: what an empty field means, what an
 * unrecognised value falls back to, and how the repeating groups are paired up by row.
 */
class BinaryOrderFormTest {

    private Context ctx;
    private Map<String, String> params;
    private Map<String, List<String>> repeated;

    @BeforeEach
    void setUp() {
        ctx = mock(Context.class);
        params = new HashMap<>();
        repeated = new HashMap<>();
        when(ctx.formParam(org.mockito.ArgumentMatchers.anyString()))
                .thenAnswer(invocation -> params.get(invocation.getArgument(0, String.class)));
        when(ctx.formParams(org.mockito.ArgumentMatchers.anyString()))
                .thenAnswer(invocation -> repeated.getOrDefault(invocation.getArgument(0, String.class), List.of()));
    }

    private void form(String name, String value) {
        params.put(name, value);
    }

    private void column(String name, String... values) {
        repeated.put(name, Arrays.asList(values));
    }

    private void minimalForm() {
        form("symbol", "BHP");
        form("side", "1");
        form("ordType", "2");
        form("qty", "100");
    }

    @Test
    void buildOrder_readsTheFieldsEveryOrderHas() {
        minimalForm();

        FixOrders.NewOrderSingle order = BinaryOrderForm.buildOrder(ctx, "ORD-001");

        assertEquals("ORD-001", order.cl_ord_id);
        assertEquals("BHP", order.symbol);
        assertEquals(FixOrders.Side.Buy, order.side);
        assertEquals(FixOrders.OrdType.Limit, order.ord_type);
        assertEquals("100", order.order_qty);
        assertTrue(order.transact_time > 0L);
    }

    @Test
    void buildOrder_trimsWhatTheFormPosted() {
        minimalForm();
        form("symbol", "  BHP  ");
        form("side", " 2 ");

        FixOrders.NewOrderSingle order = BinaryOrderForm.buildOrder(ctx, "ORD-001");

        assertEquals("BHP", order.symbol);
        assertEquals(FixOrders.Side.Sell, order.side);
    }

    @Test
    void buildOrder_leavesAnEmptyFieldUnset() {
        minimalForm();
        form("price", "   ");

        FixOrders.NewOrderSingle order = BinaryOrderForm.buildOrder(ctx, "ORD-001");

        assertFalse(order.has_price);
        assertFalse(order.has_security_id);
        assertFalse(order.has_stop_px);
        assertFalse(order.has_min_qty);
        assertFalse(order.has_max_floor);
        assertFalse(order.has_account);
        assertFalse(order.has_ex_destination);
        assertFalse(order.has_exec_inst);
        assertFalse(order.has_text);
        assertFalse(order.has_time_in_force);
    }

    @Test
    void buildOrder_setsEveryOptionalFieldTheFormFilledIn() {
        minimalForm();
        form("price", "42.50");
        form("securityId", "AU000000BHP4");
        form("securityIdSource", "4");
        form("stopPx", "41.00");
        form("minQty", "10");
        form("maxFloor", "20");
        form("account", "ACC-1");
        form("exDestination", "XASX");
        form("execInst", "G");
        form("text", "a note");

        FixOrders.NewOrderSingle order = BinaryOrderForm.buildOrder(ctx, "ORD-001");

        assertTrue(order.has_price);
        assertEquals("42.50", order.price);
        assertTrue(order.has_security_id);
        assertEquals("AU000000BHP4", order.security_id);
        assertTrue(order.has_security_id_source);
        assertEquals("4", order.security_id_source);
        assertTrue(order.has_stop_px);
        assertEquals("41.00", order.stop_px);
        assertTrue(order.has_min_qty);
        assertEquals("10", order.min_qty);
        assertTrue(order.has_max_floor);
        assertEquals("20", order.max_floor);
        assertTrue(order.has_account);
        assertEquals("ACC-1", order.account);
        assertTrue(order.has_ex_destination);
        assertEquals("XASX", order.ex_destination);
        assertTrue(order.has_exec_inst);
        assertEquals("G", order.exec_inst);
        assertTrue(order.has_text);
        assertEquals("a note", order.text);
    }

    @Test
    void buildOrder_fallsBackToBuyAndLimitWhenTheFormSaysNothingUsable() {
        form("symbol", "BHP");
        form("qty", "100");

        FixOrders.NewOrderSingle absent = BinaryOrderForm.buildOrder(ctx, "ORD-001");
        assertEquals(FixOrders.Side.Buy, absent.side);
        assertEquals(FixOrders.OrdType.Limit, absent.ord_type);

        form("side", "z");
        form("ordType", "z");
        FixOrders.NewOrderSingle unrecognised = BinaryOrderForm.buildOrder(ctx, "ORD-002");
        assertEquals(FixOrders.Side.Buy, unrecognised.side);
        assertEquals(FixOrders.OrdType.Limit, unrecognised.ord_type);
    }

    @Test
    void buildOrder_setsTimeInForceOnlyWhenTheValueIsRecognised() {
        minimalForm();
        form("timeInForce", "6");

        FixOrders.NewOrderSingle known = BinaryOrderForm.buildOrder(ctx, "ORD-001");
        assertTrue(known.has_time_in_force);
        assertEquals(FixOrders.TimeInForce.GoodTillDate, known.time_in_force);

        form("timeInForce", "z");
        assertFalse(BinaryOrderForm.buildOrder(ctx, "ORD-002").has_time_in_force);
    }

    @Test
    void buildOrder_pairsTheGroupColumnsByRow() {
        minimalForm();
        column("underlyingSymbol", "BHP", "RIO");
        column("underlyingSecurityId", "AU000000BHP4", "AU000000RIO1");
        column("underlyingQty", "50", "60");

        FixOrders.Underlyings[] underlyings = BinaryOrderForm.buildOrder(ctx, "ORD-001").no_underlyings;

        assertEquals(2, underlyings.length);
        assertEquals("BHP", underlyings[0].underlying_symbol);
        assertEquals("50", underlyings[0].underlying_qty);
        assertEquals("RIO", underlyings[1].underlying_symbol);
        assertEquals("AU000000RIO1", underlyings[1].underlying_security_id);
    }

    @Test
    void buildOrder_dropsABlankGroupRow() {
        minimalForm();
        column("underlyingSymbol", "BHP", "  ");
        column("underlyingSecurityId", "AU000000BHP4", "");
        column("underlyingQty", "50", "");
        column("partyId", "", "FIRM-B");
        column("partyIdSource", "", "");
        column("partyRole", "", "");

        FixOrders.NewOrderSingle order = BinaryOrderForm.buildOrder(ctx, "ORD-001");

        assertEquals(1, order.no_underlyings.length);
        assertEquals("BHP", order.no_underlyings[0].underlying_symbol);
        assertEquals(1, order.no_party_i_ds.length);
        assertEquals("FIRM-B", order.no_party_i_ds[0].party_id);
    }

    @Test
    void buildOrder_pairsAGroupRowShorterThanTheOthers() {
        minimalForm();
        column("partyId", "FIRM-A", "FIRM-B");
        column("partyIdSource", "6");
        column("partyRole", "1");

        FixOrders.PartyIDs[] parties = BinaryOrderForm.buildOrder(ctx, "ORD-001").no_party_i_ds;

        assertEquals(2, parties.length);
        assertTrue(parties[0].has_party_id_source);
        assertEquals(FixOrders.PartyIDSource.UkNationalInsuranceOrPensionNumber, parties[0].party_id_source);
        assertTrue(parties[0].has_party_role);
        assertEquals(FixOrders.PartyRole.ExecutingFirm, parties[0].party_role);
        assertEquals("FIRM-B", parties[1].party_id);
        assertFalse(parties[1].has_party_id_source);
        assertFalse(parties[1].has_party_role);
    }

    @Test
    void buildOrder_leavesAnUnrecognisedPartySourceOrRoleUnset() {
        minimalForm();
        column("partyId", "FIRM-A");
        column("partyIdSource", "z");
        column("partyRole", "9999");

        FixOrders.PartyIDs[] parties = BinaryOrderForm.buildOrder(ctx, "ORD-001").no_party_i_ds;

        assertEquals(1, parties.length);
        assertFalse(parties[0].has_party_id_source);
        assertFalse(parties[0].has_party_role);
    }

    @Test
    void buildOrder_hasNoGroupsWhenTheFormPostedNoRows() {
        minimalForm();

        FixOrders.NewOrderSingle order = BinaryOrderForm.buildOrder(ctx, "ORD-001");

        assertNotNull(order.no_underlyings);
        assertEquals(0, order.no_underlyings.length);
        assertEquals(0, order.no_party_i_ds.length);
    }

    @Test
    void buildCancel_readsTheOrderItCancels() {
        minimalForm();
        form("origClOrdId", " ORD-001 ");
        form("side", "2");

        FixOrders.OrderCancelRequest cancel = BinaryOrderForm.buildCancel(ctx, "CXL-001");

        assertEquals("CXL-001", cancel.cl_ord_id);
        assertEquals("ORD-001", cancel.orig_cl_ord_id);
        assertEquals("BHP", cancel.symbol);
        assertEquals(FixOrders.Side.Sell, cancel.side);
        assertEquals("100", cancel.order_qty);
        assertTrue(cancel.transact_time > 0L);
    }
}
