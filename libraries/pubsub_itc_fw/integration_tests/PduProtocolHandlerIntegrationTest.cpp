// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file PduProtocolHandlerIntegrationTest.cpp
 * @brief Integration test: large PDU transmission over loopback TCP.
 *
 * Exercises the partial-send path in PduProtocolHandler::continue_send().
 *
 * A DataQuery PDU is constructed with a query_name string of 2 MB. The Linux
 * loopback socket send buffer defaults to approximately 212 KB, so the first
 * write() call in PduFramer::send_prebuilt() will return EAGAIN well before
 * the full payload has been sent. This causes has_pending_send() to return
 * true, the Reactor to register EPOLLOUT, and subsequently to call
 * PduProtocolHandler::continue_send() one or more times to drain the
 * remainder — which is the code path this test is designed to cover.
 *
 * ApplicationThreadConfiguration::outbound_slab_size on the sending side is
 * set to 4 MB to accommodate the encoded PDU (2 MB string + framing overhead)
 * in a single slab chunk.
 *
 * ApplicationThreadConfiguration::inbound_decode_arena_size on the receiving
 * side is set to 3 MB to decode the large string field.
 *
 * Test structure mirrors VariableLengthPduTest:
 *   - One reactor (listener) registers an inbound listener on a
 *     dynamically-assigned loopback port (127.0.0.1:0).
 *   - A second reactor (connector) connects to that port.
 *   - The connector sends the large DataQuery on ConnectionEstablished.
 *   - The listener decodes it and verifies the query_name length and a sample
 *     of its content.
 *   - The listener sends a small DataResponse so the connector has a clean
 *     termination signal.
 *   - The connector disconnects on receipt of the response.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
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

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

#include <variable_length_test_protocol.hpp>

namespace pubsub_itc_fw::tests {

static constexpr int16_t PDU_ID_DATA_QUERY    = 300;
static constexpr int16_t PDU_ID_DATA_RESPONSE = 301;

// 2 MB query_name — guarantees the first write() cannot complete in one call
// on a loopback socket whose send buffer is ~212 KB, forcing continue_send().
static constexpr size_t kLargeStringBytes = 2 * 1024 * 1024;

// Outbound slab must hold the full encoded PDU in a single chunk.
// kLargeStringBytes + 4-byte string length prefix + PDU framing header + margin.
static constexpr size_t kOutboundSlabSize = 4 * 1024 * 1024;

// Inbound decode arena must accommodate the large string field.
static constexpr size_t kInboundDecodeArenaSize = 3 * 1024 * 1024;

// ============================================================
// Helpers
// ============================================================

static QueueConfiguration make_queue_config() {
    QueueConfiguration cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static AllocatorConfiguration make_allocator_config(const std::string& name) {
    AllocatorConfiguration cfg{};
    cfg.pool_name        = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

static ApplicationThreadConfiguration make_connector_thread_config() {
    ApplicationThreadConfiguration cfg{};
    cfg.outbound_slab_size        = kOutboundSlabSize;
    cfg.inbound_decode_arena_size = kInboundDecodeArenaSize;
    return cfg;
}

static ApplicationThreadConfiguration make_listener_thread_config() {
    ApplicationThreadConfiguration cfg{};
    // The listener receives the 2 MB PDU so it needs a large decode arena.
    // Its outbound reply is a small DataResponse; the default slab size is fine.
    cfg.inbound_decode_arena_size = kInboundDecodeArenaSize;
    return cfg;
}

// ============================================================
// Connector-side ApplicationThread.
// Sends a large DataQuery on ConnectionEstablished.
// Decodes the DataResponse and then disconnects cleanly.
// ============================================================
class PduProtocolHandlerConnectorThread : public ApplicationThread {
public:
    PduProtocolHandlerConnectorThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "PduProtocolHandlerConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("PduProtocolHandlerConnectorPool"),
                            make_connector_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_sent{false};
    std::atomic<bool> response_received{false};
    std::atomic<bool> connection_lost{false};

    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service("listener");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);

        // Build a 2 MB query_name to force the partial-send path.
        large_query_name_.assign(kLargeStringBytes, 'x');

        DataQuery query{};
        query.request_id  = 99;
        query.query_name  = std::string_view(large_query_name_);
        query.has_limit   = false;
        query.limit       = 0;

        send_pdu(id, PDU_ID_DATA_QUERY, query);
        query_sent.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_framework_pdu_message(const EventMessage& msg) override {
        const uint8_t* payload = msg.payload();
        const size_t   size    = static_cast<size_t>(msg.payload_size());

        DataResponseView response{};
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(response, payload, size, consumed, arena, arena_needed)) {
            response_received.store(true, std::memory_order_release);
        }

        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    std::string large_query_name_;
};

// ============================================================
// Listener-side ApplicationThread.
// Decodes the large DataQuery and verifies its content.
// Replies with a small DataResponse so the connector can exit cleanly.
// ============================================================
class PduProtocolHandlerListenerThread : public ApplicationThread {
public:
    PduProtocolHandlerListenerThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "PduProtocolHandlerListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("PduProtocolHandlerListenerPool"),
                            make_listener_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_received{false};
    std::atomic<bool> response_sent{false};
    std::atomic<bool> connection_lost{false};

    size_t  received_query_name_length{0};
    bool    received_query_name_all_x{false};
    int64_t received_request_id{0};

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

    void on_framework_pdu_message(const EventMessage& msg) override {
        const uint8_t* payload = msg.payload();
        const size_t   size    = static_cast<size_t>(msg.payload_size());

        DataQueryView query{};
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(query, payload, size, consumed, arena, arena_needed)) {
            received_request_id        = query.request_id;
            received_query_name_length = query.query_name.size();

            // Verify every character is 'x' without copying the 2 MB string.
            bool all_x = true;
            for (size_t i = 0; i < query.query_name.size(); ++i) {
                if (query.query_name[i] != 'x') {
                    all_x = false;
                    break;
                }
            }
            received_query_name_all_x = all_x;
            query_received.store(true, std::memory_order_release);

            // Send a minimal DataResponse so the connector has a clean signal
            // to disconnect rather than waiting on a timeout.
            std::array<std::string_view, 1> results_data = {"ok"};
            DataResponse response{};
            response.request_id  = query.request_id;
            response.status_code = 0;
            response.results.data = results_data.data();
            response.results.size = results_data.size();

            send_pdu(conn_id, PDU_ID_DATA_RESPONSE, response);
            response_sent.store(true, std::memory_order_release);
        }
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class PduProtocolHandlerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>("pdu_protocol_handler_integ_logger", "pdu_protocol_handler_integ_sink");
    }

    void TearDown() override {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 10000) {
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

    static ReactorConfiguration make_connector_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(2000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        return cfg;
    }

    static ReactorConfiguration make_listener_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(2000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        // Must accommodate the full 2 MB PDU payload in a single inbound slab chunk.
        cfg.inbound_slab_size          = kOutboundSlabSize;
        return cfg;
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: 2 MB DataQuery triggers continue_send on the sender,
// is fully reassembled by the receiver, and content is verified.
// ============================================================
TEST_F(PduProtocolHandlerIntegrationTest, LargeQueryNameForcesPartialsend) {
    // --- Listener side ---
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_listener_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = std::make_shared<PduProtocolHandlerListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    // --- Connector side ---
    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_connector_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = std::make_shared<PduProtocolHandlerConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    // --- Wait for each step of the exchange ---

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->query_sent.load(std::memory_order_acquire);
    })) << "Connector: large DataQuery not sent";

    // Allow generous time for the 2 MB PDU to be fully transmitted.
    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->query_received.load(std::memory_order_acquire);
    }, 15000)) << "Listener: large DataQuery not received within timeout";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->response_sent.load(std::memory_order_acquire);
    })) << "Listener: DataResponse not sent";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->response_received.load(std::memory_order_acquire);
    })) << "Connector: DataResponse not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Connector: ConnectionLost not received after disconnect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer disconnect";

    // --- Verify the received DataQuery fields ---
    EXPECT_EQ(listener_thread->received_request_id, 99);
    EXPECT_EQ(listener_thread->received_query_name_length, kLargeStringBytes);
    EXPECT_TRUE(listener_thread->received_query_name_all_x)
        << "Received query_name contained unexpected characters";

    // --- Shutdown both reactors ---
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
