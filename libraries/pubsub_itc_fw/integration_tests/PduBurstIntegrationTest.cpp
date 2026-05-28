// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * FrameworkPduBurstIntegrationTest
 * --------------------------------
 *
 * Burst integration test for the framework PDU path (Strategy A:
 * PduProtocolHandler / PduFramer / PduParser).
 *
 * Motivation
 *   When the sample applications (gateway / sequencer / matching_engine) were
 *   exercised with a fix8 client sending many NewOrderSingles in quick
 *   succession, ExecutionReports arriving back at the gateway were observed
 *   to be corrupted: ClOrdID fields shifted by one byte, the same ClOrdID
 *   appearing twice, occasional decode failures. The hex dumps logged by the
 *   gateway's PduParser showed payload bytes with stray zero bytes interspersed
 *   between fields, indicating that the corruption happens between bytes
 *   arriving on the wire and the application thread reading them.
 *
 *   The reproducer covers the same path end-to-end inside a single process so
 *   that the bug can be iterated on quickly with deterministic input.
 *
 * Architecture
 *   Two reactors are created in this process:
 *
 *     - Sender reactor   (sequencer-analogue)
 *         A SenderThread connects outbound to the receiver's listener and,
 *         once the connection is established, sends N ExecutionReport PDUs in
 *         a tight loop via send_pdu(). Each ER carries a distinguishable
 *         cl_ord_id of the form "ord<i>" where i is the 1-based sequence
 *         number, and a seq_no in the PduHeader matching i. Before sending,
 *         the SenderThread records the encoded payload bytes for each PDU so
 *         the test can later compare them byte-for-byte against what arrived
 *         at the receiver.
 *
 *     - Receiver reactor (gateway-analogue)
 *         A ReceiverThread registers an inbound framework-PDU listener. On
 *         each on_framework_pdu_message() it copies the payload bytes into a
 *         capture vector, records the seq_no, decodes the ER, captures the
 *         cl_ord_id, and releases the inbound slab chunk.
 *
 *   Both reactors are registered with the fixture's reactor-liveness watcher
 *   so a death on either side fails the test fast with a useful message.
 *
 * Tests
 *   1. ExecutionReportBurstSurvivesEndToEnd -- baseline. Sender → receiver,
 *      framework-PDU burst only, no concurrent raw traffic. This test is now
 *      a regression check: if it ever starts failing, the framework-PDU path
 *      itself is broken in isolation.
 *
 *   2. ExecutionReportBurstUnderConcurrentRawPressure -- adds a second
 *      inbound listener (raw bytes) on the receiver reactor and a test-owned
 *      raw client thread pumping FIX-like bytes throughout the framework-PDU
 *      burst. This mirrors the real gateway, which receives both raw FIX
 *      bytes from clients and framework-PDU ExecutionReports from the
 *      sequencer concurrently on the same application thread. The
 *      framework-PDU assertions are identical to test 1; a non-zero count of
 *      raw bytes consumed is also asserted to confirm the raw stream was
 *      actually flowing while the burst was in progress.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <fix_equity_orders.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Test constants
// ============================================================

static constexpr int burst_size = 100;
static constexpr int64_t raw_buffer_capacity = 65536;
static const std::string receiver_service = "receiver";
static constexpr uint16_t any_os_assigned_port = 0;

// ============================================================
// Reactor / thread configuration helpers
// ============================================================

static ReactorConfiguration make_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    return cfg;
}

// ============================================================
// SenderThread
// ============================================================
//
// Plays the part of the sequencer in the real deployment: connects outbound to
// the named "receiver" service, then on connection_established sends a burst
// of ExecutionReport PDUs and records each encoded payload for later byte-for-
// byte comparison.

class SenderThread : public ApplicationThread {
  public:
    SenderThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int count)
        : ApplicationThread(token, logger, reactor, "SenderThread", ThreadID{1}, make_queue_config(), make_allocator_config("SenderPool"),
                            ApplicationThreadConfiguration{})
        , count_(count) {
        sent_payloads_.reserve(static_cast<size_t>(count));
        cl_ord_id_storage_.reserve(static_cast<size_t>(count));
    }

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_failed{false};
    std::atomic<bool> burst_sent{false};

    // Encoded payload (no PduHeader) of each PDU the sender produced, captured
    // before send_pdu copies the bytes into the slab chunk. Indexed 0..count-1.
    std::vector<std::vector<uint8_t>> sent_payloads_;

  protected:
    void on_app_ready_event() override {
        connect_to_service(receiver_service);
    }

    void on_connection_established(ConnectionID id) override {
        conn_id_ = id;
        connection_established.store(true, std::memory_order_release);
        send_burst();
    }

    void on_connection_failed(const std::string&) override {
        connection_failed.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(const std::string&) override {}

  private:
    void send_burst() {
        for (int i = 1; i <= count_; ++i) {
            // ExecutionReport has many std::string_view fields. The backing
            // storage must outlive the send_pdu() call. send_pdu() copies the
            // encoded bytes into a slab chunk synchronously, so the storage
            // only has to survive one iteration; pushing into a vector that
            // outlives the loop is the simplest way to guarantee that.
            cl_ord_id_storage_.push_back("ord" + std::to_string(i));
            const std::string& cl_ord_id = cl_ord_id_storage_.back();

            pubsub_itc_fw_app::ExecutionReport er{};
            er.order_id = order_id_storage_;
            er.exec_id = exec_id_storage_;
            er.exec_type = pubsub_itc_fw_app::ExecType::Trade;
            er.ord_status = pubsub_itc_fw_app::OrdStatus::Filled;
            er.symbol = symbol_storage_;
            er.side = pubsub_itc_fw_app::Side::Buy;
            er.leaves_qty = zero_storage_;
            er.cum_qty = qty_storage_;
            er.avg_px = price_storage_;
            er.transact_time = 0;
            er.has_cl_ord_id = true;
            er.cl_ord_id = cl_ord_id;
            er.has_order_qty = true;
            er.order_qty = qty_storage_;
            er.has_last_qty = true;
            er.last_qty = qty_storage_;
            er.has_last_px = true;
            er.last_px = price_storage_;

            // Capture the encoded payload bytes (without the PduHeader) into
            // sent_payloads_ before send_pdu() runs. This is what the receiver
            // will be compared against.
            size_t bytes_written = 0;
            size_t bytes_needed = 0;
            const bool measure_ok = pubsub_itc_fw_app::encode(er, nullptr, 0, bytes_written, bytes_needed);
            if (bytes_needed == 0) {
                ADD_FAILURE() << "encode measuring pass gave zero bytes_needed for ER " << i << " (encode returned " << measure_ok << ")";
                return;
            }
            std::vector<uint8_t> encoded(bytes_needed);
            if (!pubsub_itc_fw_app::encode(er, encoded.data(), encoded.size(), bytes_written, bytes_needed)) {
                ADD_FAILURE() << "encode writing pass failed for ER " << i;
                return;
            }
            encoded.resize(bytes_written);
            sent_payloads_.push_back(std::move(encoded));

            const int16_t pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::ExecutionReport);
            const int64_t seq_no = static_cast<int64_t>(i);
            send_pdu(conn_id_, pdu_id, seq_no, er);
        }
        burst_sent.store(true, std::memory_order_release);
    }

    int count_;
    ConnectionID conn_id_{};

    // Long-lived backing storage for std::string_view fields that do not vary
    // across PDUs. The cl_ord_id varies per PDU and is stored separately above.
    const std::string order_id_storage_ = "ME-ORD-1";
    const std::string exec_id_storage_ = "ME-EXEC-1";
    const std::string symbol_storage_ = "BHP";
    const std::string zero_storage_ = "0";
    const std::string qty_storage_ = "100.0";
    const std::string price_storage_ = "42.0";
    std::vector<std::string> cl_ord_id_storage_;
};

// ============================================================
// ReceiverThread
// ============================================================
//
// Plays the part of the gateway in the real deployment: receives ER PDUs on
// an inbound framework-PDU listener, captures the raw payload bytes and the
// decoded cl_ord_id of each, then releases the inbound slab chunk.

class ReceiverThread : public ApplicationThread {
  public:
    ReceiverThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "ReceiverThread", ThreadID{2}, make_queue_config(), make_allocator_config("ReceiverPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> received_count{0};
    std::atomic<int> raw_bytes_received{0};

    struct CapturedPdu {
        int64_t seq_no{0};
        std::vector<uint8_t> payload;
        std::string decoded_cl_ord_id;
        bool decode_ok{false};
    };

    // Filled in callback order. Reading this vector after burst completion is
    // safe because the receiver thread has stopped touching it by then.
    std::vector<CapturedPdu> captured_;

  protected:
    void on_framework_pdu_message(const EventMessage& message) override {
        CapturedPdu cap{};
        cap.seq_no = message.seq_no();

        const auto* payload_ptr = message.payload();
        const auto payload_size = static_cast<size_t>(message.payload_size());

        cap.payload.assign(payload_ptr, payload_ptr + payload_size);

        // Decode to extract cl_ord_id for additional verification.
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::ExecutionReportView view{};
        cap.decode_ok = pubsub_itc_fw_app::decode(view, payload_ptr, payload_size, bytes_consumed, arena, arena_bytes_needed);
        if (cap.decode_ok && view.has_cl_ord_id) {
            cap.decoded_cl_ord_id.assign(view.cl_ord_id.data(), view.cl_ord_id.size());
        }

        captured_.push_back(std::move(cap));
        received_count.fetch_add(1, std::memory_order_release);

        release_pdu_payload(message);
    }

    void on_connection_established(ConnectionID) override {}
    void on_connection_lost(ConnectionID, const std::string&) override {}

    /*
     * Used by test 2 only. Raw bytes that arrive on the receiver's second
     * inbound listener get drained immediately so the MirroredBuffer never
     * fills and triggers the connection-teardown backpressure policy.
     *
     * Correct handling of the cumulative-bytes contract:
     *   Each event reports payload_size = current bytes_available, and
     *   tail_position = the buffer's tail index at enqueue time. The buffer's
     *   absolute head at enqueue time is therefore tail_position + payload_size
     *   (modulo the buffer capacity, but the test's burst is far smaller than
     *   one wrap, so we treat positions as monotonically increasing).
     *
     *   The receiver tracks the absolute head it has ever seen and the total
     *   bytes it has ever asked to be committed. On each event, the bytes to
     *   commit equal (absolute_head_now - total_committed_so_far). The
     *   reactor may have multiple CommitRawBytes commands in flight at once;
     *   that is fine, because their sum can never exceed the bytes actually
     *   produced.
     *
     * Test 1 never sets up a raw listener, so this callback is never fired
     * in that test.
     */
    void on_raw_socket_message(const EventMessage& message) override {
        const int64_t event_tail = message.tail_position();
        const int64_t event_bytes = static_cast<int64_t>(message.payload_size());
        const int64_t absolute_head_now = event_tail + event_bytes;

        if (absolute_head_now > absolute_head_seen_) {
            absolute_head_seen_ = absolute_head_now;
        }

        const int64_t to_commit = absolute_head_seen_ - total_bytes_committed_;
        if (to_commit > 0) {
            raw_bytes_received.fetch_add(static_cast<int>(to_commit), std::memory_order_acq_rel);
            commit_raw_bytes(message.connection_id(), to_commit);
            total_bytes_committed_ = absolute_head_seen_;
        }
    }

    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(const std::string&) override {}

  private:
    // Highest absolute head position observed across raw-socket events.
    // "Absolute" here means tail_position + payload_size, treated as a
    // monotonically increasing position. The test bursts are small enough
    // that wrap-around cannot occur.
    int64_t absolute_head_seen_{0};
    // Total bytes the receiver has asked the reactor to commit across all
    // raw-socket events so far.
    int64_t total_bytes_committed_{0};
};

// ============================================================
// Fixture
// ============================================================
//
// Mirrors the reactor-liveness pattern of RawBytesProtocolHandlerIntegrationTest.
// Two reactors are watched simultaneously; wait_for() reports if either dies.

class FrameworkPduBurstIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }

    void TearDown() override {
        sender_reactor_ = nullptr;
        receiver_reactor_ = nullptr;
        logger_.reset();
    }

    void set_watched_reactors(Reactor& sender, Reactor& receiver) {
        sender_reactor_ = &sender;
        receiver_reactor_ = &receiver;
    }

    bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        reactor_died_ = false;
        died_reactor_name_.clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (sender_reactor_ != nullptr && sender_reactor_->is_finished()) {
                reactor_died_ = true;
                died_reactor_name_ = "sender";
                return false;
            }
            if (receiver_reactor_ != nullptr && receiver_reactor_->is_finished()) {
                reactor_died_ = true;
                died_reactor_name_ = "receiver";
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    std::string last_wait_failure_description() const {
        if (reactor_died_) {
            const Reactor* reactor = (died_reactor_name_ == "sender") ? sender_reactor_ : receiver_reactor_;
            const std::string reason = (reactor != nullptr) ? reactor->get_shutdown_reason() : "(no reactor)";
            return died_reactor_name_ + " reactor terminated during wait; shutdown reason: " + reason;
        }
        return "predicate did not become true within timeout";
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& reactor_thread, const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (reactor_thread.joinable())
            reactor_thread.join();
    }

    std::unique_ptr<LoggerWithSink> logger_;
    Reactor* sender_reactor_{nullptr};
    Reactor* receiver_reactor_{nullptr};
    bool reactor_died_{false};
    std::string died_reactor_name_;
};

// ============================================================
// Test: burst of N framework-PDU ExecutionReports flows intact
// ============================================================

TEST_F(FrameworkPduBurstIntegrationTest, ExecutionReportBurstSurvivesEndToEnd) {
    // ----- Receiver -----
    ServiceRegistry receiver_registry;
    auto receiver_reactor = std::make_unique<Reactor>(make_reactor_config(), receiver_registry, logger_->logger);

    receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2});

    auto receiver_thread = ApplicationThread::create<ReceiverThread>(logger_->logger, *receiver_reactor);
    receiver_reactor->register_thread(receiver_thread);

    std::thread receiver_reactor_thread([&]() { receiver_reactor->run(); });

    // Wait for the receiver reactor to come up so we can read its assigned port
    // before any sender attempts to connect.
    ASSERT_TRUE(wait_for([&]() { return receiver_reactor->is_initialized(); })) << "Receiver reactor did not initialise within timeout";
    const uint16_t receiver_port = receiver_reactor->get_inbound_listener_port(0);
    ASSERT_NE(receiver_port, 0u) << "OS did not assign a valid listening port";

    // ----- Sender -----
    ServiceRegistry sender_registry;
    sender_registry.add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", receiver_port}, NetworkEndpointConfiguration{});

    auto sender_reactor = std::make_unique<Reactor>(make_reactor_config(), sender_registry, logger_->logger);

    auto sender_thread = ApplicationThread::create<SenderThread>(logger_->logger, *sender_reactor, burst_size);
    sender_reactor->register_thread(sender_thread);

    std::thread sender_reactor_thread([&]() { sender_reactor->run(); });

    // Watch both reactors from this point on.
    set_watched_reactors(*sender_reactor, *receiver_reactor);

    ASSERT_TRUE(wait_for([&]() { return sender_reactor->is_initialized(); }))
        << "Sender reactor did not initialise within timeout: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return sender_thread->connection_established.load(std::memory_order_acquire); }))
        << "Sender: outbound connection to receiver not established: " << last_wait_failure_description();
    EXPECT_FALSE(sender_thread->connection_failed.load(std::memory_order_acquire)) << "Sender: connect_to_service reported failure";

    EXPECT_TRUE(wait_for([&]() { return sender_thread->burst_sent.load(std::memory_order_acquire); }))
        << "Sender: burst of " << burst_size << " send_pdu calls did not complete: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return receiver_thread->received_count.load(std::memory_order_acquire) >= burst_size; }, 10000))
        << "Receiver: did not receive all " << burst_size << " PDUs (got " << receiver_thread->received_count.load(std::memory_order_acquire)
        << "): " << last_wait_failure_description();

    // Stop both reactors before reading captured_ so the receiver thread is no
    // longer mutating it.
    shutdown_and_join(*sender_reactor, sender_reactor_thread);
    shutdown_and_join(*receiver_reactor, receiver_reactor_thread);

    // ----- Assertions -----

    ASSERT_EQ(static_cast<int>(receiver_thread->captured_.size()), burst_size) << "Receiver captured PDU count does not match burst size";
    ASSERT_EQ(static_cast<int>(sender_thread->sent_payloads_.size()), burst_size) << "Sender recorded payload count does not match burst size";

    for (int i = 0; i < burst_size; ++i) {
        const auto& cap = receiver_thread->captured_[static_cast<size_t>(i)];
        const auto& sent_payload = sender_thread->sent_payloads_[static_cast<size_t>(i)];
        const std::string expected = "ord" + std::to_string(i + 1);

        EXPECT_EQ(cap.seq_no, static_cast<int64_t>(i + 1)) << "PDU at index " << i << ": seq_no mismatch (got " << cap.seq_no << ")";

        EXPECT_TRUE(cap.decode_ok) << "PDU at index " << i << " (expected cl_ord_id=" << expected << "): failed to decode";

        EXPECT_EQ(cap.decoded_cl_ord_id, expected) << "PDU at index " << i << ": decoded cl_ord_id mismatch";

        ASSERT_EQ(cap.payload.size(), sent_payload.size())
            << "PDU at index " << i << ": payload byte count mismatch (sender wrote " << sent_payload.size() << ", receiver got " << cap.payload.size() << ")";

        const bool bytes_equal = std::memcmp(cap.payload.data(), sent_payload.data(), sent_payload.size()) == 0;
        EXPECT_TRUE(bytes_equal) << "PDU at index " << i << ": payload bytes differ from what sender produced";
    }
}

// ============================================================
// Raw-stream test helpers
// ============================================================
//
// Used only by ExecutionReportBurstUnderConcurrentRawPressure. A small
// background thread connects to the receiver's raw-bytes listener and pumps
// length-prefixed FIX-like frames until told to stop. Its purpose is to keep
// the receiver's application thread busy with raw-socket events while the
// framework-PDU burst is in progress, so the test exercises the same
// concurrent-streams condition the real gateway is under.

namespace {

int connect_raw_socket(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    timeval timeout{};
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

bool send_all(int fd, const void* data, size_t size) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

} // anonymous namespace

// ============================================================
// Test: burst of N PDUs survives concurrent raw-stream pressure
// ============================================================

TEST_F(FrameworkPduBurstIntegrationTest, ExecutionReportBurstUnderConcurrentRawPressure) {
    // ----- Receiver: framework PDU listener (port A) + raw bytes listener (port B) -----
    ServiceRegistry receiver_registry;
    auto receiver_reactor = std::make_unique<Reactor>(make_reactor_config(), receiver_registry, logger_->logger);

    receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2});

    receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2},
                                                ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto receiver_thread = ApplicationThread::create<ReceiverThread>(logger_->logger, *receiver_reactor);
    receiver_reactor->register_thread(receiver_thread);

    std::thread receiver_reactor_thread([&]() { receiver_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return receiver_reactor->is_initialized(); })) << "Receiver reactor did not initialise within timeout";

    // The framework-PDU listener was registered first, so it occupies the
    // first inbound listener slot; the raw listener is the second.
    const uint16_t pdu_port = receiver_reactor->get_inbound_listener_port(0);
    const uint16_t raw_port = receiver_reactor->get_inbound_listener_port(1);
    ASSERT_NE(pdu_port, 0u) << "Receiver framework-PDU port not assigned";
    ASSERT_NE(raw_port, 0u) << "Receiver raw-bytes port not assigned";

    // ----- Raw client thread: pump bytes until told to stop -----
    std::atomic<bool> stop_raw_client{false};
    std::atomic<int> raw_bytes_sent{0};
    std::atomic<bool> raw_client_connected{false};
    std::atomic<bool> raw_client_failed{false};

    std::thread raw_client_thread([&]() {
        const int sock = connect_raw_socket(raw_port);
        if (sock == -1) {
            raw_client_failed.store(true, std::memory_order_release);
            return;
        }
        raw_client_connected.store(true, std::memory_order_release);

        // Send length-prefixed FIX-like frames in a loop. The receiver drains
        // them on every callback so the buffer never fills. Each frame is
        // small so the receiver gets called many times while the framework-
        // PDU burst is in progress.
        const std::string payload = "FIX-RAW-FILLER";
        const uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));

        while (!stop_raw_client.load(std::memory_order_acquire)) {
            if (!send_all(sock, &len_be, sizeof(len_be)))
                break;
            if (!send_all(sock, payload.data(), payload.size()))
                break;
            raw_bytes_sent.fetch_add(static_cast<int>(sizeof(len_be) + payload.size()), std::memory_order_acq_rel);
            // Small sleep so we don't dominate the CPU; the goal is sustained
            // concurrent activity, not throughput maximisation.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        ::close(sock);
    });

    ASSERT_TRUE(wait_for([&]() { return raw_client_connected.load(std::memory_order_acquire) || raw_client_failed.load(std::memory_order_acquire); }))
        << "Raw client did not connect within timeout";
    ASSERT_FALSE(raw_client_failed.load(std::memory_order_acquire)) << "Raw client failed to connect";

    // ----- Sender: outbound to receiver's framework-PDU listener -----
    ServiceRegistry sender_registry;
    sender_registry.add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", pdu_port}, NetworkEndpointConfiguration{});

    auto sender_reactor = std::make_unique<Reactor>(make_reactor_config(), sender_registry, logger_->logger);
    auto sender_thread = ApplicationThread::create<SenderThread>(logger_->logger, *sender_reactor, burst_size);
    sender_reactor->register_thread(sender_thread);

    std::thread sender_reactor_thread([&]() { sender_reactor->run(); });

    set_watched_reactors(*sender_reactor, *receiver_reactor);

    ASSERT_TRUE(wait_for([&]() { return sender_reactor->is_initialized(); }))
        << "Sender reactor did not initialise within timeout: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return sender_thread->connection_established.load(std::memory_order_acquire); }))
        << "Sender: outbound connection to receiver not established: " << last_wait_failure_description();
    EXPECT_FALSE(sender_thread->connection_failed.load(std::memory_order_acquire)) << "Sender: connect_to_service reported failure";

    EXPECT_TRUE(wait_for([&]() { return sender_thread->burst_sent.load(std::memory_order_acquire); }))
        << "Sender: burst of " << burst_size << " send_pdu calls did not complete: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return receiver_thread->received_count.load(std::memory_order_acquire) >= burst_size; }, 10000))
        << "Receiver: did not receive all " << burst_size << " PDUs (got " << receiver_thread->received_count.load(std::memory_order_acquire)
        << "): " << last_wait_failure_description();

    // ----- Stop raw client, then shut down both reactors -----
    stop_raw_client.store(true, std::memory_order_release);
    if (raw_client_thread.joinable())
        raw_client_thread.join();

    shutdown_and_join(*sender_reactor, sender_reactor_thread);
    shutdown_and_join(*receiver_reactor, receiver_reactor_thread);

    // ----- Assertions: framework-PDU correctness, same as test 1 -----

    ASSERT_EQ(static_cast<int>(receiver_thread->captured_.size()), burst_size) << "Receiver captured PDU count does not match burst size";
    ASSERT_EQ(static_cast<int>(sender_thread->sent_payloads_.size()), burst_size) << "Sender recorded payload count does not match burst size";

    for (int i = 0; i < burst_size; ++i) {
        const auto& cap = receiver_thread->captured_[static_cast<size_t>(i)];
        const auto& sent_payload = sender_thread->sent_payloads_[static_cast<size_t>(i)];
        const std::string expected = "ord" + std::to_string(i + 1);

        EXPECT_EQ(cap.seq_no, static_cast<int64_t>(i + 1)) << "PDU at index " << i << ": seq_no mismatch (got " << cap.seq_no << ")";

        EXPECT_TRUE(cap.decode_ok) << "PDU at index " << i << " (expected cl_ord_id=" << expected << "): failed to decode";

        EXPECT_EQ(cap.decoded_cl_ord_id, expected) << "PDU at index " << i << ": decoded cl_ord_id mismatch";

        ASSERT_EQ(cap.payload.size(), sent_payload.size())
            << "PDU at index " << i << ": payload byte count mismatch (sender wrote " << sent_payload.size() << ", receiver got " << cap.payload.size() << ")";

        const bool bytes_equal = std::memcmp(cap.payload.data(), sent_payload.data(), sent_payload.size()) == 0;
        EXPECT_TRUE(bytes_equal) << "PDU at index " << i << ": payload bytes differ from what sender produced";
    }

    // ----- Raw stream sanity check: confirm it was actually flowing -----

    EXPECT_GT(receiver_thread->raw_bytes_received.load(std::memory_order_acquire), 0)
        << "Receiver never saw any raw bytes -- concurrent pressure condition not exercised";
    EXPECT_GT(raw_bytes_sent.load(std::memory_order_acquire), 0) << "Raw client never sent any bytes -- concurrent pressure condition not exercised";
}

} // namespace pubsub_itc_fw::tests
