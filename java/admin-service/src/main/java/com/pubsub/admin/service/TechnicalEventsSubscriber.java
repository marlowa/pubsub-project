package com.pubsub.admin.service;

import com.pubsub.admin.protocol.Authentication;
import com.pubsub.admin.protocol.LeaderFollower;
import com.pubsub.admin.protocol.Topics;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * Subscribes to the technical_events topic on the matching engine publishers, so that the administration service learns
 * of the end of the trading day (docs/venue/start_and_end_of_day.md, section 4.4, decision 11).
 *
 * <p>One thread for each publisher connects to it and asks for the topic after the last record applied. The publisher
 * that leads answers with pages, which are acknowledged one by one on the connection they came on. The one that does not
 * lead replies TopicNotLeader and keeps both the connection and the request, and answers it as soon as it becomes the
 * leader, so the thread simply goes on waiting on that connection, and no connection is opened or closed while the
 * publishers keep their roles (docs/bug_list.md, BUG-0268). A thread whose connection ends connects again after the
 * retry interval. Records are applied under one lock, and one at or below the last sequence number applied is
 * discarded, so a record sent by both publishers around a change of leader is seen once.
 *
 * <p>When the end-of-day end of the service's own trading day arrives, the subscriber calls its listener once and
 * stops. The end of another day is not the service's, and is passed over.
 */
public final class TechnicalEventsSubscriber {

    /** A publisher's technical_events listener. */
    public record Endpoint(String host, int port) {
    }

    /** Told once, on the end-of-day end of the subscriber's trading day. */
    @FunctionalInterface
    public interface EndOfDayListener {
        void endOfDayReached(int tradingDay, long seqNo);
    }

    private static final Logger log = LoggerFactory.getLogger(TechnicalEventsSubscriber.class);
    private static final String TOPIC = "technical_events";
    private static final int CONNECT_TIMEOUT_MILLISECONDS = 2000;

    private final List<Endpoint> publishers;
    private final int tradingDay;
    private final String subscriberId;
    private final Duration retryInterval;
    private final EndOfDayListener listener;

    private final Object applying = new Object();
    private final List<Thread> threads = new ArrayList<>();
    private final List<Socket> sockets = new ArrayList<>();
    private volatile boolean stopping;
    private long lastAppliedSeqNo;
    private boolean ended;

    public TechnicalEventsSubscriber(List<Endpoint> publishers, int tradingDay, String subscriberId, Duration retryInterval,
                                     EndOfDayListener listener) {
        this.publishers = List.copyOf(publishers);
        this.tradingDay = tradingDay;
        this.subscriberId = subscriberId;
        this.retryInterval = retryInterval;
        this.listener = listener;
    }

    /** Starts following the publishers, one thread for each. With no publishers it does nothing. */
    public synchronized void start() {
        if (publishers.isEmpty() || !threads.isEmpty()) {
            return;
        }
        for (Endpoint publisher : publishers) {
            Thread thread = new Thread(() -> follow(publisher), "technical-events-" + publisher.host() + ":" + publisher.port());
            thread.setDaemon(true);
            threads.add(thread);
        }
        threads.forEach(Thread::start);
    }

    /** Stops following the publishers, and waits up to {@code wait} for each thread to finish. */
    public void stop(Duration wait) throws InterruptedException {
        stopping = true;
        List<Thread> running;
        synchronized (this) {
            sockets.forEach(TechnicalEventsSubscriber::close);
            running = new ArrayList<>(threads);
        }
        for (Thread thread : running) {
            thread.interrupt();
            thread.join(wait.toMillis());
        }
    }

    /** The sequence number of the last record applied. */
    public long lastAppliedSeqNo() {
        synchronized (applying) {
            return lastAppliedSeqNo;
        }
    }

    /** Follows one publisher until the end of the day or a stop, connecting again after each loss. */
    private void follow(Endpoint publisher) {
        while (!stopping && !hasEnded()) {
            followOnce(publisher);
            if (stopping || hasEnded()) {
                return;
            }
            try {
                Thread.sleep(retryInterval.toMillis());
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    /** Subscribes on one connection to the publisher and applies what it sends, until the connection ends or the day does. */
    private void followOnce(Endpoint publisher) {
        try (Socket connection = new Socket()) {
            synchronized (this) {
                if (stopping) {
                    return;
                }
                sockets.add(connection);
            }
            connection.connect(new InetSocketAddress(publisher.host(), publisher.port()), CONNECT_TIMEOUT_MILLISECONDS);
            connection.setTcpNoDelay(true);
            DataInputStream in = new DataInputStream(new BufferedInputStream(connection.getInputStream()));
            OutputStream out = connection.getOutputStream();
            Topics.TopicSubscribeRequest request = new Topics.TopicSubscribeRequest();
            request.subscriber_id = subscriberId;
            request.topic_name = TOPIC;
            request.from_seq_no = lastAppliedSeqNo();
            request.role = Topics.TopicChannelRole.Data;
            ByteBuffer requestBytes = ByteBuffer.allocate(Topics.TopicSubscribeRequest.encodedSize(request));
            Topics.TopicSubscribeRequest.encode(request, requestBytes);
            send(out, Topics.TopicSubscribeRequest.PDU_ID, requestBytes.array());
            while (!stopping && !hasEnded()) {
                byte[] header = new byte[Authentication.PDU_HEADER_SIZE];
                in.readFully(header);
                int[] decoded = Authentication.readHeader(ByteBuffer.wrap(header));
                byte[] payload = new byte[decoded[1]];
                in.readFully(payload);
                switch (decoded[0]) {
                    case Topics.TopicSubscribeAck.PDU_ID ->
                            log.info("subscribed to {} on {}:{}, which leads, after seq={}", TOPIC, publisher.host(), publisher.port(),
                                    lastAppliedSeqNo());
                    case Topics.TopicPage.PDU_ID -> {
                        long acknowledged = applyPage(payload);
                        if (hasEnded()) {
                            return;
                        }
                        Topics.TopicAck ack = new Topics.TopicAck();
                        ack.last_seq_no = acknowledged;
                        ByteBuffer ackBytes = ByteBuffer.allocate(Topics.TopicAck.encodedSize(ack));
                        Topics.TopicAck.encode(ack, ackBytes);
                        send(out, Topics.TopicAck.PDU_ID, ackBytes.array());
                    }
                    case Topics.TopicNotLeader.PDU_ID ->
                            log.debug("{}:{} does not lead, for {} -- waiting on it in case it comes to lead", publisher.host(), publisher.port(),
                                    TOPIC);
                    default -> log.debug("ignoring PDU {} from {}:{}", decoded[0], publisher.host(), publisher.port());
                }
            }
        } catch (IOException e) {
            if (!stopping && !hasEnded()) {
                log.debug("{}:{} for {}: {}", publisher.host(), publisher.port(), TOPIC, e.getMessage());
            }
        } finally {
            synchronized (this) {
                sockets.removeIf(socket -> socket.isClosed());
            }
        }
    }

    private boolean hasEnded() {
        synchronized (applying) {
            return ended;
        }
    }

    /**
     * Applies the records of a page not yet applied, under the lock all the threads share, and calls the listener, once,
     * on the end of the day. Returns the last sequence number applied, to acknowledge.
     */
    private long applyPage(byte[] payload) {
        Topics.TopicPage page = Topics.TopicPage.decode(ByteBuffer.wrap(payload));
        if (page == null) {
            log.warn("a TopicPage for {} did not decode -- ignored", TOPIC);
            return lastAppliedSeqNo();
        }
        long endSeqNo = 0;
        synchronized (applying) {
            for (Topics.TopicRecord record : page.records) {
                if (ended || record.seq_no <= lastAppliedSeqNo) {
                    continue;
                }
                lastAppliedSeqNo = record.seq_no;
                if (record.pdu_id != LeaderFollower.TechnicalEvent.PDU_ID) {
                    continue;
                }
                LeaderFollower.TechnicalEvent event = LeaderFollower.TechnicalEvent.decode(ByteBuffer.wrap(record.payload));
                if (event != null && LeaderFollower.TradingPhase.EndOfDayEnd.equals(event.phase) && event.trading_day == tradingDay) {
                    ended = true;
                    endSeqNo = record.seq_no;
                }
            }
            if (endSeqNo == 0) {
                return lastAppliedSeqNo;
            }
        }
        // Outside the lock, so that a listener that stops the service does not hold it.
        listener.endOfDayReached(tradingDay, endSeqNo);
        return endSeqNo;
    }

    private static void send(OutputStream out, int pduId, byte[] payload) throws IOException {
        ByteBuffer header = ByteBuffer.allocate(Authentication.PDU_HEADER_SIZE);
        Authentication.writeHeader(header, pduId, payload.length);
        out.write(header.array());
        out.write(payload);
        out.flush();
    }

    private static void close(Socket socket) {
        try {
            socket.close();
        } catch (IOException ignored) {
            // closing to stop: nothing more to do
        }
    }
}
