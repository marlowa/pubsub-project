package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.fix.FixEngine;
import quickfix.Message;

public class FixSessionBinding {

    private final FixEngine fixEngine;

    public FixSessionBinding(FixEngine fixEngine) {
        this.fixEngine = fixEngine;
    }

    public void logon(String compId, String password, boolean useTls) {
        try {
            fixEngine.logon(compId, password, useTls);
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
