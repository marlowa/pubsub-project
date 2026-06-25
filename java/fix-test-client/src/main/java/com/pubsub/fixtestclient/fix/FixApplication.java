package com.pubsub.fixtestclient.fix;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import quickfix.Application;
import quickfix.DoNotSend;
import quickfix.FieldNotFound;
import quickfix.Message;
import quickfix.Session;
import quickfix.SessionID;
import quickfix.UnsupportedMessageType;
import quickfix.field.MsgSeqNum;
import quickfix.field.MsgType;
import quickfix.field.Password;
import quickfix.field.Text;

import java.io.IOException;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class FixApplication implements Application {

    private static final Logger log = LoggerFactory.getLogger(FixApplication.class);

    private static final DateTimeFormatter NANO_SENDING_TIME =
            DateTimeFormatter.ofPattern("yyyyMMdd-HH:mm:ss.SSSSSSSSS").withZone(ZoneOffset.UTC);

    private static final Pattern EXPECTING_SEQ_NUM = Pattern.compile("(?i)expecting\\s+(\\d+)");

    private static final int TAG_SENDING_TIME          = 52;
    private static final int TAG_RESET_SEQ_NUM_FLAG    = 141;
    private static final int TAG_PASSWORD              = 554;
    private static final int TAG_NEXT_EXPECTED_SEQ_NUM = 789;
    private static final int TAG_ENCRYPT_METHOD        = 1400;
    private static final int TAG_ENCRYPTED_PASSWORD    = 1402;

    private final BlockingQueue<Message> inboundQueue = new LinkedBlockingQueue<>();
    private volatile Consumer<Message> inboundListener = msg -> { };
    private volatile Runnable onLogon = () -> { };
    private volatile Runnable onLogout = () -> { };
    private volatile String lastLogoutReason = "";
    private volatile boolean sessionWasEstablished = false;
    private volatile String pendingPassword = null;
    private volatile LogonMode logonMode = LogonMode.STANDARD;
    private volatile Integer pendingSeqNumOverride = null;
    private Function<SessionID, Session> sessionLookup = Session::lookupSession;
    private volatile int suggestedSeqNum = 0;

    @Override
    public void onCreate(SessionID sessionId) {
        log.info("Session created: {}", sessionId);
    }

    @Override
    public void onLogon(SessionID sessionId) {
        log.info("Logged on: {}", sessionId);
        lastLogoutReason = "";
        suggestedSeqNum = 0;
        sessionWasEstablished = true;
        onLogon.run();
    }

    @Override
    public void onLogout(SessionID sessionId) {
        log.info("Logged out: {}", sessionId);
        if (!sessionWasEstablished && lastLogoutReason.isEmpty()) {
            lastLogoutReason = "Gateway rejected logon";
        }
        sessionWasEstablished = false;
        onLogout.run();
    }

    @Override
    public void toAdmin(Message message, SessionID sessionId) {
        try {
            if (!MsgType.LOGON.equals(message.getHeader().getString(MsgType.FIELD))) {
                return;
            }
            if (logonMode == LogonMode.PROPRIETARY) {
                applyProprietaryLogon(message, sessionId);
            } else {
                applyStandardLogon(message, sessionId);
            }
        } catch (FieldNotFound ignored) {
        }
    }

    private void applyProprietaryLogon(Message message, SessionID sessionId) {
        message.getHeader().setString(TAG_SENDING_TIME, NANO_SENDING_TIME.format(Instant.now()));

        // TODO: tag 1402 requires an encryption step before the password is placed here.
        //       For now the plaintext password is sent as-is to satisfy the gateway in dev mode.
        String password = pendingPassword;
        message.setInt(TAG_ENCRYPT_METHOD, 101);
        message.setString(TAG_ENCRYPTED_PASSWORD, password != null ? password : "");

        message.removeField(TAG_RESET_SEQ_NUM_FLAG);
        message.removeField(TAG_PASSWORD);

        Integer seqOverride = pendingSeqNumOverride;
        if (seqOverride != null) {
            // Apply the override to tag 34 as well so the gateway sees the correct
            // outgoing sequence number (ResetOnLogon=N means QF/J uses the file
            // store, which may differ from what the user specified).
            message.getHeader().setInt(MsgSeqNum.FIELD, seqOverride);
            Session session = sessionLookup.apply(sessionId);
            if (session != null) {
                try {
                    session.setNextSenderMsgSeqNum(seqOverride + 1);
                } catch (IOException ignored) {
                }
            }
            // Not cleared: reconnect attempts reuse the same override.
        }

        message.setInt(TAG_NEXT_EXPECTED_SEQ_NUM, seqOverride != null ? seqOverride : 1);
    }

    private void applyStandardLogon(Message message, SessionID sessionId) {
        String password = pendingPassword;
        if (password != null && !password.isEmpty()) {
            message.setField(new Password(password));
        }

        Integer seqOverride = pendingSeqNumOverride;
        if (seqOverride != null) {
            // Override MsgSeqNum directly in the message and advance the session
            // state to match, so subsequent messages continue from seqOverride+1.
            // This is done here rather than after initiator.start() because
            // ResetOnLogon=Y would otherwise reset the value to 1 before the
            // logon fires.
            message.getHeader().setInt(MsgSeqNum.FIELD, seqOverride);
            Session session = sessionLookup.apply(sessionId);
            if (session != null) {
                try {
                    session.setNextSenderMsgSeqNum(seqOverride + 1);
                } catch (IOException ignored) {
                }
            }
            pendingSeqNumOverride = null;
        }
    }

    @Override
    public void fromAdmin(Message message, SessionID sessionId) {
        try {
            String msgType = message.getHeader().getString(MsgType.FIELD);
            if (MsgType.LOGOUT.equals(msgType)) {
                String text = message.isSetField(Text.FIELD)
                        ? message.getString(Text.FIELD)
                        : "Gateway closed the session";
                lastLogoutReason = text;
                log.info("Logout received, reason: {}", text);
                Matcher matcher = EXPECTING_SEQ_NUM.matcher(text);
                if (matcher.find()) {
                    try {
                        suggestedSeqNum = Integer.parseInt(matcher.group(1));
                        log.info("Gateway expects sequence number: {}", suggestedSeqNum);
                    } catch (NumberFormatException ignored) {
                    }
                }
            } else if (MsgType.REJECT.equals(msgType)) {
                lastLogoutReason = message.isSetField(Text.FIELD)
                        ? "Rejected: " + message.getString(Text.FIELD)
                        : "Session rejected by gateway";
                log.info("Session reject received: {}", lastLogoutReason);
            }
        } catch (FieldNotFound ignored) {
        }
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

    public String getLastLogoutReason() {
        return lastLogoutReason;
    }

    public void clearLastLogoutReason() {
        lastLogoutReason = "";
    }

    public void setPendingPassword(String password) {
        this.pendingPassword = password;
    }

    public void setLogonMode(LogonMode mode) {
        this.logonMode = mode;
    }

    public void setPendingSeqNumOverride(Integer seqNum) {
        this.pendingSeqNumOverride = seqNum;
    }

    void setSessionLookup(Function<SessionID, Session> lookup) {
        this.sessionLookup = lookup;
    }

    public int getSuggestedSeqNum() {
        return suggestedSeqNum;
    }

    public void clearSuggestedSeqNum() {
        suggestedSeqNum = 0;
    }
}
