package com.pubsub.fixtestclient.capture;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import quickfix.FieldMap;
import quickfix.Message;
import quickfix.field.MsgType;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.Iterator;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public class MessageCapture {

    private static final Logger log = LoggerFactory.getLogger(MessageCapture.class);
    private static final DateTimeFormatter FILE_TIMESTAMP =
            DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss").withZone(ZoneOffset.UTC);
    private static final DateTimeFormatter MSG_TIMESTAMP =
            DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC);

    private static final Map<String, String> MSG_TYPE_NAMES = Map.of(
            "D", "NewOrderSingle",
            "8", "ExecutionReport",
            "0", "Heartbeat",
            "1", "TestRequest",
            "2", "ResendRequest",
            "3", "Reject",
            "5", "Logout",
            "A", "Logon"
    );

    private final String outputDir;
    private final BlockingQueue<Message> queue = new LinkedBlockingQueue<>();
    private final AtomicBoolean active = new AtomicBoolean(false);
    private final AtomicLong messageCount = new AtomicLong(0);
    private volatile String currentOutputPath;
    private volatile Thread writerThread;

    public MessageCapture(String outputDir) {
        this.outputDir = outputDir;
    }

    public void start() {
        if (active.compareAndSet(false, true)) {
            queue.clear();
            messageCount.set(0);
            String filename = FILE_TIMESTAMP.format(Instant.now()) + ".log";
            currentOutputPath = outputDir + File.separator + filename;
            new File(outputDir).mkdirs();
            writerThread = new Thread(this::writerLoop, "capture-writer");
            writerThread.setDaemon(true);
            writerThread.start();
            log.info("Message capture started: {}", currentOutputPath);
        }
    }

    public void stop() {
        if (active.compareAndSet(true, false)) {
            Thread thread = writerThread;
            if (thread != null) {
                thread.interrupt();
            }
            log.info("Message capture stopped after {} messages", messageCount.get());
        }
    }

    public void capture(Message message) {
        if (active.get()) {
            queue.offer(message);
        }
    }

    public CaptureStatus status() {
        return new CaptureStatus(active.get(), currentOutputPath, messageCount.get());
    }

    private void writerLoop() {
        try (PrintWriter writer = new PrintWriter(new FileWriter(currentOutputPath, StandardCharsets.UTF_8, true))) {
            while (active.get() || !queue.isEmpty()) {
                Message message = queue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS);
                if (message != null) {
                    writeMessage(writer, message);
                    messageCount.incrementAndGet();
                    writer.flush();
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (IOException e) {
            log.error("Capture writer error", e);
        }

        drainRemaining();
    }

    private void drainRemaining() {
        try (PrintWriter writer = new PrintWriter(new FileWriter(currentOutputPath, StandardCharsets.UTF_8, true))) {
            Message message;
            while ((message = queue.poll()) != null) {
                writeMessage(writer, message);
                messageCount.incrementAndGet();
            }
        } catch (IOException e) {
            log.error("Capture drain error", e);
        }
    }

    private void writeMessage(PrintWriter writer, Message message) {
        String msgType = "";
        try {
            msgType = message.getHeader().getString(MsgType.FIELD);
        } catch (Exception ignored) {
        }
        String typeName = MSG_TYPE_NAMES.getOrDefault(msgType, "Message (" + msgType + ")");

        writer.printf("=== %s  INBOUND  %s (%s) ===%n",
                MSG_TIMESTAMP.format(Instant.now()), typeName, msgType);

        writeFields(writer, message.getHeader());
        writeFields(writer, message);
        writeFields(writer, message.getTrailer());
        writer.println();
    }

    private void writeFields(PrintWriter writer, FieldMap fieldMap) {
        Iterator<quickfix.Field<?>> iterator = fieldMap.iterator();
        while (iterator.hasNext()) {
            quickfix.Field<?> field = iterator.next();
            writer.printf("%-15s (%d):  %s%n",
                    tagName(field.getTag()), field.getTag(), field.getObject());
        }
    }

    private String tagName(int tag) {
        return switch (tag) {
            case 8 -> "BeginString";
            case 9 -> "BodyLength";
            case 35 -> "MsgType";
            case 34 -> "MsgSeqNum";
            case 49 -> "SenderCompID";
            case 52 -> "SendingTime";
            case 56 -> "TargetCompID";
            case 10 -> "Checksum";
            case 11 -> "ClOrdID";
            case 14 -> "CumQty";
            case 17 -> "ExecID";
            case 37 -> "OrderID";
            case 38 -> "OrderQty";
            case 39 -> "OrdStatus";
            case 40 -> "OrdType";
            case 44 -> "Price";
            case 54 -> "Side";
            case 55 -> "Symbol";
            case 150 -> "ExecType";
            case 151 -> "LeavesQty";
            default -> "Tag";
        };
    }
}
