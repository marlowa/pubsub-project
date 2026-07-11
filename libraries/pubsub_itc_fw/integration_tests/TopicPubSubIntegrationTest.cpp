// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TopicPubSubIntegrationTest.cpp
 * @brief Integration tests for the reusable topic pub/sub components.
 *
 * Exercises TopicPublisher and TopicSubscriberChannel end-to-end over loopback
 * TCP, with the publisher and subscriber each running on their own reactor in
 * their own thread -- i.e. the pub/sub functionality on its own, not through the
 * MEP/TAP applications.
 *
 * First test (SingleSubscriberReceivesReplayedRecordsInOrder):
 *   The publisher pre-populates a WAL with three records for topic "orders".
 *   A single subscriber subscribes from the oldest cursor, receives all three
 *   records (one TopicPage each) in seq_no order, and acks each page. The test
 *   verifies the records arrive in order with the correct payloads.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TopicPublisher.hpp>
#include <pubsub_itc_fw/TopicSubscriberChannel.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <topics.hpp>

using pubsub_itc_fw::tests::make_allocator_config;
using pubsub_itc_fw::tests::make_queue_config;

namespace pubsub_itc_fw {

static constexpr int16_t pdu_id_topic_subscribe_request = 107;
static constexpr int16_t pdu_id_topic_subscribe_ack = 108;
static constexpr int16_t pdu_id_topic_page = 109;
static constexpr int16_t pdu_id_topic_ack = 110;
static constexpr int16_t pdu_id_topic_not_leader = 111;

static constexpr int16_t pdu_id_nos = 1000; // NewOrderSingle -> topic "orders"
static constexpr int16_t pdu_id_ocr = 1001; // OrderCancelRequest -> topic "orders"

static constexpr size_t wal_segment_size = 4096;

// ============================================================
// Publisher-side ApplicationThread: owns a TopicPublisher for
// the "orders" topic and forwards the topic protocol to it.
// ============================================================
class TopicPublisherThread : public ApplicationThread, public TopicPublisherHost {
  public:
    TopicPublisherThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string wal_dir)
        : ApplicationThread(token, logger, reactor, "TopicPublisherThread", ThreadID{2}, make_queue_config(), make_allocator_config("TopicPubPool"),
                            ApplicationThreadConfiguration{})
        , publisher_(*this, "orders", [](int16_t pdu_id) { return pdu_id == pdu_id_nos || pdu_id == pdu_id_ocr; }, std::move(wal_dir)) {}

    // TopicPublisherHost
    void topic_send_subscribe_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) override {
        send_pdu(connection_id, pdu_id_topic_subscribe_ack, 0, ack);
    }
    void topic_send_page(ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) override {
        send_pdu(connection_id, pdu_id_topic_page, seq_no, page);
    }
    void topic_send_not_leader(ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) override {
        send_pdu(connection_id, pdu_id_topic_not_leader, 0, not_leader);
    }
    void topic_disconnect(ConnectionID connection_id) override {
        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = connection_id;
        get_reactor().enqueue_control_command(cmd);
    }

  protected:
    void on_connection_lost(const ConnectionID& id, const std::string&) override {
        publisher_.on_connection_lost(id);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const ConnectionID connection_id = message.connection_id();
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (message.pdu_id() == pdu_id_topic_subscribe_request) {
            pubsub_itc_fw_app::TopicSubscribeRequestView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            publisher_.on_subscribe_request(connection_id, view);
        } else if (message.pdu_id() == pdu_id_topic_ack) {
            pubsub_itc_fw_app::TopicAckView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            publisher_.on_ack(connection_id, view);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    TopicPublisher publisher_;
};

// ============================================================
// Subscriber-side ApplicationThread: owns a TopicSubscriberChannel
// and delivers received records into a vector for the test to check.
// ============================================================
class TopicSubscriberThread : public ApplicationThread, public TopicSubscriberChannelHost {
  public:
    struct ReceivedRecord {
        int64_t seq_no;
        int16_t pdu_id;
        std::vector<uint8_t> payload;
    };

    std::atomic<bool> all_records_received{false};
    std::atomic<int> records_received{0};
    std::vector<ReceivedRecord> received_records;

    TopicSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int expected_record_count)
        : ApplicationThread(token, logger, reactor, "TopicSubscriberThread", ThreadID{1}, make_queue_config(), make_allocator_config("TopicSubPool"),
                            ApplicationThreadConfiguration{})
        , expected_record_count_(expected_record_count)
        , channel_(*this, "test_subscriber", "orders", 0, [this](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) {
            received_records.push_back({seq_no, pdu_id, {payload, payload + payload_size}});
            const int count = records_received.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (count == expected_record_count_) {
                all_records_received.store(true, std::memory_order_release);
                ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
                cmd.connection_id_ = connection_id_;
                get_reactor().enqueue_control_command(cmd);
            }
        }) {}

    // TopicSubscriberChannelHost
    void topic_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) override {
        send_pdu(connection_id, pdu_id_topic_subscribe_request, 0, request);
    }
    void topic_send_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAck& ack) override {
        send_pdu(connection_id, pdu_id_topic_ack, 0, ack);
    }

  protected:
    void on_initial_event() override {
        connect_to_service("publisher");
    }

    void on_connection_established(ConnectionID id) override {
        connection_id_ = id;
        channel_.on_connected(id);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        shutdown("connection lost");
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (message.pdu_id() == pdu_id_topic_subscribe_ack) {
            pubsub_itc_fw_app::TopicSubscribeAckView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            channel_.on_subscribe_ack(view);
        } else if (message.pdu_id() == pdu_id_topic_page) {
            channel_.on_page(payload, size, arena);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    int expected_record_count_;
    ConnectionID connection_id_;
    TopicSubscriberChannel channel_;
};

// ============================================================
// Test fixture
// ============================================================
class TopicPubSubTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        std::string tmpl = "/dev/shm/topic_pubsub_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        wal_dir_ = tmpl;
    }

    void TearDown() override {
        std::filesystem::remove_all(wal_dir_);
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

    // Write `count` WAL records for the "orders" topic, in the on-disk framing the
    // publisher expects: [wall_time_ns : int64][pdu_id : int16][payload : uint32].
    void write_topic_wal_records(int count) {
        WalWriter writer;
        writer.open(wal_dir_, wal_segment_size, {0, 0});
        for (int i = 1; i <= count; ++i) {
            std::vector<uint8_t> buffer(TopicPublisher::wal_record_header_size + sizeof(uint32_t));
            const int64_t wall_time_ns = static_cast<int64_t>(i) * 1000;
            const int16_t pdu_id = pdu_id_nos;
            const uint32_t payload = static_cast<uint32_t>(i * 100);
            std::memcpy(buffer.data(), &wall_time_ns, sizeof(wall_time_ns));
            std::memcpy(buffer.data() + sizeof(int64_t), &pdu_id, sizeof(pdu_id));
            std::memcpy(buffer.data() + TopicPublisher::wal_record_header_size, &payload, sizeof(payload));
            writer.append(static_cast<int64_t>(i), buffer.data(), buffer.size());
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::string wal_dir_;
};

// ============================================================
// Test 1: one publisher, one subscriber, three replayed records
// received in seq_no order with correct payloads.
// ============================================================
TEST_F(TopicPubSubTest, SingleSubscriberReceivesReplayedRecordsInOrder) {
    static constexpr int record_count = 3;
    write_topic_wal_records(record_count);

    // --- Publisher side ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);

    std::thread publisher_reactor_thread([&]() { publisher_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";

    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Subscriber side ---
    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});

    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<TopicSubscriberThread>(logger_->logger, *subscriber_reactor, record_count);
    subscriber_reactor->register_thread(subscriber_thread);

    std::thread subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // --- Wait for all records to arrive ---
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); }))
        << "Subscriber did not receive all TopicPage records";

    // --- Verify records received in order with correct field values ---
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        const auto& rec = subscriber_thread->received_records[static_cast<size_t>(i)];
        EXPECT_EQ(rec.seq_no, i + 1) << "Wrong seq_no at index " << i;
        EXPECT_EQ(rec.pdu_id, pdu_id_nos) << "Wrong pdu_id at index " << i;
        ASSERT_EQ(rec.payload.size(), sizeof(uint32_t));
        uint32_t value{};
        std::memcpy(&value, rec.payload.data(), sizeof(value));
        EXPECT_EQ(value, static_cast<uint32_t>((i + 1) * 100)) << "Wrong payload at index " << i;
    }

    // --- Shutdown ---
    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");

    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

} // namespaces
