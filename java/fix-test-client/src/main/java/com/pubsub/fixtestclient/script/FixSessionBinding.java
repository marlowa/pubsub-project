package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.fix.FixEngine;
import quickfix.Message;

public class FixSessionBinding {

    private final FixEngine fixEngine;

    public FixSessionBinding(FixEngine fixEngine) {
        this.fixEngine = fixEngine;
    }

    public void logon() {
        fixEngine.logon();
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
