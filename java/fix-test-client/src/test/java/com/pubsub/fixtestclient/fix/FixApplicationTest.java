package com.pubsub.fixtestclient.fix;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import quickfix.FieldNotFound;
import quickfix.Message;
import quickfix.Session;
import quickfix.SessionID;
import quickfix.field.MsgSeqNum;
import quickfix.field.MsgType;
import quickfix.field.Password;

import java.io.IOException;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;

class FixApplicationTest {

    private FixApplication fixApplication;
    private Session mockSession;
    private SessionID sessionId;

    @BeforeEach
    void setUp() {
        fixApplication = new FixApplication();
        mockSession = mock(Session.class);
        sessionId = new SessionID("FIXT.1.1", "SENDER", "TARGET");
        fixApplication.setSessionLookup(id -> mockSession);
    }

    private static Message logonMessage() {
        Message message = new Message();
        message.getHeader().setString(MsgType.FIELD, MsgType.LOGON);
        message.getHeader().setInt(MsgSeqNum.FIELD, 1);
        return message;
    }

    @Test
    void toAdmin_standardLogonWithPassword_stampsPasswordInMessage() throws FieldNotFound {
        fixApplication.setPendingPassword("secret");
        Message message = logonMessage();

        fixApplication.toAdmin(message, sessionId);

        assertTrue(message.isSetField(Password.FIELD));
        assertEquals("secret", message.getString(Password.FIELD));
    }

    @Test
    void toAdmin_standardLogonWithSeqNumOverride_setsMsgSeqNumInHeader() throws FieldNotFound {
        fixApplication.setPendingPassword("pass");
        fixApplication.setPendingSeqNumOverride(5);
        Message message = logonMessage();

        fixApplication.toAdmin(message, sessionId);

        assertEquals(5, message.getHeader().getInt(MsgSeqNum.FIELD));
    }

    @Test
    void toAdmin_standardLogonWithSeqNumOverride_advancesSessionNextSenderSeqNum() throws IOException {
        fixApplication.setPendingPassword("pass");
        fixApplication.setPendingSeqNumOverride(5);

        fixApplication.toAdmin(logonMessage(), sessionId);

        verify(mockSession).setNextSenderMsgSeqNum(6);
    }

    @Test
    void toAdmin_standardLogonWithSeqNumOverride_clearsPendingOverrideAfterFirstUse() throws FieldNotFound {
        fixApplication.setPendingPassword("pass");
        fixApplication.setPendingSeqNumOverride(5);
        fixApplication.toAdmin(logonMessage(), sessionId);

        Message secondLogon = logonMessage();
        fixApplication.toAdmin(secondLogon, sessionId);

        assertEquals(1, secondLogon.getHeader().getInt(MsgSeqNum.FIELD));
    }

    @Test
    void toAdmin_standardLogonWithNoOverride_doesNotChangeMsgSeqNum() throws FieldNotFound, IOException {
        fixApplication.setPendingPassword("pass");
        Message message = logonMessage();

        fixApplication.toAdmin(message, sessionId);

        assertEquals(1, message.getHeader().getInt(MsgSeqNum.FIELD));
        verify(mockSession, never()).setNextSenderMsgSeqNum(anyInt());
    }

    @Test
    void toAdmin_nonLogonMessage_isIgnoredEntirely() throws FieldNotFound, IOException {
        fixApplication.setPendingPassword("pass");
        fixApplication.setPendingSeqNumOverride(5);

        Message heartbeat = new Message();
        heartbeat.getHeader().setString(MsgType.FIELD, MsgType.HEARTBEAT);
        heartbeat.getHeader().setInt(MsgSeqNum.FIELD, 3);

        fixApplication.toAdmin(heartbeat, sessionId);

        assertEquals(3, heartbeat.getHeader().getInt(MsgSeqNum.FIELD));
        assertFalse(heartbeat.isSetField(Password.FIELD));
        verify(mockSession, never()).setNextSenderMsgSeqNum(anyInt());
    }

    @Test
    void toAdmin_standardLogonWithSeqNumOverrideAndNullSession_stillSetsMsgSeqNum() throws FieldNotFound {
        fixApplication.setSessionLookup(id -> null);
        fixApplication.setPendingPassword("pass");
        fixApplication.setPendingSeqNumOverride(7);
        Message message = logonMessage();

        fixApplication.toAdmin(message, sessionId);

        assertEquals(7, message.getHeader().getInt(MsgSeqNum.FIELD));
    }
}
