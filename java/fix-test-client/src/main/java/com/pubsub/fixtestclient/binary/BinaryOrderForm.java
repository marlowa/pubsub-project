package com.pubsub.fixtestclient.binary;

import com.pubsub.fixtestclient.protocol.FixOrders;
import io.javalin.http.Context;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;

/**
 * Builds binary-gateway orders from the web order form.
 *
 * The FIX path builds a QuickFIX NewOrderSingle from the same form; this builds the
 * generated PDU. The form is unchanged between them, which is the point: the two gateways
 * carry the same order, so the user fills in the same fields either way.
 *
 * Empty form fields are left unset rather than sent as blanks, so a minimal order stays
 * minimal on the wire regardless of how large the form is.
 */
public final class BinaryOrderForm {

    private BinaryOrderForm() {
    }

    public static FixOrders.NewOrderSingle buildOrder(Context ctx, String clOrdId) {
        FixOrders.NewOrderSingle order = new FixOrders.NewOrderSingle();
        order.cl_ord_id = clOrdId;
        order.symbol = trimmed(ctx.formParam("symbol"));
        order.side = parseSide(ctx.formParam("side"));
        order.ord_type = parseOrdType(ctx.formParam("ordType"));
        order.transact_time = nowNanoseconds();
        order.order_qty = trimmed(ctx.formParam("qty"));

        order.has_price = setIfPresent(ctx.formParam("price"));
        order.price = trimmed(ctx.formParam("price"));
        order.has_security_id = setIfPresent(ctx.formParam("securityId"));
        order.security_id = trimmed(ctx.formParam("securityId"));
        order.has_security_id_source = setIfPresent(ctx.formParam("securityIdSource"));
        order.security_id_source = trimmed(ctx.formParam("securityIdSource"));
        order.has_stop_px = setIfPresent(ctx.formParam("stopPx"));
        order.stop_px = trimmed(ctx.formParam("stopPx"));
        order.has_min_qty = setIfPresent(ctx.formParam("minQty"));
        order.min_qty = trimmed(ctx.formParam("minQty"));
        order.has_max_floor = setIfPresent(ctx.formParam("maxFloor"));
        order.max_floor = trimmed(ctx.formParam("maxFloor"));
        order.has_account = setIfPresent(ctx.formParam("account"));
        order.account = trimmed(ctx.formParam("account"));
        order.has_ex_destination = setIfPresent(ctx.formParam("exDestination"));
        order.ex_destination = trimmed(ctx.formParam("exDestination"));
        order.has_exec_inst = setIfPresent(ctx.formParam("execInst"));
        order.exec_inst = trimmed(ctx.formParam("execInst"));
        order.has_text = setIfPresent(ctx.formParam("text"));
        order.text = trimmed(ctx.formParam("text"));

        String timeInForce = trimmed(ctx.formParam("timeInForce"));
        if (!timeInForce.isEmpty()) {
            FixOrders.TimeInForce parsed = FixOrders.TimeInForce.fromValue(timeInForce.charAt(0));
            if (parsed != null) {
                order.has_time_in_force = true;
                order.time_in_force = parsed;
            }
        }

        order.no_underlyings = buildUnderlyings(ctx);
        order.no_party_i_ds = buildParties(ctx);
        return order;
    }

    public static FixOrders.OrderCancelRequest buildCancel(Context ctx, String clOrdId) {
        FixOrders.OrderCancelRequest cancel = new FixOrders.OrderCancelRequest();
        cancel.cl_ord_id = clOrdId;
        cancel.orig_cl_ord_id = trimmed(ctx.formParam("origClOrdId"));
        cancel.symbol = trimmed(ctx.formParam("symbol"));
        cancel.side = parseSide(ctx.formParam("side"));
        cancel.transact_time = nowNanoseconds();
        cancel.order_qty = trimmed(ctx.formParam("qty"));
        return cancel;
    }

    // The form posts each group column as a parallel list of repeated params, one entry per
    // row, so the columns are paired by index. Blank rows are dropped.

    private static FixOrders.Underlyings[] buildUnderlyings(Context ctx) {
        List<String> symbols = ctx.formParams("underlyingSymbol");
        List<String> securityIds = ctx.formParams("underlyingSecurityId");
        List<String> quantities = ctx.formParams("underlyingQty");

        List<FixOrders.Underlyings> built = new ArrayList<>();
        for (int index = 0; index < symbols.size(); index++) {
            FixOrders.Underlyings underlying = new FixOrders.Underlyings();
            underlying.has_underlying_symbol = setIfPresent(at(symbols, index));
            underlying.underlying_symbol = at(symbols, index);
            underlying.has_underlying_security_id = setIfPresent(at(securityIds, index));
            underlying.underlying_security_id = at(securityIds, index);
            underlying.has_underlying_qty = setIfPresent(at(quantities, index));
            underlying.underlying_qty = at(quantities, index);
            if (underlying.has_underlying_symbol || underlying.has_underlying_security_id || underlying.has_underlying_qty) {
                built.add(underlying);
            }
        }
        return built.toArray(new FixOrders.Underlyings[0]);
    }

    private static FixOrders.PartyIDs[] buildParties(Context ctx) {
        List<String> ids = ctx.formParams("partyId");
        List<String> sources = ctx.formParams("partyIdSource");
        List<String> roles = ctx.formParams("partyRole");

        List<FixOrders.PartyIDs> built = new ArrayList<>();
        for (int index = 0; index < ids.size(); index++) {
            FixOrders.PartyIDs party = new FixOrders.PartyIDs();
            party.has_party_id = setIfPresent(at(ids, index));
            party.party_id = at(ids, index);

            String source = at(sources, index);
            if (!source.isEmpty()) {
                FixOrders.PartyIDSource parsed = FixOrders.PartyIDSource.fromValue(source.charAt(0));
                if (parsed != null) {
                    party.has_party_id_source = true;
                    party.party_id_source = parsed;
                }
            }
            String role = at(roles, index);
            if (!role.isEmpty()) {
                FixOrders.PartyRole parsed = FixOrders.PartyRole.fromValue(Integer.parseInt(role));
                if (parsed != null) {
                    party.has_party_role = true;
                    party.party_role = parsed;
                }
            }
            if (party.has_party_id || party.has_party_id_source || party.has_party_role) {
                built.add(party);
            }
        }
        return built.toArray(new FixOrders.PartyIDs[0]);
    }

    private static FixOrders.Side parseSide(String value) {
        FixOrders.Side parsed = value == null || value.isBlank() ? null : FixOrders.Side.fromValue(value.trim().charAt(0));
        return parsed != null ? parsed : FixOrders.Side.Buy;
    }

    private static FixOrders.OrdType parseOrdType(String value) {
        FixOrders.OrdType parsed = value == null || value.isBlank() ? null : FixOrders.OrdType.fromValue(value.trim().charAt(0));
        return parsed != null ? parsed : FixOrders.OrdType.Limit;
    }

    private static long nowNanoseconds() {
        Instant now = Instant.now();
        return now.getEpochSecond() * 1_000_000_000L + now.getNano();
    }

    private static boolean setIfPresent(String value) {
        return value != null && !value.trim().isEmpty();
    }

    private static String trimmed(String value) {
        return value == null ? "" : value.trim();
    }

    private static String at(List<String> values, int index) {
        return (values != null && index < values.size() && values.get(index) != null) ? values.get(index).trim() : "";
    }
}
