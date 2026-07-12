// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file WritableNotificationIntegrationTest.cpp
 * @brief Integration test for the request_writable_notification / on_connection_writable
 *        send-pacing primitive.
 *
 * A sender ApplicationThread streams N PDUs to a deliberately slow receiver over
 * loopback TCP, sending the next PDU only in response to on_connection_writable().
 * It never queues more than one frame ahead. The receiver sleeps briefly per PDU so
 * the sender's socket fills and the writable notification is satisfied via EPOLLOUT
 * (the backpressure path), not just immediately.
 *
 * Verifies: every record is delivered in order despite the slow reader, and the
 * writable callback is what drives the stream (fired once per record after the first).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
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
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <leader_follower.hpp>

using pubsub_itc_fw::tests::make_allocator_config;
using pubsub_itc_fw::tests::make_queue_config;

namespace pubsub_itc_fw {

// Any PDU id both sides agree on; the record just carries a sequence number.
static constexpr int16_t pdu_id_stream_record = 104;

// Sender: listens for a connection, then streams record_count
// WalAck PDUs, paced entirely by on_connection_writable().
class StreamingSenderThread : public ApplicationThread {
  public:
    std::atomic<bool> all_sent{false};
    std::atomic<int> writable_callbacks{0};

    StreamingSenderThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int record_count)
        : ApplicationThread(token, logger, reactor, "StreamingSenderThread", ThreadID{2}, make_queue_config(), make_allocator_config("SenderPool"),
                            ApplicationThreadConfiguration{})
        , record_count_(record_count) {}

  protected:
    void on_connection_established(ConnectionID id) override {
        connection_id_ = id;
        send_next(); // sends record 1 and arms the writable notification
    }

    void on_connection_writable(ConnectionID) override {
        writable_callbacks.fetch_add(1, std::memory_order_acq_rel);
        send_next();
    }

    void on_framework_pdu_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}

  private:
    void send_next() {
        pubsub_itc_fw_app::WalAck record{};
        record.seq_no = next_seq_no_;
        send_pdu(connection_id_, pdu_id_stream_record, next_seq_no_, record);
        ++next_seq_no_;
        if (next_seq_no_ <= record_count_) {
            request_writable_notification(connection_id_); // re-arm: more to send
        } else {
            all_sent.store(true, std::memory_order_release);
        }
    }

    int record_count_;
    int64_t next_seq_no_ = 1;
    ConnectionID connection_id_;
};

// Receiver: connects to the sender and consumes records, but
// dawdles per record so the sender's socket fills (forcing the
// EPOLLOUT path of the writable notification).
class SlowReceiverThread : public ApplicationThread {
  public:
    std::atomic<bool> all_received{false};
    std::vector<int64_t> received_seq_nos;

    SlowReceiverThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int expected_count)
        : ApplicationThread(token, logger, reactor, "SlowReceiverThread", ThreadID{1}, make_queue_config(), make_allocator_config("ReceiverPool"),
                            ApplicationThreadConfiguration{})
        , expected_count_(expected_count) {}

  protected:
    void on_initial_event() override {
        connect_to_service("sender");
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connect failed: " + reason);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;
        pubsub_itc_fw_app::WalAckView view{};
        if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
            return;
        }
        received_seq_nos.push_back(view.seq_no);
        std::this_thread::sleep_for(std::chrono::microseconds(200)); // be slow -> back the sender's socket up
        if (static_cast<int>(received_seq_nos.size()) == expected_count_) {
            all_received.store(true, std::memory_order_release);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    int expected_count_;
};

class WritableNotificationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }
    void TearDown() override {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    static ReactorConfiguration make_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
        cfg.connect_timeout = std::chrono::milliseconds(2000);
        return cfg;
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// Streaming a lot to a slow reader delivers everything in order,
// driven by the writable notification.
TEST_F(WritableNotificationTest, PacedStreamDeliversAllRecordsInOrder) {
    static constexpr int record_count = 100;

    // --- Sender (listener) ---
    const ServiceRegistry sender_registry;
    auto sender_reactor = std::make_unique<Reactor>(make_reactor_config(), sender_registry, logger_->logger);
    sender_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto sender_thread = ApplicationThread::create<StreamingSenderThread>(logger_->logger, *sender_reactor, record_count);
    sender_reactor->register_thread(sender_thread);
    ThreadWithJoinTimeout sender_reactor_thread([&]() { sender_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return sender_reactor->is_initialized(); })) << "Sender reactor did not initialize";
    const uint16_t listen_port = sender_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Receiver (connects) ---
    ServiceRegistry receiver_registry;
    receiver_registry.add("sender", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto receiver_reactor = std::make_unique<Reactor>(make_reactor_config(), receiver_registry, logger_->logger);
    auto receiver_thread = ApplicationThread::create<SlowReceiverThread>(logger_->logger, *receiver_reactor, record_count);
    receiver_reactor->register_thread(receiver_thread);
    ThreadWithJoinTimeout receiver_reactor_thread([&]() { receiver_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return receiver_thread->all_received.load(std::memory_order_acquire); })) << "Receiver did not receive all records";

    // --- Every record delivered, in order, despite the slow reader ---
    ASSERT_EQ(static_cast<int>(receiver_thread->received_seq_nos.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        EXPECT_EQ(receiver_thread->received_seq_nos[static_cast<size_t>(i)], i + 1) << "Out of order at index " << i;
    }

    // --- The writable callback drove the stream: one per record after the first ---
    EXPECT_TRUE(wait_for([&]() { return sender_thread->all_sent.load(std::memory_order_acquire); })) << "Sender did not finish";
    EXPECT_EQ(sender_thread->writable_callbacks.load(std::memory_order_acquire), record_count - 1);

    receiver_reactor->shutdown("test complete");
    sender_reactor->shutdown("test complete");
    if (receiver_reactor_thread.joinable()) {
        receiver_reactor_thread.join();
    }
    if (sender_reactor_thread.joinable()) {
        sender_reactor_thread.join();
    }
}

} // namespaces
