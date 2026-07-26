package com.pubsub.fixtestclient.gateway;

/** Which of the venue's front doors a session is using. */
public enum GatewayKind {

    /** The ASCII FIX 5.0 SP2 gateway, spoken by QuickFIX/J. */
    FIX("FIX"),

    /** The binary gateway, spoken as framed PDUs generated from the same DSL as the C++ side. */
    BINARY("binary");

    private final String displayName;

    GatewayKind(String displayName) {
        this.displayName = displayName;
    }

    public String displayName() {
        return displayName;
    }

    /** Parses the value the web form sends, defaulting to FIX for anything unrecognised. */
    public static GatewayKind fromFormValue(String value) {
        return "binary".equalsIgnoreCase(value) ? BINARY : FIX;
    }
}
