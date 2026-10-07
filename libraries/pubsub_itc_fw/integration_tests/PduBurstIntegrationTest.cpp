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
 *   1. ExecutionReportBurstSurvivesEndToEnd -- baseline. Sender -> receiver,
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
#include <endian.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <fix_orders.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;
namespace pubsub_itc_fw::tests {

// Test constants

static constexpr int burst_size = 100;
static constexpr int64_t raw_buffer_capacity = 65536;
static const std::string receiver_service = "receiver";
static constexpr uint16_t any_os_assigned_port = 0;

// Reactor / thread configuration helpers

namespace {

ReactorConfiguration make_reactor_config(std::chrono::microseconds spin_before_block = std::chrono::microseconds{0},
                                         std::chrono::milliseconds inactivity_check_interval = std::chrono::milliseconds{100}, bool with_metrics = false) {
    ReactorConfiguration cfg{};

    // The reactor's own housekeeping timer is an epoll event, and any epoll event takes a
    // polling reactor out of its loop, which empties the command queue on the way past. Left at
    // a tenth of a second it carries sends through by itself, and a test meant to show that the
    // polling loop finds them would pass with that loop removed. The polling tests below push it
    // beyond the length of the test so that the polling loop is the only way a send gets out.
    cfg.inactivity_check_interval_ = inactivity_check_interval;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);

    // Zero, the default, means the reactor sleeps in epoll_wait whenever it has nothing to do,
    // and an application thread wanting something sent has to wake it. Anything above zero puts
    // the reactor in its polling loop instead, where it finds the request itself and the sending
    // thread writes no wakeup at all. The two are different code paths through every send in this
    // file, so the tests below run the same burst down each of them.
    cfg.spin_before_block = spin_before_block;

    // Off unless a test asks, because it starts a listener per reactor. Port 0 lets the
    // operating system choose one, so parallel test binaries do not collide; the host has to be
    // given as well, since the default is empty and a metrics listener that cannot bind stops
    // the reactor.
    cfg.metrics_configuration.enabled = with_metrics;
    cfg.metrics_configuration.listen_endpoint = NetworkEndpointConfiguration{"127.0.0.1", 0};
    return cfg;
}

/** @brief How many observations a histogram family holds, read from a real scrape. */
int64_t observation_count(const std::string& exposition, const std::string& family) {
    // Every child of the family is added up. A family can have several children, told apart by
    // scope -- the reactor's send path has one for sends on the order path and one for the rest --
    // and the order a scrape lists them in says nothing about which of them a test's traffic went
    // to, so reading only the first would depend on that order.
    std::istringstream stream(exposition);
    std::string line;
    bool family_found = false;
    int64_t total = 0;
    while (std::getline(stream, line)) {
        if (line.rfind(family + "_count", 0) != 0) {
            continue;
        }
        const auto value_at = line.rfind(' ');
        if (value_at != std::string::npos) {
            family_found = true;
            total += static_cast<int64_t>(std::stod(line.substr(value_at + 1)));
        }
    }
    return family_found ? total : -1;
}

// How long a polling reactor keeps looking before it would give up and sleep. Long enough that
// it never does so during a test, which is what makes the polling path the one under test rather
// than a mixture of the two.
constexpr std::chrono::microseconds keep_polling_throughout{30000000};

// A housekeeping interval longer than any test here, so that a polling reactor gets no epoll
// event it did not earn and cannot be carried by one.
constexpr std::chrono::milliseconds no_housekeeping_during_the_test{60000};

} // un-named namespace

// SenderThread
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

    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(pubsub_itc_fw::TimerID) override {}

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

            constexpr auto pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
            const auto seq_no = static_cast<int64_t>(i);
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

// ReceiverThread
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
    void on_connection_lost(const ConnectionID&, const std::string&) override {}

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
        const auto event_bytes = static_cast<int64_t>(message.payload_size());
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

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
    void on_timer_event([[maybe_unused]] pubsub_itc_fw::TimerID id) override {}

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

// Fixture
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
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    [[nodiscard]] std::string last_wait_failure_description() const {
        if (reactor_died_) {
            const Reactor* reactor = (died_reactor_name_ == "sender") ? sender_reactor_ : receiver_reactor_;
            const std::string reason = (reactor != nullptr) ? reactor->get_shutdown_reason() : "(no reactor)";
            return died_reactor_name_ + " reactor terminated during wait; shutdown reason: " + reason;
        }
        return "predicate did not become true within timeout";
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& reactor_thread, const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (reactor_thread.joinable()) {
            reactor_thread.join();
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    Reactor* sender_reactor_{nullptr};
    Reactor* receiver_reactor_{nullptr};
    bool reactor_died_{false};
    std::string died_reactor_name_;
};

// Test: burst of N framework-PDU ExecutionReports flows intact

TEST_F(FrameworkPduBurstIntegrationTest, ExecutionReportBurstSurvivesEndToEnd) {
    // ----- Receiver -----
    const ServiceRegistry receiver_registry;
    auto receiver_reactor = std::make_unique<Reactor>(make_reactor_config(), receiver_registry, logger_->logger);

    receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2});

    auto receiver_thread = ApplicationThread::create<ReceiverThread>(logger_->logger, *receiver_reactor);
    receiver_reactor->register_thread(receiver_thread);

    std::thread receiver_reactor_thread([&]() { receiver_reactor->run(); });

    // Wait for the receiver reactor to come up so we can read its assigned port
    // before any sender attempts to connect.
    ASSERT_TRUE(wait_for([&]() { return receiver_reactor->is_initialized(); })) << "Receiver reactor did not initialise within timeout";
    const uint16_t receiver_port = receiver_reactor->get_inbound_listener_port(0);
    ASSERT_NE(receiver_port, 0U) << "OS did not assign a valid listening port";

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

// Raw-stream test helpers
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
    if (fd == -1) {
        return -1;
    }
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
        if (n <= 0) {
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

} // un-named namespace

// Test: burst of N PDUs survives concurrent raw-stream pressure

TEST_F(FrameworkPduBurstIntegrationTest, ExecutionReportBurstUnderConcurrentRawPressure) {
    // ----- Receiver: framework PDU listener (port A) + raw bytes listener (port B) -----
    const ServiceRegistry receiver_registry;
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
    ASSERT_NE(pdu_port, 0U) << "Receiver framework-PDU port not assigned";
    ASSERT_NE(raw_port, 0U) << "Receiver raw-bytes port not assigned";

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
            if (!send_all(sock, &len_be, sizeof(len_be))) {
                break;
            }
            if (!send_all(sock, payload.data(), payload.size())) {
                break;
            }
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
    if (raw_client_thread.joinable()) {
        raw_client_thread.join();
    }

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

/*
 * Sending through a polling reactor, and doing it for long enough to trust it
 * -------------------------------------------------------------------------
 *
 * An application thread that calls send_pdu does not touch the socket. It puts a command on its
 * reactor's queue, and the reactor writes the bytes. How the reactor comes to look at that queue
 * depends on what it is doing:
 *
 *   Sleeping in epoll_wait -- the sending thread writes to the reactor's wakeup descriptor,
 *   which is a system call on the sender and another on the reactor to drain it again.
 *
 *   Polling -- the reactor is already going round a loop looking at the queue, so the sending
 *   thread writes nothing.
 *
 * Every test above this point runs the first route, because that is the default. These two run
 * the second. They are integration tests rather than unit tests because the thing worth checking
 * cannot be seen from inside one process boundary: the bytes have to leave an application thread,
 * cross to a reactor, go out over a real socket, be read by a second reactor and arrive at a
 * second application thread, all with no test scaffolding standing in for any of it.
 *
 * The soak is the second of the two. A hand-off that is wrong once in a great many attempts
 * looks exactly like a hand-off that is right when you try it a hundred times, and a single
 * burst of a hundred PDUs is a hundred attempts. The soak sends a hundred thousand, in bursts
 * separated by pauses, because the pauses are what make the reactor's queue go empty and fill
 * again -- and a hand-off between two threads is at its most delicate on the edges, when the
 * queue has just become empty or has just stopped being so.
 */

namespace {

// Enough sends that a fault which shows up rarely has many chances to show up, while keeping the
// test inside the time an ordinary build is willing to spend.
constexpr int soak_burst_size = 500;
constexpr int soak_burst_count = 200;
constexpr int soak_total = soak_burst_size * soak_burst_count;

} // un-named namespace

/**
 * @brief Sends bursts of PDUs with pauses between them, for a long time.
 *
 * The pauses are the reason this exists rather than one enormous burst. Inside a burst the
 * reactor's command queue always has something on it, which is the easy case. It is when the
 * queue has just gone empty, and then something lands on it again, that the sending thread and
 * the reactor have to agree about whether the reactor is still looking -- and getting that wrong
 * shows up as a message that is never sent. A timer between bursts puts the venue through that
 * changeover thousands of times.
 *
 * Nothing is stored per message. The only thing worth checking over this many sends is that
 * every one of them arrived, exactly once and in order, and the sequence number says that in
 * constant memory.
 */
class SoakSenderThread : public ApplicationThread {
  public:
    SoakSenderThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "SoakSenderThread", ThreadID{1}, make_queue_config(), make_allocator_config("SoakSenderPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<int> sent_count{0};
    std::atomic<bool> all_bursts_sent{false};

  protected:
    void on_app_ready_event() override {
        connect_to_service(receiver_service);
    }

    void on_connection_established(ConnectionID id) override {
        conn_id_ = id;
        connection_established.store(true, std::memory_order_release);

        // One millisecond between bursts. Long enough that the reactor finishes the burst and
        // finds nothing left, which is the state the next burst has to wake it out of.
        burst_timer_ = schedule_timer(std::chrono::microseconds(1000), TimerType(TimerType::Recurring));
    }

    void on_connection_failed(const std::string&) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}

    void on_timer_event(pubsub_itc_fw::TimerID id) override {
        if (id != burst_timer_ || bursts_done_ >= soak_burst_count) {
            return;
        }
        send_one_burst();
        ++bursts_done_;
        if (bursts_done_ >= soak_burst_count) {
            cancel_timer(burst_timer_);
            all_bursts_sent.store(true, std::memory_order_release);
        }
    }

  private:
    void send_one_burst() {
        for (int i = 0; i < soak_burst_size; ++i) {
            ++next_seq_no_;
            cl_ord_id_ = "ord" + std::to_string(next_seq_no_);

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
            er.cl_ord_id = cl_ord_id_;

            constexpr auto pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
            send_pdu(conn_id_, pdu_id, next_seq_no_, er);
            sent_count.fetch_add(1, std::memory_order_release);
        }
    }

    ConnectionID conn_id_{};
    TimerID burst_timer_{};
    int bursts_done_{0};
    int64_t next_seq_no_{0};
    std::string cl_ord_id_;

    const std::string order_id_storage_ = "ME-ORD-1";
    const std::string exec_id_storage_ = "ME-EXEC-1";
    const std::string symbol_storage_ = "BHP";
    const std::string zero_storage_ = "0";
    const std::string qty_storage_ = "100.0";
    const std::string price_storage_ = "42.0";
};

/** @brief Counts arrivals and checks the sequence, without keeping any of them. */
class SoakReceiverThread : public ApplicationThread {
  public:
    SoakReceiverThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "SoakReceiverThread", ThreadID{2}, make_queue_config(), make_allocator_config("SoakReceiverPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> received_count{0};

    /** @brief The first sequence number that did not follow the one before it, or zero. */
    std::atomic<int64_t> first_break_in_sequence{0};

  protected:
    void on_framework_pdu_message(const EventMessage& message) override {
        const int64_t seq_no = message.seq_no();
        if (seq_no != expected_seq_no_ && first_break_in_sequence.load(std::memory_order_acquire) == 0) {
            first_break_in_sequence.store(seq_no, std::memory_order_release);
        }
        expected_seq_no_ = seq_no + 1;
        received_count.fetch_add(1, std::memory_order_release);
        release_pdu_payload(message);
    }

    void on_connection_established(ConnectionID) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(pubsub_itc_fw::TimerID) override {}

  private:
    int64_t expected_seq_no_{1};
};

TEST_F(FrameworkPduBurstIntegrationTest, ExecutionReportBurstSurvivesAPollingReactor) {
    // ----- Receiver -----
    const ServiceRegistry receiver_registry;
    auto receiver_reactor =
        std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test, true), receiver_registry, logger_->logger);

    receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2});

    auto receiver_thread = ApplicationThread::create<ReceiverThread>(logger_->logger, *receiver_reactor);
    receiver_reactor->register_thread(receiver_thread);

    std::thread receiver_reactor_thread([&]() { receiver_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return receiver_reactor->is_initialized(); })) << "Receiver reactor did not initialise within timeout";
    const uint16_t receiver_port = receiver_reactor->get_inbound_listener_port(0);
    ASSERT_NE(receiver_port, 0U) << "OS did not assign a valid listening port";

    // ----- Sender -----
    ServiceRegistry sender_registry;
    sender_registry.add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", receiver_port}, NetworkEndpointConfiguration{});

    auto sender_reactor =
        std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test, true), sender_registry, logger_->logger);

    auto sender_thread = ApplicationThread::create<SenderThread>(logger_->logger, *sender_reactor, burst_size);
    sender_reactor->register_thread(sender_thread);

    std::thread sender_reactor_thread([&]() { sender_reactor->run(); });

    set_watched_reactors(*sender_reactor, *receiver_reactor);

    ASSERT_TRUE(wait_for([&]() { return sender_reactor->is_initialized(); }))
        << "Sender reactor did not initialise within timeout: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return sender_thread->connection_established.load(std::memory_order_acquire); }))
        << "Sender: outbound connection to receiver not established: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return sender_thread->burst_sent.load(std::memory_order_acquire); }))
        << "Sender: burst of " << burst_size << " send_pdu calls did not complete: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return receiver_thread->received_count.load(std::memory_order_acquire) >= burst_size; }, 10000))
        << "Receiver: did not receive all " << burst_size << " PDUs through a polling reactor (got "
        << receiver_thread->received_count.load(std::memory_order_acquire) << "). A send that a polling reactor never picked up looks "
        << "exactly like this: " << last_wait_failure_description();

    // Read before the reactors are shut down: the handles point into each reactor's own
    // registry, which does not outlive it.
    const std::string sender_metrics = sender_reactor->metrics().exposition_text();
    const std::string receiver_metrics = receiver_reactor->metrics().exposition_text();

    shutdown_and_join(*sender_reactor, sender_reactor_thread);
    shutdown_and_join(*receiver_reactor, receiver_reactor_thread);

    // ----- The reactor's own two halves of the journey -----
    //
    // Both are new instruments and both are only exercised by a real socket, so this is where
    // they get to prove they record at all. The sending reactor turns a command into bytes; the
    // receiving one turns readable bytes into a queued message. A burst of PDUs went from one to
    // the other, so both must have observations, and a zero count means the timing was put
    // somewhere the code does not go.

    EXPECT_GT(observation_count(sender_metrics, "reactor_send_path_nanoseconds"), 0)
        << "the sending reactor wrote " << burst_size << " PDUs and recorded no send path at all";
    EXPECT_GT(observation_count(receiver_metrics, "reactor_receive_path_nanoseconds"), 0)
        << "the receiving reactor read " << burst_size << " PDUs and recorded no receive path at all";

    // ----- Assertions -----
    // The same byte-for-byte comparison the sleeping-reactor test makes. Arriving is not enough:
    // the point of checking the bytes is that a send picked up halfway through being written,
    // or written twice, would still arrive.

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

TEST_F(FrameworkPduBurstIntegrationTest, PollingReactorLosesNothingOverAHundredThousandSends) {
    // ----- Receiver -----
    const ServiceRegistry receiver_registry;
    auto receiver_reactor =
        std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test), receiver_registry, logger_->logger);

    receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2});

    auto receiver_thread = ApplicationThread::create<SoakReceiverThread>(logger_->logger, *receiver_reactor);
    receiver_reactor->register_thread(receiver_thread);

    std::thread receiver_reactor_thread([&]() { receiver_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return receiver_reactor->is_initialized(); })) << "Receiver reactor did not initialise within timeout";
    const uint16_t receiver_port = receiver_reactor->get_inbound_listener_port(0);
    ASSERT_NE(receiver_port, 0U) << "OS did not assign a valid listening port";

    // ----- Sender -----
    ServiceRegistry sender_registry;
    sender_registry.add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", receiver_port}, NetworkEndpointConfiguration{});

    auto sender_reactor =
        std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test), sender_registry, logger_->logger);

    auto sender_thread = ApplicationThread::create<SoakSenderThread>(logger_->logger, *sender_reactor);
    sender_reactor->register_thread(sender_thread);

    std::thread sender_reactor_thread([&]() { sender_reactor->run(); });

    set_watched_reactors(*sender_reactor, *receiver_reactor);

    ASSERT_TRUE(wait_for([&]() { return sender_reactor->is_initialized(); }))
        << "Sender reactor did not initialise within timeout: " << last_wait_failure_description();
    EXPECT_TRUE(wait_for([&]() { return sender_thread->connection_established.load(std::memory_order_acquire); }))
        << "Sender: outbound connection to receiver not established: " << last_wait_failure_description();

    // 200 bursts a millisecond apart is a little under a second of sending, but the machine this
    // runs on may be loaded and the receiver has to keep up as well, so the wait is generous.
    // A timeout here is a genuine failure, not impatience: it means sends stopped happening.
    EXPECT_TRUE(wait_for([&]() { return sender_thread->all_bursts_sent.load(std::memory_order_acquire); }, 60000))
        << "Sender: only " << sender_thread->sent_count.load(std::memory_order_acquire) << " of " << soak_total
        << " sends were made. The burst timer stopped firing, which means a command asking for it never reached the reactor: "
        << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return receiver_thread->received_count.load(std::memory_order_acquire) >= soak_total; }, 60000))
        << "Receiver: " << receiver_thread->received_count.load(std::memory_order_acquire) << " of " << soak_total
        << " PDUs arrived. Every send that a polling reactor failed to pick up is one of the missing: " << last_wait_failure_description();

    shutdown_and_join(*sender_reactor, sender_reactor_thread);
    shutdown_and_join(*receiver_reactor, receiver_reactor_thread);

    // ----- Assertions -----

    EXPECT_EQ(sender_thread->sent_count.load(std::memory_order_acquire), soak_total);
    EXPECT_EQ(receiver_thread->received_count.load(std::memory_order_acquire), soak_total) << "arrivals do not match sends";

    // Counting alone would not notice one PDU lost and another delivered twice. The sequence
    // number would.
    EXPECT_EQ(receiver_thread->first_break_in_sequence.load(std::memory_order_acquire), 0)
        << "sequence number " << receiver_thread->first_break_in_sequence.load(std::memory_order_acquire)
        << " did not follow the one before it, so a PDU was lost, duplicated or overtaken";
}

/*
 * Fairness between the things a reactor has to serve at once
 * ----------------------------------------------------------
 *
 * One reactor thread serves four different sources of work:
 *
 *   bytes arriving on sockets, which may be a whole message, several messages, or part of one;
 *   messages passed between threads in this process;
 *   timers;
 *   and the queue of commands its application threads use to ask for sends.
 *
 * None of them may be allowed to crowd out the others. The risk is specific and it is easy to
 * create by accident: whichever source the reactor looks at first, if it keeps looking at that
 * one until there is nothing left, then a source that is continuously busy leaves the others
 * waiting for as long as it stays busy.
 *
 * This test runs them at once and watches a timer that has nothing to do with any of them. A
 * heartbeat every ten milliseconds should fire about a hundred times in a second no matter what
 * else is going on. If the reactor is being held by one source, the heartbeat is the thing that
 * visibly stops -- it does not compete for anything, it merely needs the reactor to come back
 * round to it. It is measured twice on the same reactor, once idle and once under load, because
 * a timer read late does not replay the beats it missed, so a low count on its own cannot say
 * whether the reactor was busy or the timer was never running at the assumed rate.
 *
 * ONE LOAD IS DELIBERATELY KEPT LOW, and it is worth knowing why before raising it. The busy
 * thread asks the reactor for a timer in answer to a message, which is how this test puts work
 * on the command queue, but it does so for one message in a hundred. Answering every message
 * makes the heartbeat collapse from 199 beats to 13 -- and that is the cost of creating
 * thousands of timers a second, not unfairness towards the sockets. Holding the socket and
 * message loads exactly as they are and varying only that rate is what showed it. See BUG-0094,
 * which was raised on the stronger reading and dismissed once the loads were varied one at a
 * time.
 */

namespace {

constexpr auto fairness_run_time = std::chrono::milliseconds{2000};
constexpr auto heartbeat_interval = std::chrono::microseconds{10000};

// Ten milliseconds apart over two seconds is about two hundred beats. Half of that is the floor
// for calling the reactor fair: it leaves room for an ordinary loaded build machine, while a
// reactor that is actually being starved shows single figures or none at all.
constexpr int fewest_acceptable_heartbeats = 100;

} // un-named namespace

/**
 * @brief Takes work from every source at once and reports what it saw of each.
 *
 * The ITC messages are what drives the command queue: each one is answered by asking the
 * reactor for a timer, so a test thread pushing messages in as fast as it can is also pushing
 * commands in as fast as it can.
 */
class AllSourcesThread : public ApplicationThread {
  public:
    AllSourcesThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "AllSourcesThread", ThreadID{2}, make_queue_config(), make_allocator_config("AllSourcesPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> pdus_from_socket{0};
    std::atomic<int> messages_from_threads{0};
    std::atomic<int> heartbeats{0};
    std::atomic<int> timers_asked_for{0};
    std::atomic<bool> ready{false};

  protected:
    void on_app_ready_event() override {
        ready.store(true, std::memory_order_release);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        pdus_from_socket.fetch_add(1, std::memory_order_release);
        release_pdu_payload(message);
    }

    void on_itc_message(const EventMessage&) override {
        messages_from_threads.fetch_add(1, std::memory_order_release);

        // Answering with a request to the reactor is what puts this test's load on the command
        // queue. Single-shot, so each one is asked for and then goes away by itself.
        if (timers_asked_for.fetch_add(1, std::memory_order_release) % 100 == 0) {
            schedule_timer(std::chrono::microseconds(50000), TimerType(TimerType::SingleShot));
        }
    }

    void on_timer_event(pubsub_itc_fw::TimerID) override {}

    void on_connection_established(ConnectionID) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
};

/**
 * @brief Does nothing but count a heartbeat, on a thread with no other work.
 *
 * The heartbeat has to live on a thread of its own. Put on the busy thread it measures that
 * thread's backlog rather than the reactor's fairness: a timer is delivered to an application
 * thread like anything else, so a thread working through tens of thousands of messages reports
 * its beats late whatever the reactor does, and a test reading that as unfairness would be
 * blaming the wrong component. On an otherwise idle thread the only thing between the timer
 * expiring and this counter moving is the reactor coming back round to look.
 */
class HeartbeatThread : public ApplicationThread {
  public:
    HeartbeatThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "HeartbeatThread", ThreadID{3}, make_queue_config(), make_allocator_config("HeartbeatPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> heartbeats{0};
    std::atomic<bool> ready{false};

  protected:
    void on_app_ready_event() override {
        schedule_timer(heartbeat_interval, TimerType(TimerType::Recurring));
        ready.store(true, std::memory_order_release);
    }

    void on_timer_event(pubsub_itc_fw::TimerID) override {
        heartbeats.fetch_add(1, std::memory_order_release);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }

    void on_connection_established(ConnectionID) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}
};

// Disabled because it fails, and it fails because it has found something. See BUG-0094 in
// docs/bug_list.md: a reactor under sustained socket load serves a timer at about a fifteenth of
// the rate it serves the same timer when idle, and it does so whichever way the reactor waits, so
// it is older than the polling loop. The test is left here, written and working, because the day
// that is fixed this is what says so. Run it with --gtest_also_run_disabled_tests.
TEST_F(FrameworkPduBurstIntegrationTest, PollingReactorKeepsServingEverySourceWhileOneIsBusy) {
    // ----- The reactor under test -----
    const ServiceRegistry busy_registry;
    auto busy_reactor =
        std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test), busy_registry, logger_->logger);

    busy_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2});

    auto busy_thread = ApplicationThread::create<AllSourcesThread>(logger_->logger, *busy_reactor);
    busy_reactor->register_thread(busy_thread);

    auto heartbeat_thread = ApplicationThread::create<HeartbeatThread>(logger_->logger, *busy_reactor);
    busy_reactor->register_thread(heartbeat_thread);

    std::thread busy_reactor_thread([&]() { busy_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return busy_reactor->is_initialized(); })) << "Reactor under test did not initialise within timeout";
    ASSERT_TRUE(wait_for([&]() { return busy_thread->ready.load(std::memory_order_acquire); })) << "Busy thread did not reach app-ready";
    ASSERT_TRUE(wait_for([&]() { return heartbeat_thread->ready.load(std::memory_order_acquire); })) << "Heartbeat thread did not reach app-ready";
    const uint16_t busy_port = busy_reactor->get_inbound_listener_port(0);
    ASSERT_NE(busy_port, 0U) << "OS did not assign a valid listening port";

    // ----- First, how fast the heartbeat runs with nothing else going on -----
    //
    // This is the test's own control, and it is not optional. A recurring timer that is read
    // late does not replay the beats it missed: the descriptor reports how many expiries have
    // built up and the framework delivers one event for that read. So a low count can mean the
    // reactor was too busy to come back to it, or it can mean the timer never ran at the rate
    // this test assumed. Measuring the quiet rate first tells those apart, and everything below
    // is stated as a fraction of it rather than of a number worked out on paper.

    const int quiet_beats_before = heartbeat_thread->heartbeats.load(std::memory_order_acquire);
    std::this_thread::sleep_for(fairness_run_time);
    const int quiet_beats = heartbeat_thread->heartbeats.load(std::memory_order_acquire) - quiet_beats_before;

    ASSERT_GE(quiet_beats, fewest_acceptable_heartbeats)
        << "the heartbeat managed only " << quiet_beats << " beats in " << fairness_run_time.count()
        << "ms with nothing else happening at all. Nothing can be concluded about fairness from a timer that is already slow on an idle reactor";

    // ----- Now the same measurement with every source busy at once -----

    ServiceRegistry sender_registry;
    sender_registry.add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", busy_port}, NetworkEndpointConfiguration{});

    auto sender_reactor =
        std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test), sender_registry, logger_->logger);

    auto sender_thread = ApplicationThread::create<SoakSenderThread>(logger_->logger, *sender_reactor);
    sender_reactor->register_thread(sender_thread);

    std::thread sender_reactor_thread([&]() { sender_reactor->run(); });

    set_watched_reactors(*sender_reactor, *busy_reactor);
    ASSERT_TRUE(wait_for([&]() { return sender_thread->connection_established.load(std::memory_order_acquire); }))
        << "Sender: outbound connection not established: " << last_wait_failure_description();

    std::atomic<bool> keep_pushing{true};
    std::thread message_pusher([&]() {
        while (keep_pushing.load(std::memory_order_acquire)) {
            busy_reactor->route_message(ThreadID{2}, EventMessage::create_itc_message(ThreadID{2}, nullptr, 0));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    const int busy_beats_before = heartbeat_thread->heartbeats.load(std::memory_order_acquire);
    std::this_thread::sleep_for(fairness_run_time);
    const int busy_beats = heartbeat_thread->heartbeats.load(std::memory_order_acquire) - busy_beats_before;

    keep_pushing.store(false, std::memory_order_release);
    message_pusher.join();

    const int pdus = busy_thread->pdus_from_socket.load(std::memory_order_acquire);
    const int messages = busy_thread->messages_from_threads.load(std::memory_order_acquire);
    const int commands = busy_thread->timers_asked_for.load(std::memory_order_acquire);

    shutdown_and_join(*sender_reactor, sender_reactor_thread);
    shutdown_and_join(*busy_reactor, busy_reactor_thread);

    // ----- Assertions -----
    //
    // First that the test did what it set out to do. A fairness result means nothing if the
    // sources it was meant to be loading were quiet, and that failure would otherwise look
    // exactly like a pass.

    EXPECT_GT(pdus, 0) << "no PDUs arrived over the socket, so the socket source was never busy and this test proved nothing";
    EXPECT_GT(messages, 0) << "no messages arrived from other threads, so that source was never busy and this test proved nothing";
    EXPECT_GT(commands, 0) << "no commands were asked for, so the command queue was never busy and this test proved nothing";

    // And now the fairness itself, measured against the rate this same timer managed a moment
    // ago on the same reactor. A reactor that keeps serving every source keeps much of that
    // rate. One that has been captured by a single source loses nearly all of it.

    const int floor_under_load = quiet_beats / 4;
    EXPECT_GE(busy_beats, floor_under_load) << "the heartbeat managed " << quiet_beats << " beats while the reactor was idle but only " << busy_beats
                                            << " while it was also serving " << pdus << " socket message(s), " << messages
                                            << " message(s) from other threads and " << commands
                                            << " command(s). One of those sources is being served at the expense of the others";
}

/*
 * Fairness between several clients on one reactor
 * -----------------------------------------------
 *
 * The test above asks whether one reactor divides its attention fairly between DIFFERENT KINDS
 * of work. This one asks a narrower and more damaging question: does it divide its attention
 * fairly between SEVERAL CLIENTS DOING THE SAME THING?
 *
 * That is the arrangement a gateway is actually in. Several members hold connections to one
 * gateway and all of them send orders. If the reactor serves whichever descriptor the kernel
 * names first, and serves it until there is nothing left to read, then a member sending steadily
 * can be served ahead of another member every time round, and the second member's orders wait.
 * Nothing in the venue would report this. Both members are connected, nothing is dropped, no
 * error is logged, and the only evidence is that one of them is consistently slower than the
 * other for no reason either of them can see.
 *
 * Five clients, all sending as hard as they can for the same period, over one reactor. Fair
 * service means they get served about equally. The test states that as a ratio between the
 * best-served and the worst-served connection rather than as an absolute figure, because what
 * matters is the difference between them and not how fast the machine happens to be.
 */

namespace {

constexpr int fairness_client_count = 5;
constexpr size_t client_frame_payload_bytes = 200;

// The same amount for every client, and enough of it that all five are still contending for the
// reactor's attention for most of the run. That second part matters: with a small amount each,
// the clients that are served first finish and stop competing, and the ones behind them then get
// the reactor to themselves -- so everyone finishes and nothing is learned. Unfairness is only
// visible while there is something to be unfair about.
constexpr int64_t bytes_each_client_sends = 200 * 1024 * 1024;

// Generous against that: this deadline is not measuring speed, it is there so that a connection
// the reactor has effectively stopped serving produces a failure rather than a hung test.
constexpr auto client_fairness_deadline = std::chrono::milliseconds{20000};

// Five clients sending equally hard should be served equally. Half is a generous floor -- it
// allows for a loaded build machine and for the ordinary unevenness of five sockets -- while
// still failing the case this test exists for, where a connection is served a small fraction of
// what its neighbours get or is not served at all.
constexpr double worst_acceptable_share_of_the_best = 0.5;

// Each connection's receive buffer, which is large enough that the reactor never has to stop
// reading a connection during the run. This test is about whether the reactor shares its time
// fairly between connections, and with a small buffer it measures something else.
//
// When a connection's buffer is three quarters full, the reactor stops reading that socket until
// the application thread has taken enough out (see RawBytesProtocolHandler). While it is not
// reading, the kernel's receive buffer for the socket fills and the receiving side tells the
// sender it has no room, which TCP calls advertising a zero window. The sender then waits for the
// receiver to say there is room again. If that message is not sent, the sender finds out only by
// probing on a timer that starts at 200ms and doubles each time it finds no room.
//
// With the 64 KiB buffer the other tests here use, each connection was stopped and restarted
// about 3,200 times a run, and in about one run in four two of the five senders were left waiting
// on that timer for 1.5 seconds while the other three finished. The kernel's own socket listing
// showed those two senders in the probing state, with the timer backed off twice. With this
// buffer the reactor never stops reading, all five finish together, and the whole test takes
// about 0.6 seconds instead of 2.6. The stall itself is recorded as BUG-0104. The FIX gateway
// gives each member session 16 MiB, so this size is the smaller of the two, not the larger.
constexpr int64_t fairness_raw_buffer_capacity = 4 * 1024 * 1024;

} // un-named namespace

/** @brief Counts raw bytes per connection, so that one starved connection is visible. */
class PerClientCountingThread : public ApplicationThread {
  public:
    PerClientCountingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "PerClientCountingThread", ThreadID{2}, make_queue_config(), make_allocator_config("PerClientPool"),
                            ApplicationThreadConfiguration{}) {}

    /** @brief Bytes taken from each connection, keyed by connection id. */
    [[nodiscard]] std::map<int, int64_t> bytes_per_connection() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return bytes_taken_;
    }

  protected:
    /*
     * The commit accounting is kept per connection rather than once for the thread. Each
     * connection has a MirroredBuffer of its own with positions of its own, so a single running
     * total across all of them would commit one connection's bytes against another's buffer.
     */
    void on_raw_socket_message(const EventMessage& message) override {
        const int connection = message.connection_id().get_value();
        const int64_t event_tail = message.tail_position();
        const auto event_bytes = static_cast<int64_t>(message.payload_size());
        const int64_t absolute_head_now = event_tail + event_bytes;

        const std::lock_guard<std::mutex> lock(mutex_);
        int64_t& head_seen = head_seen_[connection];
        int64_t& committed = committed_[connection];

        if (absolute_head_now > head_seen) {
            head_seen = absolute_head_now;
        }
        const int64_t to_commit = head_seen - committed;
        if (to_commit > 0) {
            bytes_taken_[connection] += to_commit;
            commit_raw_bytes(message.connection_id(), to_commit);
            committed = head_seen;
        }
    }

    void on_connection_established(ConnectionID) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(pubsub_itc_fw::TimerID) override {}

  private:
    mutable std::mutex mutex_;
    std::map<int, int64_t> bytes_taken_;
    std::map<int, int64_t> head_seen_;
    std::map<int, int64_t> committed_;
};

TEST_F(FrameworkPduBurstIntegrationTest, PollingReactorServesSeveralClientsEvenly) {
    const ServiceRegistry registry;
    auto reactor = std::make_unique<Reactor>(make_reactor_config(keep_polling_throughout, no_housekeeping_during_the_test), registry, logger_->logger);

    reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{2}, ProtocolType{ProtocolType::RawBytes},
                                       fairness_raw_buffer_capacity);

    auto counting_thread = ApplicationThread::create<PerClientCountingThread>(logger_->logger, *reactor);
    reactor->register_thread(counting_thread);

    std::thread reactor_thread([&]() { reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return reactor->is_initialized(); })) << "Reactor did not initialise within timeout";
    const uint16_t port = reactor->get_inbound_listener_port(0);
    ASSERT_NE(port, 0U) << "OS did not assign a valid listening port";

    // ----- Five clients, each with exactly the same amount to send -----
    //
    // The same amount each, rather than each sending as hard as it can for a fixed time. Those
    // sound alike and they are not. A client sending flat out can only send as fast as its
    // socket is drained, so a client that is being served badly also sends little, and a test
    // that measured bytes over a fixed time could not say which of those was the cause. Giving
    // every client an identical amount to deliver removes the question: the work offered is
    // equal by construction, so anything left over is a difference in how they were served.

    std::vector<std::thread> clients;
    std::vector<std::atomic<int64_t>> bytes_sent(fairness_client_count);
    std::vector<std::atomic<int64_t>> finished_after_ms(fairness_client_count);
    for (auto& counter : bytes_sent) {
        counter.store(0, std::memory_order_release);
    }
    for (auto& counter : finished_after_ms) {
        counter.store(0, std::memory_order_release);
    }

    const auto started_at = std::chrono::steady_clock::now();

    for (int client = 0; client < fairness_client_count; ++client) {
        clients.emplace_back([&, client]() {
            const int sock = connect_raw_socket(port);
            if (sock == -1) {
                ADD_FAILURE() << "client " << client << " could not connect";
                return;
            }
            const std::string payload(client_frame_payload_bytes, static_cast<char>('a' + client));
            const auto frame_bytes = static_cast<int64_t>(sizeof(uint32_t) + payload.size());
            int64_t delivered = 0;
            while (delivered < bytes_each_client_sends) {
                const uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
                if (!send_all(sock, &len_be, sizeof(len_be)) || !send_all(sock, payload.data(), payload.size())) {
                    break;
                }
                delivered += frame_bytes;
                bytes_sent[static_cast<size_t>(client)].store(delivered, std::memory_order_release);
            }
            finished_after_ms[static_cast<size_t>(client)].store(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count(), std::memory_order_release);
            ::close(sock);
        });
    }

    // Wait for every client to finish, or for the deadline. A client that cannot finish is the
    // finding, so the deadline has to be generous enough that slowness alone does not produce
    // one: five clients sending this much over the loopback interface is well under a second of
    // work when they are served evenly.
    const bool all_finished = wait_for(
        [&]() {
            for (const auto& counter : bytes_sent) {
                if (counter.load(std::memory_order_acquire) < bytes_each_client_sends) {
                    return false;
                }
            }
            return true;
        },
        static_cast<int>(client_fairness_deadline.count()));

    const auto finished_at = std::chrono::steady_clock::now();

    for (auto& client : clients) {
        client.join();
    }

    const std::map<int, int64_t> served = counting_thread->bytes_per_connection();
    shutdown_and_join(*reactor, reactor_thread);

    // ----- Assertions -----

    std::string per_client;
    for (int client = 0; client < fairness_client_count; ++client) {
        per_client += " client " + std::to_string(client) + " delivered " + std::to_string(bytes_sent[static_cast<size_t>(client)].load()) + " of " +
                      std::to_string(bytes_each_client_sends) + ";";
    }

    ASSERT_EQ(static_cast<int>(served.size()), fairness_client_count)
        << "only " << served.size() << " of " << fairness_client_count << " connections delivered any bytes at all";

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started_at);
    EXPECT_TRUE(all_finished) << "not every client got its " << bytes_each_client_sends << " bytes through within " << client_fairness_deadline.count()
                              << "ms, although all of them had exactly the same amount to send and were sending it at the same time. "
                              << "A client that cannot finish is one whose connection the reactor is not coming back to:" << per_client;

    if (all_finished) {
        // Everything got through in the end. The remaining question is whether some clients were
        // made to wait a great deal longer than others for it, which is what unfair service
        // looks like once every client has a finite amount to deliver: the same work, the same
        // starting moment, and very different finishing times.
        int64_t first_finished = finished_after_ms[0].load(std::memory_order_acquire);
        int64_t last_finished = first_finished;
        std::string finishing_times;
        for (int client = 0; client < fairness_client_count; ++client) {
            const int64_t when = finished_after_ms[static_cast<size_t>(client)].load(std::memory_order_acquire);
            first_finished = std::min(first_finished, when);
            last_finished = std::max(last_finished, when);
            finishing_times += " client " + std::to_string(client) + " finished after " + std::to_string(when) + "ms;";
        }

        ASSERT_GT(first_finished, 0) << "a client finished before it started, which means the timing is wrong rather than the reactor";

        const double spread = static_cast<double>(first_finished) / static_cast<double>(last_finished);
        EXPECT_GE(spread, worst_acceptable_share_of_the_best)
            << "the first client to finish took " << first_finished << "ms and the last took " << last_finished
            << "ms, for identical amounts of data sent from the same moment over five connections to one reactor. "
            << "A reactor coming back to every connection evenly finishes them at about the same time:" << finishing_times;
    }
}

// A burst of PDUs on a connection this process opened, to a peer that is not reading, must all be
// delivered, in order. When a send cannot be written at once, the reactor keeps it in a waiting slot
// until the socket drains, and must take no further command meanwhile: keeping a second waiting send
// in the same slot loses the first (docs/bug_list.md, BUG-0117). RawBytesProtocolHandlerIntegrationTest
// checks the same on a connection the process accepted.
//
// The peer is a plain socket owned by the test, not a reactor: a reactor starts reading the moment a
// connection is accepted, before its application thread could ask it to pause, and would take the whole
// burst into its own buffers so that the sender's socket never fills.
TEST_F(FrameworkPduBurstIntegrationTest, ABurstToAPeerThatIsNotReadingIsDeliveredWhole) {
    constexpr int paused_burst_size = 20000;

    // The peer: a listening socket with a small receive buffer, set before listening so that the
    // accepted connection has it too.
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(listen_fd, -1);
    const int small_buffer = 4096;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &small_buffer, sizeof(small_buffer));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listen_fd, 1), 0);
    socklen_t address_length = sizeof(address);
    ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &address_length), 0);
    const uint16_t peer_port = ntohs(address.sin_port);

    ServiceRegistry sender_registry;
    sender_registry.add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", peer_port}, NetworkEndpointConfiguration{});
    // A small send buffer as well, so the burst fills the connection almost at once.
    ReactorConfiguration sender_config = make_reactor_config();
    sender_config.socket_send_buffer_size = 4096;
    auto sender_reactor = std::make_unique<Reactor>(sender_config, sender_registry, logger_->logger);
    auto sender_thread = ApplicationThread::create<SenderThread>(logger_->logger, *sender_reactor, paused_burst_size);
    sender_reactor->register_thread(sender_thread);
    std::thread sender_reactor_thread([&]() { sender_reactor->run(); });
    set_watched_reactors(*sender_reactor, *sender_reactor);

    pollfd accepting{listen_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&accepting, 1, 5000), 1) << "the sender never connected";
    const int peer_fd = ::accept(listen_fd, nullptr, nullptr);
    ASSERT_NE(peer_fd, -1);

    // Not reading until the whole burst has been handed to the reactor, and a little longer, so the
    // reactor meets many sends while the connection is full.
    EXPECT_TRUE(wait_for([&]() { return sender_thread->burst_sent.load(std::memory_order_acquire); }))
        << "Sender: burst not sent: " << last_wait_failure_description();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Read everything, until nothing more arrives for two seconds, then split it into frames.
    std::vector<uint8_t> received;
    std::vector<uint8_t> chunk(65536);
    for (;;) {
        pollfd reading{peer_fd, POLLIN, 0};
        if (::poll(&reading, 1, 2000) <= 0) {
            break;
        }
        const ssize_t n = ::recv(peer_fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            break;
        }
        received.insert(received.end(), chunk.begin(), chunk.begin() + n);
    }
    ::close(peer_fd);
    ::close(listen_fd);
    shutdown_and_join(*sender_reactor, sender_reactor_thread);

    std::vector<int64_t> sequence_numbers;
    size_t offset = 0;
    while (offset + sizeof(PduHeader) <= received.size()) {
        PduHeader header{};
        std::memcpy(&header, received.data() + offset, sizeof(header));
        const size_t payload_bytes = ntohl(header.byte_count);
        if (offset + sizeof(PduHeader) + payload_bytes > received.size()) {
            break;
        }
        sequence_numbers.push_back(static_cast<int64_t>(be64toh(static_cast<uint64_t>(header.seq_no))));
        offset += sizeof(PduHeader) + payload_bytes;
    }
    EXPECT_EQ(static_cast<int>(sequence_numbers.size()), paused_burst_size) << "sends were lost while the peer was not reading";
    for (size_t index = 0; index < sequence_numbers.size(); ++index) {
        ASSERT_EQ(sequence_numbers[index], static_cast<int64_t>(index + 1))
            << "frame " << index << " carries sequence number " << sequence_numbers[index] << ": the ones between are missing";
    }
}

} // namespaces
