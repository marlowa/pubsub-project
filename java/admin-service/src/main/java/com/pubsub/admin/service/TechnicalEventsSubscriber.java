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
import java.util.List;

/**
 * Subscribes to the technical_events topic on the matching engine publishers, so that the administration service learns
 * of the end of the trading day (docs/venue/start_and_end_of_day.md, section 4.4, decision 11).
 *
 * <p>One thread of its own connects to a publisher and asks for the topic after the last record it has applied. The
 * publisher that leads answers with pages, which the subscriber acknowledges one by one; the one that does not lead
 * replies TopicNotLeader and closes the connection. On a refusal or a lost connection the subscriber tries the other
 * publisher, and goes on trying them in turn, waiting the retry interval after trying both, so it keeps to whichever
 * publisher leads. A record at or below the last sequence number applied is discarded, so a record sent by both
 * publishers around a change of leader is seen once.
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

    private volatile boolean stopping;
    private volatile Socket socket;
    private volatile long lastAppliedSeqNo;
    private Thread thread;

    public TechnicalEventsSubscriber(List<Endpoint> publishers, int tradingDay, String subscriberId, Duration retryInterval,
                                     EndOfDayListener listener) {
        this.publishers = List.copyOf(publishers);
        this.tradingDay = tradingDay;
        this.subscriberId = subscriberId;
        this.retryInterval = retryInterval;
        this.listener = listener;
    }

    /** Starts following the publishers, on a thread of its own. With no publishers it does nothing. */
    public synchronized void start() {
        if (publishers.isEmpty() || thread != null) {
            return;
        }
        thread = new Thread(this::run, "technical-events-subscriber");
        thread.setDaemon(true);
        thread.start();
    }

    /** Stops following the publishers, and waits up to {@code wait} for the thread to finish. */
    public void stop(Duration wait) throws InterruptedException {
        stopping = true;
        closeSocket();
        Thread running;
        synchronized (this) {
            running = thread;
        }
        if (running != null) {
            running.interrupt();
            running.join(wait.toMillis());
        }
    }

    /** The sequence number of the last record applied. */
    public long lastAppliedSeqNo() {
        return lastAppliedSeqNo;
    }

    private void run() {
        while (!stopping) {
            for (Endpoint publisher : publishers) {
                if (stopping) {
                    return;
                }
                if (follow(publisher)) {
                    return;
                }
            }
            try {
                Thread.sleep(retryInterval.toMillis());
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    /**
     * Subscribes on one publisher and applies what it sends, until it refuses, the connection ends, or the end of the day
     * arrives.
     *
     * @return whether the end of the day arrived
     */
    private boolean follow(Endpoint publisher) {
        try (Socket connection = new Socket()) {
            socket = connection;
            connection.connect(new InetSocketAddress(publisher.host(), publisher.port()), CONNECT_TIMEOUT_MILLISECONDS);
            connection.setTcpNoDelay(true);
            DataInputStream in = new DataInputStream(new BufferedInputStream(connection.getInputStream()));
            OutputStream out = connection.getOutputStream();
            Topics.TopicSubscribeRequest request = new Topics.TopicSubscribeRequest();
            request.subscriber_id = subscriberId;
            request.topic_name = TOPIC;
            request.from_seq_no = lastAppliedSeqNo;
            request.role = Topics.TopicChannelRole.Data;
            ByteBuffer requestBytes = ByteBuffer.allocate(Topics.TopicSubscribeRequest.encodedSize(request));
            Topics.TopicSubscribeRequest.encode(request, requestBytes);
            send(out, Topics.TopicSubscribeRequest.PDU_ID, requestBytes.array());
            while (!stopping) {
                byte[] header = new byte[Authentication.PDU_HEADER_SIZE];
                in.readFully(header);
                int[] decoded = Authentication.readHeader(ByteBuffer.wrap(header));
                byte[] payload = new byte[decoded[1]];
                in.readFully(payload);
                switch (decoded[0]) {
                    case Topics.TopicSubscribeAck.PDU_ID ->
                            log.info("subscribed to {} on {}:{}, which leads, after seq={}", TOPIC, publisher.host(), publisher.port(),
                                    lastAppliedSeqNo);
                    case Topics.TopicPage.PDU_ID -> {
                        if (applyPage(payload)) {
                            return true;
                        }
                        Topics.TopicAck ack = new Topics.TopicAck();
                        ack.last_seq_no = lastAppliedSeqNo;
                        ByteBuffer ackBytes = ByteBuffer.allocate(Topics.TopicAck.encodedSize(ack));
                        Topics.TopicAck.encode(ack, ackBytes);
                        send(out, Topics.TopicAck.PDU_ID, ackBytes.array());
                    }
                    case Topics.TopicNotLeader.PDU_ID -> {
                        log.debug("{}:{} does not lead, for {}", publisher.host(), publisher.port(), TOPIC);
                        return false;
                    }
                    default -> log.debug("ignoring PDU {} from {}:{}", decoded[0], publisher.host(), publisher.port());
                }
            }
        } catch (IOException e) {
            if (!stopping) {
                log.debug("{}:{} for {}: {}", publisher.host(), publisher.port(), TOPIC, e.getMessage());
            }
        } finally {
            socket = null;
        }
        return false;
    }

    /** Applies the records of a page not yet applied. Returns whether the end of the day was among them. */
    private boolean applyPage(byte[] payload) {
        Topics.TopicPage page = Topics.TopicPage.decode(ByteBuffer.wrap(payload));
        if (page == null) {
            log.warn("a TopicPage for {} did not decode -- ignored", TOPIC);
            return false;
        }
        for (Topics.TopicRecord record : page.records) {
            if (record.seq_no <= lastAppliedSeqNo) {
                continue;
            }
            lastAppliedSeqNo = record.seq_no;
            if (record.pdu_id != LeaderFollower.TechnicalEvent.PDU_ID) {
                continue;
            }
            LeaderFollower.TechnicalEvent event = LeaderFollower.TechnicalEvent.decode(ByteBuffer.wrap(record.payload));
            if (event != null && LeaderFollower.TradingPhase.EndOfDayEnd.equals(event.phase) && event.trading_day == tradingDay) {
                listener.endOfDayReached(tradingDay, record.seq_no);
                return true;
            }
        }
        return false;
    }

    private static void send(OutputStream out, int pduId, byte[] payload) throws IOException {
        ByteBuffer header = ByteBuffer.allocate(Authentication.PDU_HEADER_SIZE);
        Authentication.writeHeader(header, pduId, payload.length);
        out.write(header.array());
        out.write(payload);
        out.flush();
    }

    private void closeSocket() {
        Socket current = socket;
        if (current != null) {
            try {
                current.close();
            } catch (IOException ignored) {
                // closing to stop: nothing more to do
            }
        }
    }
}
