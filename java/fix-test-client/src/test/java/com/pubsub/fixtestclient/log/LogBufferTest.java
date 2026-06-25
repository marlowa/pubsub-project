package com.pubsub.fixtestclient.log;

import ch.qos.logback.classic.Level;
import ch.qos.logback.classic.spi.ILoggingEvent;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

class LogBufferTest {

    private LogBuffer logBuffer;

    @BeforeEach
    void setUp() {
        logBuffer = new LogBuffer();
        logBuffer.start();
    }

    private ILoggingEvent event(Level level, String loggerName, String message) {
        ILoggingEvent event = mock(ILoggingEvent.class);
        when(event.getTimeStamp()).thenReturn(1_000_000_000_000L);
        when(event.getLevel()).thenReturn(level);
        when(event.getLoggerName()).thenReturn(loggerName);
        when(event.getFormattedMessage()).thenReturn(message);
        return event;
    }

    @Test
    void append_messageAppearsInSnapshot() {
        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "hello world"));

        List<String> snapshot = logBuffer.snapshot();
        assertEquals(1, snapshot.size());
        assertTrue(snapshot.get(0).contains("hello world"));
    }

    @Test
    void append_usesShortLoggerName() {
        logBuffer.doAppend(event(Level.INFO, "com.example.SomeService", "msg"));

        String line = logBuffer.snapshot().get(0);
        assertTrue(line.contains("SomeService"));
        assertFalse(line.contains("com.example"));
    }

    @Test
    void append_loggerWithNoDots_usesFullName() {
        logBuffer.doAppend(event(Level.INFO, "RootLogger", "msg"));

        assertTrue(logBuffer.snapshot().get(0).contains("RootLogger"));
    }

    @Test
    void append_includesLevelInOutput() {
        logBuffer.doAppend(event(Level.WARN, "com.example.Foo", "msg"));

        assertTrue(logBuffer.snapshot().get(0).contains("WARN"));
    }

    @Test
    void append_includesTimestampInOutput() {
        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "msg"));

        // Timestamp 1_000_000_000_000 ms = 2001-09-09 01:46:40 UTC → HH:mm:ss starts with "01:"
        assertTrue(logBuffer.snapshot().get(0).matches("\\d{2}:\\d{2}:\\d{2}\\.\\d{3}.*"));
    }

    @Test
    void append_bufferFull_evictsOldestEntry() {
        for (int i = 0; i < 1000; i++) {
            logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "message " + i));
        }
        assertEquals(1000, logBuffer.snapshot().size());

        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "message 1000"));

        List<String> snapshot = logBuffer.snapshot();
        assertEquals(1000, snapshot.size());
        assertFalse(snapshot.get(0).contains("message 0"));
        assertTrue(snapshot.get(999).contains("message 1000"));
    }

    @Test
    void subscribe_receivesSubsequentMessages() throws InterruptedException {
        BlockingQueue<String> queue = logBuffer.subscribe();
        try {
            logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "subscribed message"));

            String received = queue.poll(1, TimeUnit.SECONDS);
            assertNotNull(received);
            assertTrue(received.contains("subscribed message"));
        } finally {
            logBuffer.unsubscribe(queue);
        }
    }

    @Test
    void subscribe_doesNotReceiveMessagesAppendedBeforeSubscription() throws InterruptedException {
        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "before subscribe"));
        BlockingQueue<String> queue = logBuffer.subscribe();
        try {
            assertNull(queue.poll(100, TimeUnit.MILLISECONDS));
        } finally {
            logBuffer.unsubscribe(queue);
        }
    }

    @Test
    void unsubscribe_stopsDelivery() throws InterruptedException {
        BlockingQueue<String> queue = logBuffer.subscribe();
        logBuffer.unsubscribe(queue);

        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "after unsubscribe"));

        assertNull(queue.poll(100, TimeUnit.MILLISECONDS));
    }

    @Test
    void snapshot_returnsCopyNotLiveView() {
        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "first"));
        List<String> snapshot = logBuffer.snapshot();

        logBuffer.doAppend(event(Level.INFO, "com.example.Foo", "second"));

        assertEquals(1, snapshot.size());
    }

    @Test
    void start_registersInstanceAsSingleton() {
        LogBuffer newBuffer = new LogBuffer();
        newBuffer.start();

        assertSame(newBuffer, LogBuffer.getInstance());
    }
}
