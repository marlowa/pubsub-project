package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.Config;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.LogonMode;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import com.pubsub.fixtestclient.gateway.GatewayKind;
import quickfix.Message;

public class FixSessionBinding {

    private final FixEngine fixEngine;
    private final Config config;

    public FixSessionBinding(FixEngine fixEngine, Config config) {
        this.fixEngine = fixEngine;
        this.config = config;
    }

    public void logon(String compId, String password, boolean useTls) {
        logon(compId, null, password, useTls);
    }

    public void logon(String compId, String targetCompId, String password, boolean useTls) {
        doLogon(defaultEndpoint(LogonMode.STANDARD), compId, targetCompId, password, useTls);
    }

    /**
     * Logs on to a named gateway, e.g. "fix-b".
     *
     * The venue runs more than one instance of the FIX gateway, so a script that only ever
     * reached the first one could not exercise a failover -- nor the refusal a gateway gives
     * a session provisioned elsewhere.
     */
    public void logonTo(String gatewayKey, String compId, String targetCompId, String password, boolean useTls) {
        GatewayEndpoint endpoint = config.gatewayByKey(gatewayKey).orElseThrow(
                () -> new IllegalArgumentException("no gateway '" + gatewayKey + "' is configured"));
        doLogon(endpoint, compId, targetCompId, password, useTls);
    }

    public void logonProprietary(String compId, String password) {
        logonProprietary(compId, null, password);
    }

    public void logonProprietary(String compId, String targetCompId, String password) {
        // The proprietary gateway path is plaintext only -- TLS is rejected for
        // proprietary logon -- so no useTls argument is exposed to scripts.
        doLogon(defaultEndpoint(LogonMode.PROPRIETARY), compId, targetCompId, password, false);
    }

    private void doLogon(GatewayEndpoint endpoint, String compId, String targetCompId, String password, boolean useTls) {
        try {
            fixEngine.logon(endpoint, compId, targetCompId, password, useTls);
        } catch (Exception e) {
            throw new RuntimeException("Logon failed: " + e.getMessage(), e);
        }
    }

    /** The first configured FIX endpoint speaking this dialect -- what an unqualified logon uses. */
    private GatewayEndpoint defaultEndpoint(LogonMode logonMode) {
        return config.gateways().stream()
                .filter(endpoint -> endpoint.kind() == GatewayKind.FIX && endpoint.logonMode() == logonMode)
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                        "no FIX gateway configured for " + logonMode + " logon"));
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
