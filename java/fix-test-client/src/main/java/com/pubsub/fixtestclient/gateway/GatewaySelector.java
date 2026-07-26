package com.pubsub.fixtestclient.gateway;

import com.pubsub.fixtestclient.binary.BinaryEngine;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.fix.SessionStatus;

/**
 * Which of the venue's two front doors this client is currently using.
 *
 * The venue offers the same book over ASCII FIX and over the internal binary protocol. This
 * client holds one session at a time: the logon form chooses a gateway, and everything after
 * that -- the order form, the cancel button, the blotter -- follows whichever is live. That
 * keeps the question "where did this order go?" from ever arising, which a client whose
 * purpose is diagnosis should not make its user work out.
 *
 * The two engines are genuinely different rather than two configurations of one thing:
 * QuickFIX/J runs a FIX session layer with sequence numbers and heartbeats, while the binary
 * engine has a socket and a Logon. So this selects between them rather than abstracting over
 * their internals, and exposes only what the web handlers actually need in common.
 */
public final class GatewaySelector {

    private final FixEngine fixEngine;
    private final BinaryEngine binaryEngine;

    private volatile GatewayKind active = GatewayKind.FIX;

    public GatewaySelector(FixEngine fixEngine, BinaryEngine binaryEngine) {
        this.fixEngine = fixEngine;
        this.binaryEngine = binaryEngine;
    }

    public GatewayKind active() {
        return active;
    }

    /**
     * Records which gateway a logon is being attempted against.
     *
     * Refuses to switch while a session is live, rather than silently abandoning it: the
     * caller should log out first, and being told so is better than a leaked connection.
     *
     * @param kind the gateway to make active
     * @return an empty string on success, or the reason it was refused
     */
    public synchronized String setActive(GatewayKind kind) {
        if (kind == active) {
            return "";
        }
        if (isLoggedOn()) {
            return "log out of the " + active.displayName() + " gateway before switching";
        }
        active = kind;
        return "";
    }

    public boolean isLoggedOn() {
        return active == GatewayKind.BINARY ? binaryEngine.isLoggedOn() : fixEngine.isLoggedOn();
    }

    public SessionStatus status() {
        return active == GatewayKind.BINARY ? binaryEngine.getStatus() : fixEngine.getStatus();
    }

    public FixEngine fix() {
        return fixEngine;
    }

    public BinaryEngine binary() {
        return binaryEngine;
    }

    /** True when orders should be built as binary PDUs rather than QuickFIX messages. */
    public boolean isBinaryActive() {
        return active == GatewayKind.BINARY;
    }
}
