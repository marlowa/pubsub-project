package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.blotter.BlotterRow;
import com.pubsub.fixtestclient.blotter.BlotterStore;
import com.pubsub.fixtestclient.fix.FixEngine;
import io.javalin.http.Context;
import quickfix.fix50sp2.NewOrderSingle;
import quickfix.fix50sp2.OrderCancelRequest;
import quickfix.field.ClOrdID;
import quickfix.field.HandlInst;
import quickfix.field.OrdType;
import quickfix.field.OrderQty;
import quickfix.field.OrigClOrdID;
import quickfix.field.Price;
import quickfix.field.Side;
import quickfix.field.Symbol;
import quickfix.field.TransactTime;

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

    private final FixEngine fixEngine;
    private final BlotterStore blotterStore;

    public MessagesHandler(FixEngine fixEngine, BlotterStore blotterStore) {
        this.fixEngine = fixEngine;
        this.blotterStore = blotterStore;
    }

    public void send(Context ctx) {
        String clOrdId = ctx.formParam("clOrdId");
        String symbol = ctx.formParam("symbol");
        String sideStr = ctx.formParam("side");
        String ordTypeStr = ctx.formParam("ordType");
        String qtyStr = ctx.formParam("qty");
        String priceStr = ctx.formParam("price");

        if (clOrdId == null || clOrdId.isBlank()) {
            ctx.status(400).json(Map.of("error", "ClOrdID is required"));
            return;
        }
        if (symbol == null || symbol.isBlank()) {
            ctx.status(400).json(Map.of("error", "Symbol is required"));
            return;
        }

        try {
            NewOrderSingle nos = new NewOrderSingle();
            nos.set(new ClOrdID(clOrdId.trim()));
            nos.set(new Symbol(symbol.trim()));
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

            fixEngine.send(nos);
            blotterStore.addOutbound(nos);
            ctx.json(Map.of("ok", true));
        } catch (Exception e) {
            ctx.status(400).json(Map.of("error", e.getMessage()));
        }
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

        try {
            String clOrdId = "CXL-" + origClOrdId.trim() + "-" + System.currentTimeMillis();
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
            map.put("seqNum", row.seqNum());
            map.put("clOrdId", row.clOrdId());
            map.put("origClOrdId", row.origClOrdId());
            map.put("orderId", row.orderId());
            map.put("execId", row.execId());
            map.put("execType", row.execType());
            map.put("ordStatus", row.ordStatus());
            map.put("ordRejReason", row.ordRejReason());
            map.put("cxlRejReason", row.cxlRejReason());
            map.put("symbol", row.symbol());
            map.put("side", row.side());
            map.put("ordQty", row.ordQty());
            map.put("price", row.price());
            map.put("ordType", row.ordType());
            map.put("cumQty", row.cumQty());
            map.put("leavesQty", row.leavesQty());
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
