package com.pubsub.fixtestclient.fix;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import quickfix.Log;

import java.util.regex.Pattern;

/**
 * Wraps a delegate {@link Log} and replaces tag 554 (Password) values with
 * {@code ***} before any message is written, so plaintext passwords never
 * appear in FIX log files.
 */
class MaskingLog implements Log {

    private static final Logger log = LoggerFactory.getLogger(MaskingLog.class);

    // FIX fields are delimited by SOH (). Match tag 554 up to the
    // next delimiter so the replacement leaves the SOH intact.
    private static final Pattern PASSWORD_TAG =
            Pattern.compile("554=[^]*");
    private static final String PASSWORD_REPLACEMENT = "554=***";

    private final Log delegate;

    MaskingLog(Log delegate) {
        this.delegate = delegate;
    }

    private static String mask(String message) {
        return PASSWORD_TAG.matcher(message).replaceAll(PASSWORD_REPLACEMENT);
    }

    @Override
    public void onIncoming(String message) {
        String masked = mask(message);
        String readable = masked.replace('', '|');
        log.info("FIX IN:  {}", readable);
        delegate.onIncoming(readable);
    }

    @Override
    public void onOutgoing(String message) {
        String masked = mask(message);
        String readable = masked.replace('', '|');
        log.info("FIX OUT: {}", readable);
        delegate.onOutgoing(readable);
    }

    @Override
    public void onEvent(String text) {
        delegate.onEvent(text);
    }

    @Override
    public void onErrorEvent(String text) {
        delegate.onErrorEvent(text);
    }

    @Override
    public void clear() {
        delegate.clear();
    }
}
