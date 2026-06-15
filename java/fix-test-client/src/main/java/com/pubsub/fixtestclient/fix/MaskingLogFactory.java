package com.pubsub.fixtestclient.fix;

import quickfix.Log;
import quickfix.LogFactory;
import quickfix.SessionID;

/**
 * Wraps a delegate {@link LogFactory} and ensures every {@link Log} it
 * produces passes through {@link MaskingLog}, which strips tag 554
 * (Password) values before writing to disk.
 */
public class MaskingLogFactory implements LogFactory {

    private final LogFactory delegate;

    public MaskingLogFactory(LogFactory delegate) {
        this.delegate = delegate;
    }

    @Override
    public Log create(SessionID sessionID) {
        return new MaskingLog(delegate.create(sessionID));
    }
}
