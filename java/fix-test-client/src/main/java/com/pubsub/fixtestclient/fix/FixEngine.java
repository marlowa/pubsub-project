package com.pubsub.fixtestclient.fix;

import com.pubsub.fixtestclient.Config;
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

    private final Config config;
    private final FixApplication fixApplication;
    private SocketInitiator initiator;
    private SessionID sessionId;

    private volatile Instant logonTime;
    private volatile int startingSeqNum;
    private volatile Integer overrideSeqNum;

    public FixEngine(Config config, FixApplication fixApplication) {
        this.config = config;
        this.fixApplication = fixApplication;
        fixApplication.setOnLogon(this::handleLogon);
        fixApplication.setOnLogout(this::handleLogout);
    }

    public synchronized void logon(String compId, String password, boolean useTls)
            throws ConfigError, IOException {
        stopInitiator();

        SessionSettings settings = buildSettings(compId, useTls);
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
        log.info("FIX initiator started, session: {}", sessionId);

        if (overrideSeqNum != null) {
            setNextOutgoingSeqNum(overrideSeqNum);
            overrideSeqNum = null;
        }

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
        Session session = getSession();
        if (session == null) {
            return SessionStatus.disconnected();
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

        return new SessionStatus(
                true,
                session.isLoggedOn(),
                senderCompId,
                targetCompId,
                host,
                port,
                logonTime,
                startingSeqNum,
                nextOut,
                nextIn,
                fixApplication.getLastLogoutReason()
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
        if (initiator != null) {
            initiator.stop();
            initiator = null;
            sessionId = null;
        }
    }

    private SessionSettings buildSettings(String compId, boolean useTls) {
        SessionSettings settings = new SessionSettings();

        settings.setString("ConnectionType",          "initiator");
        settings.setString("HeartBtInt",              "10");
        settings.setString("SenderCompID",            compId);
        settings.setString("TargetCompID",            config.targetCompId());
        settings.setString("TransportDataDictionary", "FIXT11.xml");
        settings.setString("AppDataDictionary",       "FIX50SP2.xml");
        settings.setString("DefaultApplVerID",        "FIX.5.0SP2");
        settings.setString("FileStorePath",           "data/sessions");
        settings.setString("FileLogPath",             "logs/fix");
        settings.setString("ResetOnLogon",            "Y");
        settings.setString("ReconnectInterval",       "5");
        settings.setString("StartTime",               "00:00:00");
        settings.setString("EndTime",                 "00:00:00");

        SessionID sid = new SessionID("FIXT.1.1", compId, config.targetCompId());
        settings.setString(sid, "BeginString",       "FIXT.1.1");
        settings.setString(sid, "SocketConnectHost", config.gatewayHost());
        settings.setString(sid, "SocketConnectPort", String.valueOf(config.gatewayPort()));

        if (useTls) {
            settings.setString(sid, "SocketUseSSL",            "Y");
            settings.setString(sid, "SocketTrustStore",         config.trustStorePath());
            settings.setString(sid, "SocketTrustStorePassword", config.trustStorePassword());
        }

        return settings;
    }
}
