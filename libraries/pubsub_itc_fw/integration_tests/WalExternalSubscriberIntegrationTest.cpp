// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file WalExternalSubscriberIntegrationTest.cpp
 * @brief Integration tests for the external WAL subscriber protocol.
 *
 * Exercises the WalSubscribeRequest / WalSubscribeAck handshake and
 * WalRecord streaming end-to-end over loopback TCP.
 *
 * Two tests:
 *
 *   WalSubscribeHandshake
 *     Subscriber connects and sends WalSubscribeRequest. Publisher replies
 *     with WalSubscribeAck. No WAL records exist; the test verifies only
 *     the handshake fields.
 *
 *   WalSubscribeAndStreamRecords
 *     Publisher pre-populates a WAL with three records. Subscriber
 *     subscribes, receives all three WalRecord PDUs in seq_no order, and
 *     sends WalAck after each. The test verifies the records and confirms
 *     the publisher's cursor reflects the final ack.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
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
#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <leader_follower.hpp>

using pubsub_itc_fw::tests::make_allocator_config;
using pubsub_itc_fw::tests::make_queue_config;

namespace pubsub_itc_fw {

using pubsub_itc_fw_app::decode;
using pubsub_itc_fw_app::WalAck;
using pubsub_itc_fw_app::WalAckView;
using pubsub_itc_fw_app::WalRecord;
using pubsub_itc_fw_app::WalRecordView;
using pubsub_itc_fw_app::WalSubscribeAck;
using pubsub_itc_fw_app::WalSubscribeAckView;
using pubsub_itc_fw_app::WalSubscribeRequest;
using pubsub_itc_fw_app::WalSubscribeRequestView;

static constexpr int16_t pdu_id_wal_record = 103;
static constexpr int16_t pdu_id_wal_ack = 104;
static constexpr int16_t pdu_id_wal_subscribe_request = 105;
static constexpr int16_t pdu_id_wal_subscribe_ack = 106;

static constexpr size_t wal_segment_size = 4096;

// ============================================================
// Publisher-side ApplicationThread.
//
// Listens for external WAL subscriber connections.
// On WalSubscribeRequest: replies with WalSubscribeAck, then
// replays wal_dir_ and sends each entry as a WalRecord PDU.
// On WalAck: updates ExternalWalSubscriberRegistry.
// ============================================================
class WalPublisherThread : public ApplicationThread {
  public:
    WalPublisherThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor,
                       std::string wal_dir)
        : ApplicationThread(token, logger, reactor, "WalPublisherThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("PublisherPool"),
                            ApplicationThreadConfiguration{})
        , wal_dir_(std::move(wal_dir)) {}

    std::atomic<bool> subscriber_connected{false};
    std::atomic<bool> subscribe_request_received{false};
    std::atomic<bool> subscribe_ack_sent{false};
    std::atomic<bool> all_records_streamed{false};
    std::atomic<int> wal_acks_received{0};
    std::atomic<int64_t> last_acked_seq_no{-1};

    int64_t accepted_from_seq_no{-1};

  protected:
    void on_connection_established(ConnectionID id) override {
        subscriber_conn_id_ = id;
        registry_.register_subscriber(id, "test_subscriber", 0);
        subscriber_connected.store(true, std::memory_order_release);
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        shutdown("subscriber disconnected");
    }

    void on_framework_pdu_message(const EventMessage& msg) override {
        const uint8_t* payload = msg.payload();
        const size_t size = static_cast<size_t>(msg.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (msg.pdu_id() == pdu_id_wal_subscribe_request) {
            WalSubscribeRequestView req_view{};
            if (!decode(req_view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            subscribe_request_received.store(true, std::memory_order_release);

            WalSubscribeAck ack{};
            ack.accepted_from_seq_no = req_view.from_seq_no;
            accepted_from_seq_no = req_view.from_seq_no;
            send_pdu(subscriber_conn_id_, pdu_id_wal_subscribe_ack, 0, ack);
            subscribe_ack_sent.store(true, std::memory_order_release);

            // Replay existing WAL records and stream each as a WalRecord PDU.
            [[maybe_unused]] auto end_position = WalReader::replay(wal_dir_, {0, 0},
                [this](int64_t id, const void* data, size_t record_size) {
                    WalRecord record{};
                    record.seq_no = id;
                    record.pdu_id = 1000;
                    record.payload.data = static_cast<const uint8_t*>(data);
                    record.payload.size = record_size;
                    record.wall_time_ns = 0;
                    send_pdu(subscriber_conn_id_, pdu_id_wal_record, id, record);
                });

            all_records_streamed.store(true, std::memory_order_release);
        } else if (msg.pdu_id() == pdu_id_wal_ack) {
            WalAckView ack_view{};
            if (!decode(ack_view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            registry_.update_cursor(subscriber_conn_id_, ack_view.seq_no);
            last_acked_seq_no.store(ack_view.seq_no, std::memory_order_release);
            wal_acks_received.fetch_add(1, std::memory_order_release);
        }
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    std::string wal_dir_;
    ConnectionID subscriber_conn_id_;
    ExternalWalSubscriberRegistry registry_;
};

// ============================================================
// Subscriber-side ApplicationThread.
//
// Connects to the publisher. Sends WalSubscribeRequest on
// ConnectionEstablished. Receives WalSubscribeAck and WalRecord
// PDUs. Sends WalAck after each WalRecord. Disconnects once
// expected_record_count records have been received.
// ============================================================
class WalSubscriberThread : public ApplicationThread {
  public:
    WalSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor,
                        int expected_record_count)
        : ApplicationThread(token, logger, reactor, "WalSubscriberThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("SubscriberPool"),
                            ApplicationThreadConfiguration{})
        , expected_record_count_(expected_record_count) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> subscribe_ack_received{false};
    std::atomic<bool> all_records_received{false};
    std::atomic<int64_t> accepted_from_seq_no{-1};
    std::atomic<int> records_received{0};

    struct ReceivedRecord {
        int64_t seq_no;
        int16_t pdu_id;
        std::vector<uint8_t> payload;
    };
    std::vector<ReceivedRecord> received_records;

  protected:
    void on_initial_event() override {
        connect_to_service("publisher");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id_ = id;
        connection_established.store(true, std::memory_order_release);

        const std::string sub_id = "test_subscriber";
        WalSubscribeRequest req{};
        req.subscriber_id = sub_id;
        req.from_seq_no = 0;
        send_pdu(id, pdu_id_wal_subscribe_request, 0, req);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        shutdown("connection lost");
    }

    void on_framework_pdu_message(const EventMessage& msg) override {
        const uint8_t* payload = msg.payload();
        const size_t size = static_cast<size_t>(msg.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (msg.pdu_id() == pdu_id_wal_subscribe_ack) {
            WalSubscribeAckView ack_view{};
            if (!decode(ack_view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            accepted_from_seq_no.store(ack_view.accepted_from_seq_no, std::memory_order_release);
            subscribe_ack_received.store(true, std::memory_order_release);
        } else if (msg.pdu_id() == pdu_id_wal_record) {
            WalRecordView record_view{};
            if (!decode(record_view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            const uint8_t* pdata = record_view.payload.data;
            received_records.push_back({
                record_view.seq_no,
                record_view.pdu_id,
                {pdata, pdata + record_view.payload.size}
            });
            const int count = records_received.fetch_add(1, std::memory_order_acq_rel) + 1;

            WalAck ack{};
            ack.seq_no = record_view.seq_no;
            send_pdu(conn_id_, pdu_id_wal_ack, 0, ack);

            if (count == expected_record_count_) {
                all_records_received.store(true, std::memory_order_release);
                ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
                cmd.connection_id_ = conn_id_;
                get_reactor().enqueue_control_command(cmd);
            }
        }
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    int expected_record_count_;
    ConnectionID conn_id_;
};

// ============================================================
// Test fixture
// ============================================================
class WalExternalSubscriberTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        std::string tmpl = "/dev/shm/wal_sub_test_XXXXXX";
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

    void write_wal_records(int count) {
        WalWriter writer;
        writer.open(wal_dir_, wal_segment_size, {0, 0});
        for (int i = 1; i <= count; ++i) {
            const uint32_t payload = static_cast<uint32_t>(i * 100);
            writer.append(static_cast<int64_t>(i), &payload, sizeof(payload));
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::string wal_dir_;
};

// ============================================================
// Test 1: WalSubscribeRequest / WalSubscribeAck handshake only.
// No WAL records exist; verifies the handshake fields.
// ============================================================
TEST_F(WalExternalSubscriberTest, WalSubscribeHandshake) {
    // --- Publisher side ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto publisher_thread = ApplicationThread::create<WalPublisherThread>(
        logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);

    std::thread publisher_reactor_thread([&]() { publisher_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); }))
        << "Publisher reactor did not initialize";

    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Subscriber side ---
    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<WalSubscriberThread>(
        logger_->logger, *subscriber_reactor, 0);
    subscriber_reactor->register_thread(subscriber_thread);

    std::thread subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // --- Wait for handshake ---
    EXPECT_TRUE(wait_for([&]() { return publisher_thread->subscribe_request_received.load(std::memory_order_acquire); }))
        << "Publisher: WalSubscribeRequest not received";

    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->subscribe_ack_received.load(std::memory_order_acquire); }))
        << "Subscriber: WalSubscribeAck not received";

    // --- Verify fields ---
    EXPECT_EQ(subscriber_thread->accepted_from_seq_no.load(std::memory_order_acquire), 0);
    EXPECT_EQ(publisher_thread->accepted_from_seq_no, 0);

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

// ============================================================
// Test 2: WAL records streamed and acked after subscribe.
// Publisher pre-populates WAL with 3 records. Subscriber
// receives all three in order and acks each. Verifies payload
// values and that publisher cursor advances to seq_no 3.
// ============================================================
TEST_F(WalExternalSubscriberTest, WalSubscribeAndStreamRecords) {
    static constexpr int record_count = 3;
    write_wal_records(record_count);

    // --- Publisher side ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto publisher_thread = ApplicationThread::create<WalPublisherThread>(
        logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);

    std::thread publisher_reactor_thread([&]() { publisher_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); }))
        << "Publisher reactor did not initialize";

    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Subscriber side ---
    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<WalSubscriberThread>(
        logger_->logger, *subscriber_reactor, record_count);
    subscriber_reactor->register_thread(subscriber_thread);

    std::thread subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // --- Wait for all records to arrive and be acked ---
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); }))
        << "Subscriber: did not receive all WalRecord PDUs";

    EXPECT_TRUE(wait_for([&]() {
        return publisher_thread->wal_acks_received.load(std::memory_order_acquire) == record_count;
    })) << "Publisher: did not receive all WalAck PDUs";

    // --- Verify records received in order with correct field values ---
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);

    for (int i = 0; i < record_count; ++i) {
        const auto& rec = subscriber_thread->received_records[static_cast<size_t>(i)];
        EXPECT_EQ(rec.seq_no, i + 1) << "Wrong seq_no at index " << i;
        EXPECT_EQ(rec.pdu_id, 1000) << "Wrong pdu_id at index " << i;
        ASSERT_EQ(rec.payload.size(), sizeof(uint32_t));
        uint32_t val{};
        std::memcpy(&val, rec.payload.data(), sizeof(val));
        EXPECT_EQ(val, static_cast<uint32_t>((i + 1) * 100)) << "Wrong payload at index " << i;
    }

    // --- Verify publisher cursor advanced to the last acked seq_no ---
    EXPECT_EQ(publisher_thread->last_acked_seq_no.load(std::memory_order_acquire), record_count);

    // --- Shutdown ---
    publisher_reactor->shutdown("test complete");

    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

} // namespaces
