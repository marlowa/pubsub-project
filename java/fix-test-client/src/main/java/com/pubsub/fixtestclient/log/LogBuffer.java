package com.pubsub.fixtestclient.log;

import ch.qos.logback.classic.spi.ILoggingEvent;
import ch.qos.logback.core.AppenderBase;

import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;

public class LogBuffer extends AppenderBase<ILoggingEvent> {

    private static final int CAPACITY = 1000;
    private static final DateTimeFormatter FORMATTER =
            DateTimeFormatter.ofPattern("HH:mm:ss.SSS").withZone(ZoneOffset.UTC);

    // Logback instantiates appenders via reflection, so a separate object is
    // created from the one the static initialiser makes.  start() is overridden
    // below so the Logback-activated instance replaces the placeholder.
    private static volatile LogBuffer instance = new LogBuffer();

    private final ArrayBlockingQueue<String> buffer = new ArrayBlockingQueue<>(CAPACITY);
    private final CopyOnWriteArrayList<BlockingQueue<String>> subscribers = new CopyOnWriteArrayList<>();
    private final ReentrantLock lock = new ReentrantLock();
    private final Condition newEntry = lock.newCondition();

    public static LogBuffer getInstance() {
        return instance;
    }

    @Override
    public void start() {
        super.start();
        instance = this;
    }

    @Override
    protected void append(ILoggingEvent event) {
        String line = format(event);
        if (!buffer.offer(line)) {
            buffer.poll();
            buffer.offer(line);
        }
        for (BlockingQueue<String> subscriber : subscribers) {
            subscriber.offer(line);
        }
        lock.lock();
        try {
            newEntry.signalAll();
        } finally {
            lock.unlock();
        }
    }

    public List<String> snapshot() {
        return new ArrayList<>(buffer);
    }

    public BlockingQueue<String> subscribe() {
        BlockingQueue<String> queue = new ArrayBlockingQueue<>(CAPACITY);
        subscribers.add(queue);
        return queue;
    }

    public void unsubscribe(BlockingQueue<String> queue) {
        subscribers.remove(queue);
    }

    private String format(ILoggingEvent event) {
        String timestamp = FORMATTER.format(Instant.ofEpochMilli(event.getTimeStamp()));
        String level = event.getLevel().toString();
        String logger = event.getLoggerName();
        int lastDot = logger.lastIndexOf('.');
        String shortLogger = lastDot >= 0 ? logger.substring(lastDot + 1) : logger;
        return timestamp + " " + level + " " + shortLogger + " - " + event.getFormattedMessage();
    }
}
