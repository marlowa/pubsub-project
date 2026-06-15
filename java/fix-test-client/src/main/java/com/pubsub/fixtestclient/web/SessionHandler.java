package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.SessionStatus;
import io.javalin.http.Context;

import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.Map;


public class SessionHandler {

    private static final DateTimeFormatter TIMESTAMP =
            DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss").withZone(ZoneOffset.UTC);

    private final FixEngine fixEngine;

    private volatile LastSession lastSession;

    public SessionHandler(FixEngine fixEngine) {
        this.fixEngine = fixEngine;
    }

    public void getStatus(Context ctx) {
        SessionStatus status = fixEngine.getStatus();
        Map<String, Object> body = new java.util.LinkedHashMap<>();
        body.put("connected",         status.connected());
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
        ctx.json(body);
    }

    public void logon(Context ctx) {
        String senderCompId = ctx.formParam("senderCompId");
        String password     = ctx.formParam("password");
        boolean useTls      = "true".equals(ctx.formParam("useTls"));

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
            fixEngine.logon(senderCompId, password, useTls);
        } catch (Exception e) {
            ctx.status(500).json(Map.of("ok", false, "error", e.getMessage()));
            return;
        }
        ctx.json(Map.of("ok", true));
    }

    public void logout(Context ctx) {
        SessionStatus status = fixEngine.getStatus();
        if (status.loggedOn() && status.logonTime() != null) {
            lastSession = new LastSession(
                    status.senderCompId(),
                    status.logonTime(),
                    Instant.now()
            );
        }
        fixEngine.logout();
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
