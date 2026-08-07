package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.blotter.BlotterRow;
import com.pubsub.fixtestclient.blotter.BlotterStore;
import com.pubsub.fixtestclient.binary.BinaryOrderForm;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.gateway.GatewaySelector;
import io.javalin.http.Context;
import quickfix.fix50sp2.NewOrderSingle;
import quickfix.fix50sp2.OrderCancelRequest;
import quickfix.field.Account;
import quickfix.field.ClOrdID;
import quickfix.field.ExDestination;
import quickfix.field.ExecInst;
import quickfix.field.ExpireTime;
import quickfix.field.HandlInst;
import quickfix.field.MaxFloor;
import quickfix.field.MinQty;
import quickfix.field.OrdType;
import quickfix.field.OrderQty;
import quickfix.field.OrigClOrdID;
import quickfix.field.PartyID;
import quickfix.field.PartyIDSource;
import quickfix.field.PartyRole;
import quickfix.field.Price;
import quickfix.field.SecurityID;
import quickfix.field.SecurityIDSource;
import quickfix.field.Side;
import quickfix.field.StopPx;
import quickfix.field.Symbol;
import quickfix.field.Text;
import quickfix.field.TimeInForce;
import quickfix.field.TransactTime;
import quickfix.field.UnderlyingQty;
import quickfix.field.UnderlyingSecurityID;
import quickfix.field.UnderlyingSymbol;

import quickfix.Message;

import java.time.LocalDateTime;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class MessagesHandler {

    private static final Set<Integer> ENGINE_MANAGED_TAGS = Set.of(8, 9, 10, 34, 49, 52, 56);

    private static final DateTimeFormatter TIME_FORMAT =
            DateTimeFormatter.ofPattern("HH:mm:ss.SSSSSS").withZone(ZoneOffset.UTC);

    private final GatewaySelector gateways;
    private final FixEngine fixEngine;
    private final BlotterStore blotterStore;

    public MessagesHandler(GatewaySelector gateways, BlotterStore blotterStore) {
        this.gateways = gateways;
        this.fixEngine = gateways.fix();
        this.blotterStore = blotterStore;
    }

    public void send(Context ctx) {
        String clOrdId = ctx.formParam("clOrdId");
        String symbol = ctx.formParam("symbol");
        String sideStr = ctx.formParam("side");
        String ordTypeStr = ctx.formParam("ordType");
        String qtyStr = ctx.formParam("qty");
        String priceStr = ctx.formParam("price");
        String securityId = ctx.formParam("securityId");
        String securityIdSource = ctx.formParam("securityIdSource");

        if (clOrdId == null || clOrdId.isBlank()) {
            ctx.status(400).json(Map.of("error", "ClOrdID is required"));
            return;
        }
        if (symbol == null || symbol.isBlank()) {
            ctx.status(400).json(Map.of("error", "Symbol is required"));
            return;
        }

        if (gateways.isBinaryActive()) {
            String error = gateways.binary().sendNewOrderSingle(BinaryOrderForm.buildOrder(ctx, clOrdId.trim()));
            if (!error.isEmpty()) {
                ctx.status(400).json(Map.of("error", error));
                return;
            }
            ctx.json(Map.of("ok", true, "clOrdId", clOrdId.trim()));
            return;
        }

        try {
            NewOrderSingle nos = new NewOrderSingle();
            nos.set(new ClOrdID(clOrdId.trim()));
            nos.set(new Symbol(symbol.trim()));
            // Exchange instrument identification for venues (e.g. futures) that key on a numeric
            // exchange instrument id rather than the Symbol alone: SecurityID (48) paired with
            // SecurityIDSource (22). Only sent when a SecurityID is supplied, so the plain
            // Symbol-only flow is unchanged. Default source is "8" (Exchange Symbol).
            if (securityId != null && !securityId.isBlank()) {
                nos.set(new SecurityID(securityId.trim()));
                String idSource = (securityIdSource == null || securityIdSource.isBlank()) ? "8" : securityIdSource.trim();
                nos.set(new SecurityIDSource(idSource));
            }
            nos.set(new HandlInst(HandlInst.AUTOMATED_EXECUTION_ORDER_PRIVATE_NO_BROKER_INTERVENTION));
            nos.set(new TransactTime(LocalDateTime.now()));

            char side = parseSide(sideStr);
            nos.set(new Side(side));

            char ordType = parseOrdType(ordTypeStr);
            nos.set(new OrdType(ordType));

            if (qtyStr != null && !qtyStr.isBlank()) {
                nos.set(new OrderQty(Double.parseDouble(qtyStr.trim())));
            }
            if (priceStr != null && !priceStr.isBlank()) {
                nos.set(new Price(Double.parseDouble(priceStr.trim())));
            }

            // Optional scalar fields from the collapsible "More fields" section -- each is
            // only put on the wire when supplied, so a minimal order stays minimal.
            String stopPxStr = ctx.formParam("stopPx");
            if (stopPxStr != null && !stopPxStr.isBlank()) {
                nos.set(new StopPx(Double.parseDouble(stopPxStr.trim())));
            }
            String minQtyStr = ctx.formParam("minQty");
            if (minQtyStr != null && !minQtyStr.isBlank()) {
                nos.set(new MinQty(Double.parseDouble(minQtyStr.trim())));
            }
            String maxFloorStr = ctx.formParam("maxFloor");
            if (maxFloorStr != null && !maxFloorStr.isBlank()) {
                nos.set(new MaxFloor(Double.parseDouble(maxFloorStr.trim())));
            }
            String tifStr = ctx.formParam("timeInForce");
            if (tifStr != null && !tifStr.isBlank()) {
                nos.set(new TimeInForce(tifStr.trim().charAt(0)));
            }
            String expireTimeStr = ctx.formParam("expireTime");
            if (expireTimeStr != null && !expireTimeStr.isBlank()) {
                // datetime-local supplies "yyyy-MM-ddTHH:mm" (ISO_LOCAL_DATE_TIME).
                nos.set(new ExpireTime(LocalDateTime.parse(expireTimeStr.trim())));
            }
            String account = ctx.formParam("account");
            if (account != null && !account.isBlank()) {
                nos.set(new Account(account.trim()));
            }
            String exDestination = ctx.formParam("exDestination");
            if (exDestination != null && !exDestination.isBlank()) {
                nos.set(new ExDestination(exDestination.trim()));
            }
            String execInst = ctx.formParam("execInst");
            if (execInst != null && !execInst.isBlank()) {
                nos.set(new ExecInst(execInst.trim())); // MULTIPLECHARVALUE, e.g. "1 G"
            }
            String text = ctx.formParam("text");
            if (text != null && !text.isBlank()) {
                nos.set(new Text(text.trim()));
            }

            // Repeating groups: the form posts one value per row as parallel repeated
            // params, paired here by index into QuickFIX group instances.
            addUnderlyings(ctx, nos);
            addParties(ctx, nos);

            fixEngine.send(nos);
            blotterStore.addOutbound(nos);
            ctx.json(Map.of("ok", true));
        } catch (Exception e) {
            ctx.status(400).json(Map.of("error", e.getMessage()));
        }
    }

    // The order form posts each repeating-group column as a parallel list of repeated
    // form params (one entry per row). The three lists are the same length, so pair
    // them by index into one QuickFIX group instance per row; blank cells are skipped.

    private static void addUnderlyings(Context ctx, NewOrderSingle nos) {
        List<String> symbols = ctx.formParams("underlyingSymbol");
        List<String> securityIds = ctx.formParams("underlyingSecurityId");
        List<String> qtys = ctx.formParams("underlyingQty");
        for (int i = 0; i < symbols.size(); i++) {
            NewOrderSingle.NoUnderlyings group = new NewOrderSingle.NoUnderlyings();
            String symbol = at(symbols, i);
            if (!symbol.isEmpty()) {
                group.set(new UnderlyingSymbol(symbol));
            }
            String securityId = at(securityIds, i);
            if (!securityId.isEmpty()) {
                group.set(new UnderlyingSecurityID(securityId));
            }
            String qty = at(qtys, i);
            if (!qty.isEmpty()) {
                group.set(new UnderlyingQty(Double.parseDouble(qty)));
            }
            nos.addGroup(group);
        }
    }

    private static void addParties(Context ctx, NewOrderSingle nos) {
        List<String> ids = ctx.formParams("partyId");
        List<String> sources = ctx.formParams("partyIdSource");
        List<String> roles = ctx.formParams("partyRole");
        for (int i = 0; i < ids.size(); i++) {
            NewOrderSingle.NoPartyIDs group = new NewOrderSingle.NoPartyIDs();
            String id = at(ids, i);
            if (!id.isEmpty()) {
                group.set(new PartyID(id));
            }
            String source = at(sources, i);
            if (!source.isEmpty()) {
                group.set(new PartyIDSource(source.charAt(0)));
            }
            String role = at(roles, i);
            if (!role.isEmpty()) {
                group.set(new PartyRole(Integer.parseInt(role)));
            }
            nos.addGroup(group);
        }
    }

    /** @return the trimmed element at index i, or "" if absent. */
    private static String at(List<String> values, int i) {
        return (values != null && i < values.size() && values.get(i) != null) ? values.get(i).trim() : "";
    }

    public void cancel(Context ctx) {
        String origClOrdId = ctx.formParam("origClOrdId");
        String symbol = ctx.formParam("symbol");
        String sideStr = ctx.formParam("side");
        String qtyStr = ctx.formParam("qty");

        if (origClOrdId == null || origClOrdId.isBlank()) {
            ctx.status(400).json(Map.of("error", "OrigClOrdID is required"));
            return;
        }
        if (symbol == null || symbol.isBlank()) {
            ctx.status(400).json(Map.of("error", "Symbol is required"));
            return;
        }

        String cancelClOrdId = "CXL-" + origClOrdId.trim() + "-" + System.currentTimeMillis();

        if (gateways.isBinaryActive()) {
            String error = gateways.binary().sendOrderCancelRequest(BinaryOrderForm.buildCancel(ctx, cancelClOrdId));
            if (!error.isEmpty()) {
                ctx.status(400).json(Map.of("error", error));
                return;
            }
            ctx.json(Map.of("ok", true, "clOrdId", cancelClOrdId));
            return;
        }

        try {
            String clOrdId = cancelClOrdId;
            OrderCancelRequest ocr = new OrderCancelRequest();
            ocr.set(new ClOrdID(clOrdId));
            ocr.set(new OrigClOrdID(origClOrdId.trim()));
            ocr.set(new Symbol(symbol.trim()));
            ocr.set(new TransactTime(LocalDateTime.now()));
            ocr.set(new Side(parseSide(sideStr)));
            if (qtyStr != null && !qtyStr.isBlank()) {
                ocr.set(new OrderQty(Double.parseDouble(qtyStr.trim())));
            }

            fixEngine.send(ocr);
            blotterStore.addOutbound(ocr);
            ctx.json(Map.of("ok", true, "clOrdId", clOrdId));
        } catch (Exception e) {
            ctx.status(400).json(Map.of("error", e.getMessage()));
        }
    }

    public void sendRaw(Context ctx) {
        if (gateways.isBinaryActive()) {
            ctx.status(400).json(Map.of("error",
                    "Raw entry is FIX text; the binary gateway takes encoded PDUs. Log on to the FIX gateway to use it."));
            return;
        }
        String raw = ctx.formParam("raw");
        if (raw == null || raw.isBlank()) {
            ctx.status(400).json(Map.of("error", "FIX string is required"));
            return;
        }

        try {
            Message message = new Message();
            for (String pair : raw.trim().split("\\|")) {
                pair = pair.trim();
                if (pair.isEmpty()) {
                    continue;
                }
                int eq = pair.indexOf('=');
                if (eq < 1) {
                    throw new IllegalArgumentException("Invalid field: '" + pair + "'");
                }
                int tag = Integer.parseInt(pair.substring(0, eq).trim());
                String value = pair.substring(eq + 1);
                if (ENGINE_MANAGED_TAGS.contains(tag)) {
                    continue;
                }
                if (tag == 35) {
                    message.getHeader().setString(tag, value);
                } else {
                    message.setString(tag, value);
                }
            }

            if (!message.getHeader().isSetField(35)) {
                ctx.status(400).json(Map.of("error", "MsgType (35) is required"));
                return;
            }

            if (!fixEngine.send(message)) {
                ctx.status(400).json(Map.of("error", "Not logged on"));
                return;
            }
            blotterStore.addOutbound(message);
            ctx.json(Map.of("ok", true));
        } catch (NumberFormatException e) {
            ctx.status(400).json(Map.of("error", "Invalid tag number: " + e.getMessage()));
        } catch (Exception e) {
            ctx.status(400).json(Map.of("error", e.getMessage()));
        }
    }

    public void clear(Context ctx) {
        blotterStore.clear();
        ctx.json(Map.of("ok", true));
    }

    public void getBlotter(Context ctx) {
        List<BlotterRow> rows = blotterStore.rows();
        List<Map<String, Object>> result = new ArrayList<>();
        for (BlotterRow row : rows) {
            Map<String, Object> map = new LinkedHashMap<>();
            map.put("id", row.id());
            map.put("time", TIME_FORMAT.format(row.time()));
            map.put("direction", row.direction());
            map.put("msgType", row.msgType());
            map.put("seqNum", row.seqNum());
            map.put("clOrdId", row.clOrdId());
            map.put("origClOrdId", row.origClOrdId());
            map.put("orderId", row.orderId());
            map.put("execId", row.execId());
            map.put("execType", row.execType());
            map.put("ordStatus", row.ordStatus());
            map.put("ordRejReason", row.ordRejReason());
            map.put("cxlRejReason", row.cxlRejReason());
            map.put("securityId", row.securityId());
            map.put("symbol", row.symbol());
            map.put("side", row.side());
            map.put("ordQty", row.ordQty());
            map.put("price", row.price());
            map.put("ordType", row.ordType());
            map.put("timeInForce", row.timeInForce());
            map.put("expireTime", row.expireTime());
            map.put("cumQty", row.cumQty());
            map.put("leavesQty", row.leavesQty());
            map.put("parties", row.parties());
            map.put("underlyings", row.underlyings());
            result.add(map);
        }
        ctx.json(result);
    }

    private char parseSide(String value) {
        if (value == null) {
            return Side.BUY;
        }
        return switch (value.toLowerCase()) {
            case "sell" -> Side.SELL;
            case "sell short" -> Side.SELL_SHORT;
            default -> Side.BUY;
        };
    }

    private char parseOrdType(String value) {
        if (value == null) {
            return OrdType.LIMIT;
        }
        return switch (value.toLowerCase()) {
            case "market" -> OrdType.MARKET;
            default -> OrdType.LIMIT;
        };
    }
}
