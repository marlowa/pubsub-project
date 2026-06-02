package com.pubsub.fixtestclient.fix;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import quickfix.Application;
import quickfix.DoNotSend;
import quickfix.FieldNotFound;
import quickfix.Message;
import quickfix.SessionID;
import quickfix.UnsupportedMessageType;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.function.Consumer;

public class FixApplication implements Application {

    private static final Logger log = LoggerFactory.getLogger(FixApplication.class);

    private final BlockingQueue<Message> inboundQueue = new LinkedBlockingQueue<>();
    private volatile Consumer<Message> inboundListener = msg -> { };
    private volatile Runnable onLogon = () -> { };
    private volatile Runnable onLogout = () -> { };

    @Override
    public void onCreate(SessionID sessionId) {
        log.info("Session created: {}", sessionId);
    }

    @Override
    public void onLogon(SessionID sessionId) {
        log.info("Logged on: {}", sessionId);
        onLogon.run();
    }

    @Override
    public void onLogout(SessionID sessionId) {
        log.info("Logged out: {}", sessionId);
        onLogout.run();
    }

    @Override
    public void toAdmin(Message message, SessionID sessionId) {
    }

    @Override
    public void fromAdmin(Message message, SessionID sessionId) {
    }

    @Override
    public void toApp(Message message, SessionID sessionId) throws DoNotSend {
    }

    @Override
    public void fromApp(Message message, SessionID sessionId)
            throws FieldNotFound, UnsupportedMessageType {
        inboundListener.accept(message);
        inboundQueue.offer(message);
    }

    public BlockingQueue<Message> inboundQueue() {
        return inboundQueue;
    }

    public void setInboundListener(Consumer<Message> listener) {
        this.inboundListener = listener;
    }

    public void setOnLogon(Runnable callback) {
        this.onLogon = callback;
    }

    public void setOnLogout(Runnable callback) {
        this.onLogout = callback;
    }
}
