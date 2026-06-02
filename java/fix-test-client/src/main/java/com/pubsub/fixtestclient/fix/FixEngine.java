package com.pubsub.fixtestclient.fix;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import quickfix.ConfigError;
import quickfix.Message;
import quickfix.Session;
import quickfix.SessionID;
import quickfix.SessionNotFound;
import quickfix.SessionSettings;
import quickfix.SocketInitiator;
import java.io.FileInputStream;
import java.io.IOException;
import java.time.Instant;

public class FixEngine {

    private static final Logger log = LoggerFactory.getLogger(FixEngine.class);

    private final String sessionConfigPath;
    private final FixApplication fixApplication;
    private SocketInitiator initiator;
    private SessionID sessionId;

    private volatile Instant logonTime;
    private volatile int startingSeqNum;
    private volatile Integer overrideSeqNum;

    public FixEngine(String sessionConfigPath, FixApplication fixApplication) {
        this.sessionConfigPath = sessionConfigPath;
        this.fixApplication = fixApplication;
        fixApplication.setOnLogon(this::handleLogon);
        fixApplication.setOnLogout(this::handleLogout);
    }

    public synchronized void start() throws ConfigError, IOException {
        if (initiator != null) {
            return;
        }
        SessionSettings settings;
        try (FileInputStream stream = new FileInputStream(sessionConfigPath)) {
            settings = new SessionSettings(stream);
        }
        var storeFactory = new quickfix.FileStoreFactory(settings);
        var logFactory = new quickfix.FileLogFactory(settings);
        var messageFactory = new quickfix.DefaultMessageFactory();
        initiator = new SocketInitiator(fixApplication, storeFactory, settings, logFactory, messageFactory);
        initiator.start();

        for (SessionID sid : initiator.getSessions()) {
            sessionId = sid;
            break;
        }
        log.info("FIX initiator started, session: {}", sessionId);
    }

    public synchronized void stop() {
        if (initiator != null) {
            initiator.stop();
            initiator = null;
            sessionId = null;
        }
    }

    public void logon() {
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
        Session session = getSession();
        if (session != null) {
            session.logout();
        }
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
        int nextIn = 0;
        try {
            nextOut = session.getExpectedSenderNum();
            nextIn = session.getExpectedTargetNum();
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
                nextIn
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
}
