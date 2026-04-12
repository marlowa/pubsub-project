// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file RawBytesProtocolHandlerTest.cpp
 * @brief Integration test: raw byte stream exchange over loopback TCP.
 *
 * Exercises the full RawBytesProtocolHandler path end-to-end:
 *   - One reactor (the listener side) registers an inbound raw-bytes listener
 *     on a dynamically-assigned loopback port (127.0.0.1:0).
 *   - A second reactor (the connector side) connects to that port using a
 *     standard outbound connection.
 *   - Both sides exchange messages using a simple test protocol: each message
 *     is a 4-byte big-endian length prefix followed by that many payload bytes.
 *     This protocol is entirely independent of the framework's PDU framing.
 *   - The connector sends a request message on ConnectionEstablished.
 *   - The listener receives it via on_raw_socket_message(), decodes the length
 *     prefix, copies the payload, calls commit_raw_bytes() to release the
 *     buffer, and sends a reply using send_raw().
 *   - The connector receives the reply, decodes it, verifies the payload, and
 *     disconnects cleanly.
 *   - ConnectionLost is verified on both sides.
 *
 * Both reactors run on separate std::threads simultaneously.
 *
 * Test protocol framing:
 *   [ uint32_t length (big-endian) ][ payload bytes ]
 *
 * This is deliberately distinct from the framework's PduHeader to make clear
 * that RawBytesProtocolHandler places no constraints on byte stream structure.
 */

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfig.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Test protocol constants
// ============================================================

static const std::string request_payload  = "HELLO_RAW_SERVER";
static const std::string response_payload = "HELLO_RAW_CLIENT";

static constexpr int64_t raw_buffer_capacity = 65536; // 64KB MirroredBuffer
static constexpr std::size_t length_prefix_size = sizeof(uint32_t);

// ============================================================
// Test protocol helpers
// ============================================================

/*
 * Encodes a message as [ uint32_t length (big-endian) ][ payload bytes ]
 * and sends it via send_raw().
 */
static void send_framed(ApplicationThread& thread, ConnectionID conn_id,
                        const std::string& payload) {
    const uint32_t length = static_cast<uint32_t>(payload.size());
    const std::size_t total = length_prefix_size + payload.size();

    std::string frame(total, '\0');
    const uint32_t length_be = htonl(length);
    std::memcpy(frame.data(), &length_be, length_prefix_size);
    std::memcpy(frame.data() + length_prefix_size, payload.data(), payload.size());

    thread.send_raw(conn_id, frame.data(), static_cast<uint32_t>(total));
}

/*
 * Attempts to decode a length-prefixed message from a raw byte view.
 * Returns the decoded payload string on success, or an empty string if
 * insufficient bytes are available. On success, sets bytes_consumed to
 * the total number of bytes to commit (length prefix + payload).
 */
static std::string try_decode_framed(const uint8_t* data, int available,
                                     int64_t& bytes_consumed) {
    bytes_consumed = 0;
    if (available < static_cast<int>(length_prefix_size)) {
        return {};
    }
    uint32_t length_be = 0;
    std::memcpy(&length_be, data, length_prefix_size);
    const uint32_t payload_length = ntohl(length_be);
    const int64_t total = static_cast<int64_t>(length_prefix_size + payload_length);
    if (available < static_cast<int>(total)) {
        return {};
    }
    std::string payload(reinterpret_cast<const char*>(data + length_prefix_size), payload_length);
    bytes_consumed = total;
    return payload;
}

// ============================================================
// Helpers
// ============================================================

static QueueConfig make_queue_config() {
    QueueConfig cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static AllocatorConfig make_allocator_config(const std::string& name) {
    AllocatorConfig cfg{};
    cfg.pool_name        = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

// ============================================================
// Listener-side ApplicationThread.
// Receives a length-prefixed request, sends a length-prefixed reply.
// ============================================================
class RawListenerThread : public ApplicationThread {
public:
    RawListenerThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "RawListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("RawListenerPool"),
                            ApplicationThreadConfig{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> message_received{false};
    std::atomic<bool> reply_sent{false};
    std::atomic<bool> connection_lost{false};

    std::string received_payload;
    ConnectionID conn_id{};

protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("peer disconnected");
    }

    void on_raw_socket_message(const EventMessage& message) override {
        const uint8_t* data = message.payload();
        const int available = message.payload_size();

        if (data == nullptr || available <= 0) {
            return;
        }

        int64_t bytes_consumed = 0;
        std::string payload = try_decode_framed(data, available, bytes_consumed);
        if (bytes_consumed == 0) {
            return; // incomplete message — wait for more data
        }

        received_payload = payload;
        message_received.store(true, std::memory_order_release);

        // Release the bytes we have consumed from the MirroredBuffer.
        commit_raw_bytes(conn_id, bytes_consumed);

        // Send the reply.
        send_framed(*this, conn_id, response_payload);
        reply_sent.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Connector-side ApplicationThread.
// Connects, sends a length-prefixed request, receives a reply, disconnects.
// ============================================================
class RawConnectorThread : public ApplicationThread {
public:
    RawConnectorThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "RawConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("RawConnectorPool"),
                            ApplicationThreadConfig{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> request_sent{false};
    std::atomic<bool> reply_received{false};
    std::atomic<bool> connection_lost{false};

    std::string received_payload;
    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service("raw_listener");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
        send_framed(*this, conn_id, request_payload);
        request_sent.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_raw_socket_message(const EventMessage& message) override {
        const uint8_t* data = message.payload();
        const int available = message.payload_size();

        if (data == nullptr || available <= 0) {
            return;
        }

        int64_t bytes_consumed = 0;
        std::string payload = try_decode_framed(data, available, bytes_consumed);
        if (bytes_consumed == 0) {
            return; // incomplete message — wait for more data
        }

        received_payload = payload;
        reply_received.store(true, std::memory_order_release);

        commit_raw_bytes(conn_id, bytes_consumed);

        // Disconnect now that we have the reply.
        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class RawBytesProtocolHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>("raw_integ_logger", "raw_integ_sink");
    }

    void TearDown() override {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
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
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        return cfg;
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: raw byte round-trip with length-prefix framing
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, RawByteRoundTrip) {
    // --- Listener side ---
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0},
        ThreadID{2},
        ProtocolType{ProtocolType::RawBytes},
        raw_buffer_capacity);

    auto listener_thread = std::make_shared<RawListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    // --- Connector side ---
    ServiceRegistry connector_registry;
    connector_registry.add("raw_listener",
        NetworkEndpointConfig{"127.0.0.1", listen_port},
        NetworkEndpointConfig{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = std::make_shared<RawConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    // --- Wait for each step ---

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->request_sent.load(std::memory_order_acquire);
    })) << "Connector: request not sent";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->message_received.load(std::memory_order_acquire);
    })) << "Listener: request not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->reply_sent.load(std::memory_order_acquire);
    })) << "Listener: reply not sent";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->reply_received.load(std::memory_order_acquire);
    })) << "Connector: reply not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Connector: ConnectionLost not received after disconnect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer disconnect";

    // --- Verify payload content ---
    EXPECT_EQ(listener_thread->received_payload,  request_payload);
    EXPECT_EQ(connector_thread->received_payload, response_payload);

    // --- Shutdown ---
    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) {
        connector_reactor_thread.join();
    }
    if (listener_reactor_thread.joinable()) {
        listener_reactor_thread.join();
    }
}

} // namespace pubsub_itc_fw::tests
