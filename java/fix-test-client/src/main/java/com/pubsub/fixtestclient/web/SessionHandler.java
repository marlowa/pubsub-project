package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.LogonMode;
import com.pubsub.fixtestclient.gateway.GatewayKind;
import com.pubsub.fixtestclient.gateway.GatewaySelector;
import com.pubsub.fixtestclient.fix.SessionStatus;
import io.javalin.http.Context;

import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.Map;


public class SessionHandler {

    private static final DateTimeFormatter TIMESTAMP =
            DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss").withZone(ZoneOffset.UTC);

    private final GatewaySelector gateways;
    private final FixEngine fixEngine;
    private final int plainPort;
    private final int tlsPort;
    private final int proprietaryPort;
    private final boolean tlsEnabled;
    private final int binaryPort;

    private volatile LastSession lastSession;

    public SessionHandler(GatewaySelector gateways, int plainPort, int tlsPort, int proprietaryPort, boolean tlsEnabled, int binaryPort) {
        this.gateways        = gateways;
        this.fixEngine       = gateways.fix();
        this.binaryPort      = binaryPort;
        this.plainPort       = plainPort;
        this.tlsPort         = tlsPort;
        this.proprietaryPort = proprietaryPort;
        this.tlsEnabled      = tlsEnabled;
    }

    public void getPorts(Context ctx) {
        ctx.json(Map.of("plainPort", plainPort, "tlsPort", tlsPort, "proprietaryPort", proprietaryPort,
                        "tlsEnabled", tlsEnabled, "binaryPort", binaryPort));
    }

    public void getStatus(Context ctx) {
        SessionStatus status = gateways.status();
        Map<String, Object> body = new java.util.LinkedHashMap<>();
        body.put("gateway",           gateways.active().name().toLowerCase(java.util.Locale.ROOT));
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
        GatewayKind requested = GatewayKind.fromFormValue(ctx.formParam("gateway"));
        String switchError = gateways.setActive(requested);
        if (!switchError.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", switchError));
            return;
        }
        if (requested == GatewayKind.BINARY) {
            logonBinary(ctx);
            return;
        }

        String senderCompId = ctx.formParam("senderCompId");
        String targetCompId = ctx.formParam("targetCompId");
        String password     = ctx.formParam("password");
        boolean useTls             = "true".equals(ctx.formParam("useTls"));
        LogonMode logonMode = "true".equals(ctx.formParam("proprietaryLogon"))
                ? LogonMode.PROPRIETARY
                : LogonMode.STANDARD;

        if (logonMode == LogonMode.PROPRIETARY && useTls) {
            ctx.status(400).json(Map.of("ok", false, "error", "Proprietary logon cannot use TLS"));
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
            fixEngine.logon(senderCompId, targetCompId, password, useTls, logonMode);
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
    private void logonBinary(Context ctx) {
        String compId = ctx.formParam("senderCompId");
        String password = ctx.formParam("password");
        if (password == null || password.isEmpty()) {
            ctx.status(400).json(Map.of("ok", false, "error", "Password is required"));
            return;
        }
        String error = gateways.binary().logon(compId, password, ctx.formParam("targetCompId"));
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
