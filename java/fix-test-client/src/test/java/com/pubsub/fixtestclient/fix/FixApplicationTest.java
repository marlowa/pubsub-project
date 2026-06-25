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
import quickfix.field.Text;

import java.io.IOException;

import java.util.concurrent.atomic.AtomicBoolean;

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

    // ── fromAdmin ─────────────────────────────────────────────────────────────

    @Test
    void fromAdmin_logoutWithText_setsLastLogoutReason() {
        fixApplication.fromAdmin(logoutMessage("Connection limit exceeded"), sessionId);

        assertEquals("Connection limit exceeded", fixApplication.getLastLogoutReason());
    }

    @Test
    void fromAdmin_logoutWithoutText_setsGenericReason() {
        Message logout = new Message();
        logout.getHeader().setString(MsgType.FIELD, MsgType.LOGOUT);

        fixApplication.fromAdmin(logout, sessionId);

        assertEquals("Gateway closed the session", fixApplication.getLastLogoutReason());
    }

    @Test
    void fromAdmin_logoutWithExpectingPattern_setsSuggestedSeqNum() {
        fixApplication.fromAdmin(logoutMessage("MsgSeqNum too low, expecting 5 received 1"), sessionId);

        assertEquals(5, fixApplication.getSuggestedSeqNum());
    }

    @Test
    void fromAdmin_logoutExpectingPatternIsCaseInsensitive() {
        fixApplication.fromAdmin(logoutMessage("EXPECTING 12 but received 1"), sessionId);

        assertEquals(12, fixApplication.getSuggestedSeqNum());
    }

    @Test
    void fromAdmin_logoutWithNoExpectingPattern_doesNotSetSuggestedSeqNum() {
        fixApplication.fromAdmin(logoutMessage("Session terminated by administrator"), sessionId);

        assertEquals(0, fixApplication.getSuggestedSeqNum());
    }

    @Test
    void fromAdmin_rejectWithText_setsLastLogoutReasonWithPrefix() {
        fixApplication.fromAdmin(rejectMessage("Invalid logon credentials"), sessionId);

        assertEquals("Rejected: Invalid logon credentials", fixApplication.getLastLogoutReason());
    }

    @Test
    void fromAdmin_rejectWithoutText_setsGenericRejectReason() {
        Message reject = new Message();
        reject.getHeader().setString(MsgType.FIELD, MsgType.REJECT);

        fixApplication.fromAdmin(reject, sessionId);

        assertEquals("Session rejected by gateway", fixApplication.getLastLogoutReason());
    }

    // ── onLogon / onLogout lifecycle ──────────────────────────────────────────

    @Test
    void onLogon_clearsLastLogoutReason() {
        fixApplication.fromAdmin(logoutMessage("some error"), sessionId);
        fixApplication.onLogon(sessionId);

        assertEquals("", fixApplication.getLastLogoutReason());
    }

    @Test
    void onLogon_clearsSuggestedSeqNum() {
        fixApplication.fromAdmin(logoutMessage("expecting 5 received 1"), sessionId);
        fixApplication.onLogon(sessionId);

        assertEquals(0, fixApplication.getSuggestedSeqNum());
    }

    @Test
    void onLogon_invokesRegisteredCallback() {
        AtomicBoolean called = new AtomicBoolean(false);
        fixApplication.setOnLogon(() -> called.set(true));

        fixApplication.onLogon(sessionId);

        assertTrue(called.get());
    }

    @Test
    void onLogout_invokesRegisteredCallback() {
        AtomicBoolean called = new AtomicBoolean(false);
        fixApplication.setOnLogout(() -> called.set(true));

        fixApplication.onLogout(sessionId);

        assertTrue(called.get());
    }

    @Test
    void onLogout_whenNeverEstablished_andNoReason_setsGenericReason() {
        fixApplication.onLogout(sessionId);

        assertEquals("Gateway rejected logon", fixApplication.getLastLogoutReason());
    }

    @Test
    void onLogout_whenNeverEstablished_andReasonAlreadySet_doesNotOverride() {
        fixApplication.fromAdmin(logoutMessage("Specific error from gateway"), sessionId);
        fixApplication.onLogout(sessionId);

        assertEquals("Specific error from gateway", fixApplication.getLastLogoutReason());
    }

    @Test
    void onLogout_afterSuccessfulLogon_doesNotSetGenericReason() {
        fixApplication.onLogon(sessionId);
        fixApplication.onLogout(sessionId);

        assertEquals("", fixApplication.getLastLogoutReason());
    }

    @Test
    void clearLastLogoutReason_clearsReason() {
        fixApplication.fromAdmin(logoutMessage("some error"), sessionId);
        fixApplication.clearLastLogoutReason();

        assertEquals("", fixApplication.getLastLogoutReason());
    }

    // ── helpers ───────────────────────────────────────────────────────────────

    private static Message logoutMessage(String text) {
        Message message = new Message();
        message.getHeader().setString(MsgType.FIELD, MsgType.LOGOUT);
        message.setString(Text.FIELD, text);
        return message;
    }

    private static Message rejectMessage(String text) {
        Message message = new Message();
        message.getHeader().setString(MsgType.FIELD, MsgType.REJECT);
        message.setString(Text.FIELD, text);
        return message;
    }
}
