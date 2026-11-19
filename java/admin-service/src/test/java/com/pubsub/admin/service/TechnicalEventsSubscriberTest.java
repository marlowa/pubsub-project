package com.pubsub.admin.service;

import com.pubsub.admin.protocol.Authentication;
import com.pubsub.admin.protocol.LeaderFollower;
import com.pubsub.admin.protocol.Topics;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

class TechnicalEventsSubscriberTest {

    private static final int DAY = 20261119;

    private final List<FakePublisher> publishers = new ArrayList<>();
    private TechnicalEventsSubscriber subscriber;

    @AfterEach
    void tearDown() throws Exception {
        if (subscriber != null) {
            subscriber.stop(Duration.ofSeconds(5));
        }
        for (FakePublisher publisher : publishers) {
            publisher.close();
        }
    }

    /** A record of the topic: the TechnicalEvent entering {@code phase} on {@code tradingDay}, at {@code seqNo}. */
    private static Topics.TopicRecord technicalEvent(long seqNo, LeaderFollower.TradingPhase phase, int tradingDay) {
        LeaderFollower.TechnicalEvent event = new LeaderFollower.TechnicalEvent();
        event.phase = phase;
        event.trading_day = tradingDay;
        event.reason = LeaderFollower.TechnicalEventReason.CannotAccountForBook;
        ByteBuffer bytes = ByteBuffer.allocate(LeaderFollower.TechnicalEvent.encodedSize(event));
        LeaderFollower.TechnicalEvent.encode(event, bytes);
        Topics.TopicRecord record = new Topics.TopicRecord();
        record.seq_no = seqNo;
        record.pdu_id = (short) LeaderFollower.TechnicalEvent.PDU_ID;
        record.payload = bytes.array();
        return record;
    }

    /**
     * A publisher of the topic, on a port of its own, that answers one subscription as the real one does: if it leads it
     * accepts and sends one page; if not, it says so and keeps the connection and the request, and, when it is told to
     * come to lead after a delay, it then answers the request on that same connection.
     */
    private static final class FakePublisher implements AutoCloseable {
        final ServerSocket server = new ServerSocket(0);
        final CompletableFuture<Topics.TopicSubscribeRequest> request = new CompletableFuture<>();
        final CompletableFuture<Long> acknowledged = new CompletableFuture<>();
        final AtomicInteger connections = new AtomicInteger();
        private final Thread thread;

        FakePublisher(boolean leads, List<Topics.TopicRecord> page) throws IOException {
            this(leads, page, -1);
        }

        FakePublisher(boolean leads, List<Topics.TopicRecord> page, long comesToLeadAfterMilliseconds) throws IOException {
            thread = new Thread(() -> serve(leads, page, comesToLeadAfterMilliseconds), "fake-publisher");
            thread.setDaemon(true);
            thread.start();
        }

        TechnicalEventsSubscriber.Endpoint endpoint() {
            return new TechnicalEventsSubscriber.Endpoint("127.0.0.1", server.getLocalPort());
        }

        private void serve(boolean leads, List<Topics.TopicRecord> page, long comesToLeadAfterMilliseconds) {
            try (Socket connection = server.accept()) {
                connections.incrementAndGet();
                DataInputStream in = new DataInputStream(new BufferedInputStream(connection.getInputStream()));
                OutputStream out = connection.getOutputStream();
                request.complete(Topics.TopicSubscribeRequest.decode(ByteBuffer.wrap(receive(in, Topics.TopicSubscribeRequest.PDU_ID))));
                if (!leads) {
                    Topics.TopicNotLeader notLeader = new Topics.TopicNotLeader();
                    notLeader.publisher_role = Topics.TopicPublisherRole.Follower;
                    ByteBuffer bytes = ByteBuffer.allocate(Topics.TopicNotLeader.encodedSize(notLeader));
                    Topics.TopicNotLeader.encode(notLeader, bytes);
                    send(out, Topics.TopicNotLeader.PDU_ID, bytes.array());
                    if (comesToLeadAfterMilliseconds < 0) {
                        in.read(); // keeps the connection and the request, as a publisher that does not lead does
                        return;
                    }
                    Thread.sleep(comesToLeadAfterMilliseconds);
                }
                Topics.TopicSubscribeAck ack = new Topics.TopicSubscribeAck();
                ByteBuffer ackBytes = ByteBuffer.allocate(Topics.TopicSubscribeAck.encodedSize(ack));
                Topics.TopicSubscribeAck.encode(ack, ackBytes);
                send(out, Topics.TopicSubscribeAck.PDU_ID, ackBytes.array());
                Topics.TopicPage message = new Topics.TopicPage();
                message.records = page.toArray(new Topics.TopicRecord[0]);
                message.record_count = (short) page.size();
                message.page_number = 1;
                message.total_pages = 1;
                ByteBuffer pageBytes = ByteBuffer.allocate(Topics.TopicPage.encodedSize(message));
                Topics.TopicPage.encode(message, pageBytes);
                send(out, Topics.TopicPage.PDU_ID, pageBytes.array());
                acknowledged.complete(Topics.TopicAck.decode(ByteBuffer.wrap(receive(in, Topics.TopicAck.PDU_ID))).last_seq_no);
                in.read(); // holds the connection open until the subscriber closes it
            } catch (IOException e) {
                request.completeExceptionally(e);
                acknowledged.completeExceptionally(e);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }

        private static byte[] receive(DataInputStream in, int expectedPduId) throws IOException {
            byte[] header = new byte[Authentication.PDU_HEADER_SIZE];
            in.readFully(header);
            int[] decoded = Authentication.readHeader(ByteBuffer.wrap(header));
            byte[] payload = new byte[decoded[1]];
            in.readFully(payload);
            if (decoded[0] != expectedPduId) {
                throw new IOException("expected PDU " + expectedPduId + ", got " + decoded[0]);
            }
            return payload;
        }

        private static void send(OutputStream out, int pduId, byte[] payload) throws IOException {
            ByteBuffer header = ByteBuffer.allocate(Authentication.PDU_HEADER_SIZE);
            Authentication.writeHeader(header, pduId, payload.length);
            out.write(header.array());
            out.write(payload);
            out.flush();
        }

        @Override
        public void close() throws IOException {
            server.close();
        }
    }

    private FakePublisher publisher(boolean leads, List<Topics.TopicRecord> page) throws IOException {
        return publisher(leads, page, -1);
    }

    private FakePublisher publisher(boolean leads, List<Topics.TopicRecord> page, long comesToLeadAfterMilliseconds) throws IOException {
        FakePublisher publisher = new FakePublisher(leads, page, comesToLeadAfterMilliseconds);
        publishers.add(publisher);
        return publisher;
    }

    @Test
    void theEndOfDayEndOfItsOwnDayIsReported() throws Exception {
        FakePublisher leader = publisher(true, List.of(technicalEvent(1, LeaderFollower.TradingPhase.StartOfDay, DAY),
                technicalEvent(2, LeaderFollower.TradingPhase.EndOfDayEnd, DAY)));
        CompletableFuture<Long> ended = new CompletableFuture<>();
        subscriber = new TechnicalEventsSubscriber(List.of(leader.endpoint()), DAY, "admin_service", Duration.ofMillis(50),
                (day, seqNo) -> ended.complete(seqNo));
        subscriber.start();
        assertEquals(2L, ended.get(5, TimeUnit.SECONDS));
        Topics.TopicSubscribeRequest request = leader.request.get(5, TimeUnit.SECONDS);
        assertEquals("technical_events", request.topic_name);
        assertEquals("admin_service", request.subscriber_id);
        assertEquals(0L, request.from_seq_no);
    }

    /** A service started for one day stops only for that day's end; the page is acknowledged as applied. */
    @Test
    void theEndOfAnotherDayIsPassedOver() throws Exception {
        FakePublisher leader = publisher(true, List.of(technicalEvent(1, LeaderFollower.TradingPhase.EndOfDayEnd, DAY - 1)));
        CompletableFuture<Long> ended = new CompletableFuture<>();
        subscriber = new TechnicalEventsSubscriber(List.of(leader.endpoint()), DAY, "admin_service", Duration.ofMillis(50),
                (day, seqNo) -> ended.complete(seqNo));
        subscriber.start();
        assertEquals(1L, leader.acknowledged.get(5, TimeUnit.SECONDS));
        assertEquals(1L, subscriber.lastAppliedSeqNo());
        assertFalse(ended.isDone());
    }

    /**
     * Both publishers are asked at once; the one that leads serves, and the one that does not is waited on, over the one
     * connection it was asked on, never connected to again (docs/bug_list.md, BUG-0268).
     */
    @Test
    void theSubscriberIsServedByWhicheverPublisherLeads() throws Exception {
        FakePublisher follower = publisher(false, List.of());
        FakePublisher leader = publisher(true, List.of(technicalEvent(5, LeaderFollower.TradingPhase.EndOfDayEnd, DAY)));
        CompletableFuture<Long> ended = new CompletableFuture<>();
        subscriber = new TechnicalEventsSubscriber(List.of(follower.endpoint(), leader.endpoint()), DAY, "admin_service",
                Duration.ofMillis(50), (day, seqNo) -> ended.complete(seqNo));
        subscriber.start();
        assertEquals(5L, ended.get(5, TimeUnit.SECONDS));
        assertEquals(0L, follower.request.get(5, TimeUnit.SECONDS).from_seq_no);
        assertEquals(0L, leader.request.get(5, TimeUnit.SECONDS).from_seq_no);
        Thread.sleep(300);
        assertEquals(1, follower.connections.get());
    }

    /** A subscriber waiting on a publisher that did not lead is served on that connection when it comes to lead. */
    @Test
    void aPublisherThatComesToLeadServesTheSubscriberWaitingOnIt() throws Exception {
        FakePublisher comesToLead = publisher(false, List.of(technicalEvent(3, LeaderFollower.TradingPhase.EndOfDayEnd, DAY)), 300);
        CompletableFuture<Long> ended = new CompletableFuture<>();
        subscriber = new TechnicalEventsSubscriber(List.of(comesToLead.endpoint()), DAY, "admin_service", Duration.ofMillis(50),
                (day, seqNo) -> ended.complete(seqNo));
        subscriber.start();
        assertEquals(3L, ended.get(5, TimeUnit.SECONDS));
        assertEquals(1, comesToLead.connections.get());
    }

    @Test
    void withNoPublishersNothingIsStarted() throws Exception {
        subscriber = new TechnicalEventsSubscriber(List.of(), DAY, "admin_service", Duration.ofMillis(50), (day, seqNo) -> { });
        subscriber.start();
        assertEquals(0L, subscriber.lastAppliedSeqNo());
    }
}
