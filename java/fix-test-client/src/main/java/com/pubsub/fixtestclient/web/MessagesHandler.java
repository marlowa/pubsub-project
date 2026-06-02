package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.blotter.BlotterRow;
import com.pubsub.fixtestclient.blotter.BlotterStore;
import com.pubsub.fixtestclient.fix.FixEngine;
import io.javalin.http.Context;
import quickfix.fix50sp2.NewOrderSingle;
import quickfix.field.ClOrdID;
import quickfix.field.HandlInst;
import quickfix.field.OrdType;
import quickfix.field.OrderQty;
import quickfix.field.Price;
import quickfix.field.Side;
import quickfix.field.Symbol;
import quickfix.field.TransactTime;

import java.time.LocalDateTime;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class MessagesHandler {

    private static final DateTimeFormatter TIME_FORMAT =
            DateTimeFormatter.ofPattern("HH:mm:ss.SSS").withZone(ZoneOffset.UTC);

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
            map.put("orderId", row.orderId());
            map.put("execId", row.execId());
            map.put("execType", row.execType());
            map.put("ordStatus", row.ordStatus());
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
