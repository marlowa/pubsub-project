package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.Config;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.LogonMode;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import com.pubsub.fixtestclient.gateway.GatewayKind;
import com.pubsub.fixtestclient.gateway.GatewaySelector;
import com.pubsub.fixtestclient.fix.SessionStatus;
import io.javalin.http.Context;

import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;


public class SessionHandler {

    private static final DateTimeFormatter TIMESTAMP =
            DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss").withZone(ZoneOffset.UTC);

    private final GatewaySelector gateways;
    private final FixEngine fixEngine;
    private final Config config;

    private volatile LastSession lastSession;

    public SessionHandler(GatewaySelector gateways, Config config) {
        this.gateways  = gateways;
        this.fixEngine = gateways.fix();
        this.config    = config;
    }

    /**
     * The endpoints the logon page offers, in configured order.
     *
     * Replaces a flat set of ports (plain, tls, proprietary, binary) that could describe only
     * one instance of each protocol. The page needs to name each address as well as reach it,
     * so each entry carries its label and whether TLS is available -- an endpoint with no TLS
     * listener has its TLS control disabled rather than offering a choice that would fail.
     */
    public void getPorts(Context ctx) {
        List<Map<String, Object>> endpoints = new ArrayList<>();
        for (GatewayEndpoint endpoint : config.gateways()) {
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("key", endpoint.key());
            entry.put("label", endpoint.label());
            entry.put("protocol", endpoint.kind().name().toLowerCase(java.util.Locale.ROOT));
            entry.put("host", endpoint.host());
            entry.put("port", endpoint.port());
            entry.put("tlsPort", endpoint.tlsPort());
            entry.put("supportsTls", endpoint.supportsTls());
            entry.put("proprietary", endpoint.logonMode() == LogonMode.PROPRIETARY);
            endpoints.add(entry);
        }
        ctx.json(Map.of("gateways", endpoints,
                        "tlsEnabled", config.tlsEnabled(),
                        "activeKey", gateways.active().key()));
    }

    public void getStatus(Context ctx) {
        SessionStatus status = gateways.status();
        Map<String, Object> body = new java.util.LinkedHashMap<>();
        // The protocol drives which fields the page shows; the key and label say WHICH of the
        // several gateways of that protocol this session is on, which the protocol alone
        // cannot answer now that each runs as more than one instance.
        body.put("gateway",           gateways.activeKind().name().toLowerCase(java.util.Locale.ROOT));
        body.put("gatewayKey",        gateways.active().key());
        body.put("gatewayLabel",      gateways.active().label());
        body.put("connected",         status.connected());
        body.put("loggingOn",         status.loggingOn());
        body.put("loggedOn",          status.loggedOn());
        body.put("senderCompId",      status.senderCompId());
        body.put("targetCompId",      status.targetCompId());
        body.put("host",              status.host());
        body.put("port",              status.port());
        body.put("logonTime",         status.logonTime() != null ? TIMESTAMP.format(status.logonTime()) : "");
        body.put("startingSeqNum",    status.startingSeqNum());
        body.put("nextOutgoingSeqNum", status.nextOutgoingSeqNum());
        body.put("nextIncomingSeqNum", status.nextIncomingSeqNum());
        body.put("lastError",         status.lastError());
        body.put("suggestedSeqNum",   status.suggestedSeqNum());
        ctx.json(body);
    }

    public void logon(Context ctx) {
        // The form names ONE endpoint, which is the whole address. It used to send a protocol
        // plus a pair of booleans that the server recombined into a port -- a scheme with no
        // room for a second instance of a protocol, so instance b was unreachable.
        String key = ctx.formParam("gateway");
        Optional<GatewayEndpoint> chosen = key == null ? Optional.empty() : config.gatewayByKey(key);
        if (chosen.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", "unknown gateway '" + key + "'"));
            return;
        }
        GatewayEndpoint endpoint = chosen.get();

        String switchError = gateways.setActive(endpoint);
        if (!switchError.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", switchError));
            return;
        }
        if (endpoint.kind() == GatewayKind.BINARY) {
            logonBinary(ctx, endpoint);
            return;
        }

        String senderCompId = ctx.formParam("senderCompId");
        String targetCompId = ctx.formParam("targetCompId");
        String password     = ctx.formParam("password");
        boolean useTls      = "true".equals(ctx.formParam("useTls"));

        // The logon dialect is a property of the endpoint, not a box the user ticks: the
        // proprietary one lives at its own address. That makes "proprietary over TLS"
        // unrepresentable rather than an error to report.
        if (useTls && !endpoint.supportsTls()) {
            ctx.status(400).json(Map.of("ok", false, "error", endpoint.label() + " has no TLS listener"));
            return;
        }

        if (password == null || password.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", "Password is required"));
            return;
        }

        String overrideSeqStr = ctx.formParam("overrideSeqNum");
        boolean overrideSeq   = "true".equals(ctx.formParam("overrideSeq"));
        if (overrideSeq && overrideSeqStr != null && !overrideSeqStr.isBlank()) {
            try {
                fixEngine.setOverrideSeqNum(Integer.parseInt(overrideSeqStr.trim()));
            } catch (NumberFormatException ignored) {
            }
        }

        try {
            fixEngine.logon(endpoint, senderCompId, targetCompId, password, useTls);
        } catch (Exception e) {
            ctx.status(500).json(Map.of("ok", false, "error", e.getMessage()));
            return;
        }
        ctx.json(Map.of("ok", true));
    }

    /**
     * Logs on to the binary gateway, which needs only an identity.
     *
     * Comp id, password and target comp id. No TLS flag or sequence-number override: the
     * binary gateway has no TLS listener and no session-layer sequence numbers, so offering
     * those would imply capabilities that do not exist. Authentication it does have --
     * SCRAM-SHA-256 against the same service the FIX gateway uses.
     */
    private void logonBinary(Context ctx, GatewayEndpoint endpoint) {
        String compId = ctx.formParam("senderCompId");
        String password = ctx.formParam("password");
        if (password == null || password.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", "Password is required"));
            return;
        }
        String error = gateways.binary().logon(endpoint, compId, password, ctx.formParam("targetCompId"));
        if (!error.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", error));
            return;
        }
        ctx.json(Map.of("ok", true));
    }

    public void logout(Context ctx) {
        SessionStatus status = gateways.status();
        if (status.loggedOn() && status.logonTime() != null) {
            lastSession = new LastSession(
                    status.senderCompId(),
                    status.logonTime(),
                    Instant.now()
            );
        }
        if (gateways.isBinaryActive()) {
            gateways.binary().logout();
        } else {
            fixEngine.logout();
        }
        ctx.json(Map.of("ok", true));
    }

    public void getLastSession(Context ctx) {
        LastSession ls = lastSession;
        if (ls == null) {
            ctx.json(Map.of("present", false));
            return;
        }
        long durationSeconds = ls.endTime().getEpochSecond() - ls.startTime().getEpochSecond();
        long minutes = durationSeconds / 60;
        long seconds = durationSeconds % 60;
        String duration = minutes > 0
                ? minutes + " minutes " + seconds + " seconds"
                : seconds + " seconds";
        ctx.json(Map.of(
                "present",     true,
                "senderCompId", ls.senderCompId(),
                "startTime",   TIMESTAMP.format(ls.startTime()),
                "endTime",     TIMESTAMP.format(ls.endTime()),
                "duration",    duration
        ));
    }

    private record LastSession(String senderCompId, Instant startTime, Instant endTime) {
    }
}
