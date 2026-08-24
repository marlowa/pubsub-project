package com.pubsub.fixtestclient.gateway;

import com.pubsub.fixtestclient.fix.LogonMode;

/**
 * One address this client has been told it may log on to.
 *
 * The venue runs several gateways -- two instances of each protocol -- and a member is given
 * its endpoints out of band, exactly as a real venue provisions them (see
 * docs/availability/gateway_ha.md). So the client is CONFIGURED with this list rather than
 * discovering it: reading the venue's own deployed configs would work only on the venue host
 * and would quietly abandon that principle.
 *
 * The endpoint, not a protocol, is what the logon page offers. Protocol and instance are two
 * axes that between them pick one address, and asking a user to combine them correctly --
 * then not showing which address resulted -- is how instance b came to be unreachable in the
 * first place: the client held one port per protocol and it was always instance a's.
 *
 * @param key        stable identifier the web form posts back, e.g. "fix-a"
 * @param label      what the logon page shows, e.g. "FIX a"
 * @param kind       which engine speaks to it -- the two are genuinely different clients
 * @param host       address to connect to
 * @param port       plain (non-TLS) port
 * @param tlsPort    TLS port, or 0 when this endpoint offers none
 * @param logonMode  the logon dialect this endpoint expects
 */
public record GatewayEndpoint(
        String key,
        String label,
        GatewayKind kind,
        String host,
        int port,
        int tlsPort,
        LogonMode logonMode
) {
    /**
     * @return true when this endpoint can be reached over TLS.
     *
     * The logon page disables its TLS control for endpoints that cannot, rather than
     * offering a choice that would fail: the binary gateway has no TLS listener, and the
     * proprietary FIX dialect is defined as plaintext.
     */
    public boolean supportsTls() {
        return tlsPort > 0;
    }

    /** @return the port a connection should use, given whether TLS was asked for. */
    public int connectPort(boolean useTls) {
        return useTls && supportsTls() ? tlsPort : port;
    }
}
