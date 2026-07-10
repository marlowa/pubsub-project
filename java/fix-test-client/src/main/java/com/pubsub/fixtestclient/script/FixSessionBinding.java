package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.LogonMode;
import quickfix.Message;

public class FixSessionBinding {

    private final FixEngine fixEngine;

    public FixSessionBinding(FixEngine fixEngine) {
        this.fixEngine = fixEngine;
    }

    public void logon(String compId, String password, boolean useTls) {
        logon(compId, null, password, useTls);
    }

    public void logon(String compId, String targetCompId, String password, boolean useTls) {
        doLogon(compId, targetCompId, password, useTls, LogonMode.STANDARD);
    }

    public void logonProprietary(String compId, String password) {
        logonProprietary(compId, null, password);
    }

    public void logonProprietary(String compId, String targetCompId, String password) {
        // The proprietary gateway path is plaintext only -- TLS is rejected for
        // proprietary logon -- so no useTls argument is exposed to scripts.
        doLogon(compId, targetCompId, password, false, LogonMode.PROPRIETARY);
    }

    private void doLogon(String compId, String targetCompId, String password, boolean useTls, LogonMode logonMode) {
        try {
            fixEngine.logon(compId, targetCompId, password, useTls, logonMode);
        } catch (Exception e) {
            throw new RuntimeException("Logon failed: " + e.getMessage(), e);
        }
    }

    public void logout() {
        fixEngine.logout();
    }

    public void disconnect() {
        fixEngine.disconnect();
    }

    public void setNextOutgoingSeqNum(int seqNum) {
        fixEngine.setNextOutgoingSeqNum(seqNum);
    }

    public boolean isLoggedOn() {
        return fixEngine.isLoggedOn();
    }

    public boolean send(Message message) {
        return fixEngine.send(message);
    }
}
