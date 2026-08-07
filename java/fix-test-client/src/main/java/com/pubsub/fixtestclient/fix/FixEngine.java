package com.pubsub.fixtestclient.fix;

import com.pubsub.fixtestclient.Config;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import quickfix.ConfigError;
import quickfix.Message;
import quickfix.Session;
import quickfix.SessionID;
import quickfix.SessionNotFound;
import quickfix.SessionSettings;
import quickfix.SocketInitiator;

import java.io.IOException;
import java.time.Instant;

public class FixEngine {

    private static final Logger log = LoggerFactory.getLogger(FixEngine.class);
    private static final int CONNECT_TIMEOUT_SECONDS = 10;

    private final Config config;
    private final FixApplication fixApplication;
    private SocketInitiator initiator;
    private SessionID sessionId;

    private volatile Instant logonTime;
    private volatile int startingSeqNum;
    private volatile Integer overrideSeqNum;
    private volatile boolean loggingOn;
    private volatile Instant connectDeadline;
    private volatile String connectError;

    public FixEngine(Config config, FixApplication fixApplication) {
        this.config = config;
        this.fixApplication = fixApplication;
        fixApplication.setOnLogon(this::handleLogon);
        fixApplication.setOnLogout(this::handleLogout);
    }

    public synchronized void logon(GatewayEndpoint endpoint, String compId, String targetCompId, String password, boolean useTls)
            throws ConfigError, IOException {
        LogonMode logonMode = endpoint.logonMode();
        // Proprietary logon is plaintext only; the proprietary gateway does not accept a TLS
        // handshake. The logon page cannot express the combination -- that endpoint has no
        // TLS port, so its TLS control is disabled -- but the scripting API can still ask
        // for it, so the invariant is enforced here where every caller passes.
        if (logonMode == LogonMode.PROPRIETARY && useTls) {
            throw new IllegalArgumentException("Proprietary logon is incompatible with TLS");
        }
        if (useTls && !endpoint.supportsTls()) {
            throw new IllegalArgumentException(endpoint.label() + " has no TLS listener");
        }

        connectError = null;
        stopInitiator();

        fixApplication.clearSuggestedSeqNum();
        fixApplication.setLogonMode(logonMode);
        fixApplication.setPendingSeqNumOverride(overrideSeqNum);
        overrideSeqNum = null;

        if (useTls) {
            java.io.File trustStore = new java.io.File(config.trustStorePath());
            if (!trustStore.exists() || !trustStore.isFile()) {
                throw new IOException("TLS requested but trust store not found: "
                        + trustStore.getAbsolutePath()
                        + " — either provide the trust store or set useTls=false");
            }
        }

        SessionSettings settings = buildSettings(endpoint, compId, targetCompId, useTls, logonMode);
        fixApplication.setPendingPassword(password);

        var storeFactory   = new quickfix.FileStoreFactory(settings);
        var logFactory     = new MaskingLogFactory(new quickfix.FileLogFactory(settings));
        var messageFactory = new quickfix.DefaultMessageFactory();
        initiator = new SocketInitiator(fixApplication, storeFactory, settings, logFactory, messageFactory);
        initiator.start();

        for (SessionID sid : initiator.getSessions()) {
            sessionId = sid;
            break;
        }
        loggingOn = true;
        connectDeadline = Instant.now().plusSeconds(CONNECT_TIMEOUT_SECONDS);
        log.info("FIX initiator started, session: {}", sessionId);

        Session session = getSession();
        if (session != null) {
            session.logon();
        }
    }

    public void logout() {
        fixApplication.setPendingPassword(null);
        stopInitiator();
        fixApplication.clearLastLogoutReason();
    }

    public void disconnect() {
        Session session = getSession();
        if (session != null) {
            try {
                session.disconnect("manual disconnect", false);
            } catch (IOException e) {
                log.warn("Disconnect error", e);
            }
        }
    }

    public synchronized void stop() {
        stopInitiator();
    }

    public boolean send(Message message) {
        if (sessionId == null) {
            return false;
        }
        try {
            return Session.sendToTarget(message, sessionId);
        } catch (SessionNotFound e) {
            log.warn("Send failed: session not found", e);
            return false;
        }
    }

    public void setNextOutgoingSeqNum(int seqNum) {
        Session session = getSession();
        if (session != null) {
            try {
                session.setNextSenderMsgSeqNum(seqNum);
            } catch (IOException e) {
                log.warn("Failed to set next outgoing seq num", e);
            }
        }
    }

    public void setOverrideSeqNum(int seqNum) {
        this.overrideSeqNum = seqNum;
    }

    public boolean isLoggedOn() {
        Session session = getSession();
        return session != null && session.isLoggedOn();
    }

    public boolean isConnected() {
        return sessionId != null && getSession() != null;
    }

    public SessionStatus getStatus() {
        if (loggingOn && connectDeadline != null && Instant.now().isAfter(connectDeadline)) {
            connectError = "Connection timed out — check gateway host and port";
            stopInitiator();
            return SessionStatus.disconnected(connectError);
        }

        Session session = getSession();
        if (session == null) {
            return SessionStatus.disconnected(connectError != null ? connectError : "");
        }

        String senderCompId = "";
        String targetCompId = "";
        String host = "";
        int port = 0;

        try {
            senderCompId = sessionId.getSenderCompID();
            targetCompId = sessionId.getTargetCompID();
        } catch (Exception ignored) {
        }

        try {
            SessionSettings settings = initiator.getSettings();
            host = settings.getString(sessionId, "SocketConnectHost");
            port = (int) settings.getLong(sessionId, "SocketConnectPort");
        } catch (Exception ignored) {
        }

        int nextOut = 0;
        int nextIn  = 0;
        try {
            nextOut = session.getExpectedSenderNum();
            nextIn  = session.getExpectedTargetNum();
        } catch (Exception ignored) {
        }

        String lastError = connectError != null ? connectError : fixApplication.getLastLogoutReason();
        return new SessionStatus(
                true,
                loggingOn,
                session.isLoggedOn(),
                senderCompId,
                targetCompId,
                host,
                port,
                logonTime,
                startingSeqNum,
                nextOut,
                nextIn,
                lastError,
                fixApplication.getSuggestedSeqNum()
        );
    }

    public FixApplication fixApplication() {
        return fixApplication;
    }

    private Session getSession() {
        if (sessionId == null) {
            return null;
        }
        return Session.lookupSession(sessionId);
    }

    private void handleLogon() {
        loggingOn = false;
        connectError = null;
        logonTime = Instant.now();
        fixApplication.setPendingPassword(null);
        Session session = getSession();
        if (session != null) {
            try {
                startingSeqNum = session.getExpectedSenderNum() - 1;
            } catch (Exception ignored) {
                startingSeqNum = 1;
            }
        }
    }

    private void handleLogout() {
        logonTime = null;
    }

    private synchronized void stopInitiator() {
        loggingOn = false;
        if (initiator != null) {
            initiator.stop();
            initiator = null;
            sessionId = null;
        }
    }

    private SessionSettings buildSettings(GatewayEndpoint endpoint, String compId, String targetCompId, boolean useTls, LogonMode logonMode) {
        SessionSettings settings = new SessionSettings();

        // The login screen may override the target comp ID; fall back to the
        // configured default (config.targetCompId(), e.g. GATEWAY) when the caller
        // supplies nothing.
        String effectiveTargetCompId = (targetCompId == null || targetCompId.isBlank())
                ? config.targetCompId()
                : targetCompId.trim();

        settings.setString("ConnectionType",          "initiator");
        settings.setString("HeartBtInt",              "10");
        settings.setString("SenderCompID",            compId);
        settings.setString("TargetCompID",            effectiveTargetCompId);
        settings.setString("TransportDataDictionary", "FIXT11.xml");
        settings.setString("AppDataDictionary",       "FIX50SP2.xml");
        settings.setString("DefaultApplVerID",        "FIX.5.0SP2");
        settings.setString("FileStorePath",           "data/sessions");
        settings.setString("FileLogPath",             config.outputDir());
        settings.setString("ResetOnLogon",            logonMode == LogonMode.PROPRIETARY ? "N" : "Y");
        settings.setString("ReconnectInterval",       "5");
        settings.setString("StartTime",               "00:00:00");
        settings.setString("EndTime",                 "00:00:00");

        if (logonMode == LogonMode.PROPRIETARY) {
            // Defensive default only: FixApplication stamps every outbound SendingTime
            // and TransactTime from its JDK-independent NanoClock, so correctness does
            // not depend on this setting. It is kept so that any message path not routed
            // through toAdmin/toApp still emits a nanosecond-width UTCTimestamp rather
            // than a millisecond one the proprietary gateway would reject. The project
            // gateway keeps QuickFIX/J's millisecond default.
            settings.setString("TimestampPrecision", "NANOS");
        }

        // The endpoint IS the address. It used to be reconstructed here from three config
        // scalars and two booleans, which is why the client could only ever reach instance a:
        // there was one port per protocol and no way to express a second.
        int connectPort = endpoint.connectPort(useTls);

        SessionID sid = new SessionID("FIXT.1.1", compId, effectiveTargetCompId);
        settings.setString(sid, "BeginString",       "FIXT.1.1");
        settings.setString(sid, "SocketConnectHost", endpoint.host());
        settings.setString(sid, "SocketConnectPort", String.valueOf(connectPort));

        if (useTls) {
            settings.setString(sid, "SocketUseSSL",            "Y");
            settings.setString(sid, "SocketTrustStore",         config.trustStorePath());
            settings.setString(sid, "SocketTrustStorePassword", config.trustStorePassword());
        }

        return settings;
    }
}
